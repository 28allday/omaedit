#pragma once

#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <memory>
#include "workingframe.h"
#include "mediaprobe.h"

class Timeline;
class NativeVideo;
namespace AudioSampleSource { class Reader; }
namespace render { class MediaSource; }
class QFile;
class QTemporaryDir;

// A media file opened for its pictures and sound: what it is (media::probe,
// libavformat), its pictures (NativeVideo, through the colour pipeline) and its
// sound (AudioSampleSource). A project file (ProjectFile) opens as its
// timeline. No Qt object machinery here — Backend owns one of these and
// decides when frames are pulled.
class MediaPlayer {
public:
    MediaPlayer();
    ~MediaPlayer();

    bool open(const QString &path);
    void setInputTransform(const QVariantMap &settings);
    void setWorkingManagement(const QVariantMap &management) { m_workingManagement=management; }
    // The source viewer's pictures made by the engine: the file alone,
    // untouched, on a timeline of its own shape (its size, rate and pixel
    // shape, `project`'s colour settings, `sources`, the project's media
    // settings, and `mode`, its playback choice of proxies) with the input
    // colour space setWorkingManagement gave, so a clip looks in the source
    // viewer as it will once it is cut in, and is decoded and drawn by what
    // draws the timeline. `original` is the clip's path in the project
    // (open() may have been given its proxy). Positions stay the file's own
    // pictures. What the engine does not draw stays with the native decoder:
    // sensor (Bayer) recordings, which are developed there; a generated
    // colour; a project; a file with no colour management given; and any
    // file whose first picture the engine fails to make where the native
    // decoder makes it. OMAEDIT_SOURCE_VIEWER=native keeps everything there.
    //
    // Threads: the player, this timeline, its engine and its decoders belong
    // to the one thread that uses the player (the worker's, for the source
    // viewer); the engine is one of the program's on the shared device,
    // taking its turn on the queues with the timeline's.
    void drawOnEngine(const QString &original, const QVariantMap &project, const QVariantMap &sources,
                      const QString &mode);
    // How the engine draws the pictures asked for from here on: as a viewer
    // shows them (Timeline::setDrawSize and setShownAs: at `size`, the stored
    // pixels the viewer shows the picture at, and for `display`, with nothing
    // left to convert and no working frame read back), or, with neither, as
    // working frames. The native decoder's pictures are always working frames.
    void setViewerDraw(const QSize &size, const QVariant &display) { m_drawSize = size; m_shownAs = display; }
    // Whether the engine made the last picture (for tests).
    bool engineDrawn() const { return m_engineDrew; }
    // The engine's CPU twin instead of the GPU (for tests).
    void setEngineOnCpu(bool cpu);
    void close();
    bool isOpen() const { return m_document != nullptr || m_facts.valid; }

    double fps() const;
    int frameCount() const;
    int width() const;
    int height() const;

    struct Decoded {
        QImage image;
        QByteArray audio; // interleaved s16, empty when audio was not asked for
        WorkingFrame working;
        QString error;
        bool cancelled = false;
        // The picture in an output encoding, 8-bit RGBA: the working frame over
        // black, through that OCIO output (by default Rec.709, ordinary
        // footage's own, so its pixels read as they were filmed), else the
        // image as it is.
        QImage picture(const QString &space = QStringLiteral("rec709/rec709")) const {
            if (working.isNull()) return image;
            // Over black in linear light, as an export without alpha delivers it.
            QImage flat = working.pixels().convertToFormat(QImage::Format_RGBA32FPx4);
            for (int y = 0; y < flat.height(); ++y) {
                auto *p = reinterpret_cast<float *>(flat.scanLine(y));
                for (int x = 0; x < flat.width(); ++x, p += 4) {
                    const float a = p[3] < 0.f ? 0.f : p[3] > 1.f ? 1.f : p[3];
                    p[0] *= a; p[1] *= a; p[2] *= a; p[3] = 1.f;
                }
            }
            return working.withPixels(flat).output(space).convertToFormat(QImage::Format_RGBA8888);
        }
    };

    // What the container says about itself, for the Media page's metadata
    // inspector: videoCodec, pixelFormat, bitDepth, colorSpace, audioCodec,
    // channels, sampleRate, audioBitDepth, created, make, model — only the
    // keys the file actually carries. Camera fields are rare outside MOV.
    QVariantMap metadata() const;
    // Per-channel audio peak (0..1) of one frame's worth of sound, left then
    // right — the Media page's L/R waveform. Empty when the clip is silent
    // or has no audio stream.
    QList<float> audioPeaksAt(int position);

    // Seeks and decodes one frame as RGBA. The QImage owns its pixels.
    QImage frameAt(int position);

    // Same, optionally with that frame's worth of audio resampled to
    // frequency/channels.
    // withImage false pulls only the sound: playback skipping a late picture.
    Decoded decodeAt(int position, bool withAudio, int frequency = 48000, int channels = 2,
                     bool withImage = true);

    // Still pictures: a still lasts kStillFrames.
    static bool isStillPath(const QString &path);
    static constexpr int kStillFrames = 15000;

private:
    QString m_path, m_decodePath;
    // Keep self-contained local media readable across rename/unlink while this
    // player is open. These outlive the native decoders.
    std::unique_ptr<QFile> m_sourceFile;
    std::unique_ptr<QTemporaryDir> m_sourceDirectory;
    QVariantMap m_workingManagement,m_sourceMetadata;
    std::unique_ptr<NativeVideo> m_native;
    // The file as it is read natively: its facts, a solid colour's name, its sound.
    media::Probe m_facts;
    QString m_colour;
    std::unique_ptr<AudioSampleSource::Reader> m_sound;
    bool m_soundTried = false;
    // Pictures NativeVideo does not take (float stills: EXR), through the engine's reader.
    std::unique_ptr<render::MediaSource> m_media;
    struct SwsContext *m_convert = nullptr;
    // The sound at `frequency` × `channels` for one frame, as 16-bit samples.
    QByteArray soundAt(int position, int frequency, int channels);
    std::unique_ptr<Timeline> m_document;  // an opened project
    // The file alone on a timeline (drawOnEngine), made when a picture is first
    // asked for and again when what it was made from changes.
    struct Single {
        QString original, mode;
        QVariantMap project, sources;
        bool wanted = false;   // drawOnEngine was called
        bool refused = false;  // the engine does not draw this file: the native decoder does
        bool proved = false;   // it has made a picture of it
        bool cpu = false;
        std::unique_ptr<Timeline> timeline;
        QVariantMap management;  // what `timeline` was made with
    } m_single;
    bool m_engineDrew = false;
    QSize m_drawSize;    // see setViewerDraw
    QVariant m_shownAs;
    Timeline *single();
};
