#include "mediaplayer.h"
#include "projectfile.h"
#include "animatedwebp.h"
#include "timeline.h"
#include "nativevideo.h"
#include "bands.h"
#include "renderrequest.h"
#include "audiosamplesource.h"
#include "mediapool.h"
extern "C" {
#include <libswscale/swscale.h>
}
#include <cstring>
#include "colourpipeline.h"
#include <QFileInfo>
#include <QScopeGuard>
#include <QFile>
#include <QTemporaryDir>
#include <QImageReader>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <QColor>

#include <QtGlobal>
#include <QRegularExpression>
#include <QStringList>

MediaPlayer::MediaPlayer() = default;

MediaPlayer::~MediaPlayer() {
    close();
}

bool MediaPlayer::open(const QString &path) {
    close();

    QString decodePath=path;
    const QFileInfo info(path);
    std::unique_ptr<QFile> sourceFile;
    std::unique_ptr<QTemporaryDir> sourceDirectory;
    if (info.isAbsolute() || !path.contains(':')) {
        if (!info.isFile() || !info.isReadable()) return false;
#ifdef Q_OS_LINUX
        // The probe and the decoder each open the file. Keep a
        // descriptor and an extension-preserving link, so renaming/unlinking a
        // source cannot invalidate a later open inside that probe. Manifests and
        // projects retain their original base directory for relative resources.
        const QStringList selfContained{"mp4","mov","mxf","mkv","webm","avi","m4v","mpg","mpeg",
            "mts","m2ts","ts","ogv","ogg","wav","flac","aac","mp3","opus","aif","aiff","m4a",
            "png","jpg","jpeg","tif","tiff","exr","webp","bmp","dpx","psd"};
        if (selfContained.contains(info.suffix().toLower())) {
            sourceFile=std::make_unique<QFile>(path);
            if (!sourceFile->open(QIODevice::ReadOnly)) return false;
            sourceDirectory=std::make_unique<QTemporaryDir>();
            if (!sourceDirectory->isValid()) return false;
            decodePath=sourceDirectory->filePath("source."+info.suffix());
            if (!QFile::link(QString("/proc/self/fd/%1").arg(sourceFile->handle()),decodePath)) return false;
        }
#endif
    }
    // A file, or a solid colour: read natively.
    const bool document = ProjectFile::isProject(path);
    if (path.startsWith(QStringLiteral("color:"))) {
        m_facts = media::Probe{};
        m_facts.valid = true;
        m_facts.frames = kStillFrames;
        m_colour = path.mid(6);
        m_path = path;
        m_decodePath = path;
        return true;
    }
    if (info.isFile() && !document) {
        m_facts = media::probe(decodePath);
        if (!m_facts.valid) return false;
        m_path = path;
        m_decodePath = std::move(decodePath);
        m_sourceFile = std::move(sourceFile);
        m_sourceDirectory = std::move(sourceDirectory);
        return true;
    }
    if (!document || !info.isFile()) return false;
    // A project (or a nested sequence's file): its timeline, from its snapshot.
    auto timeline = std::make_unique<Timeline>();
    timeline->setAllowOffline(true);
    if (!timeline->loadDocument(path, nullptr) || timeline->frameCount() <= 0) return false;
    m_document = std::move(timeline);
    m_path = path;
    m_decodePath = path;
    return true;
}

bool MediaPlayer::isStillPath(const QString &path) {
    static const QStringList stills{"png","jpg","jpeg","tif","tiff","exr","webp","bmp","psd"};
    return stills.contains(QFileInfo(path).suffix().toLower()) && !AnimatedWebP::inspect(path).animated;
}

void MediaPlayer::close() {
    m_media.reset();
    sws_freeContext(m_convert);
    m_convert = nullptr;
    m_native.reset();m_workingManagement.clear();m_sourceMetadata.clear();m_path.clear();
    m_facts = media::Probe{};m_colour.clear();m_sound.reset();m_soundTried=false;
    m_document.reset();
    m_single = Single{}; m_engineDrew = false;
    m_sourceDirectory.reset(); m_sourceFile.reset(); m_decodePath.clear();
}

void MediaPlayer::drawOnEngine(const QString &original, const QVariantMap &project, const QVariantMap &sources,
                               const QString &mode) {
    // (Of the project's media settings a file's picture depends on its own
    // alone: another clip's changing is no reason to make its timeline again.)
    if (m_single.wanted && m_single.original == original && m_single.project == project && m_single.mode == mode
        && m_single.sources.value(original) == sources.value(original)) return;
    const bool cpu = m_single.cpu;
    m_single = Single{};
    m_single.wanted = true; m_single.cpu = cpu;
    m_single.original = original; m_single.project = project; m_single.sources = sources; m_single.mode = mode;
}

void MediaPlayer::setEngineOnCpu(bool cpu) {
    if (m_single.cpu == cpu) return;
    m_single.cpu = cpu; m_single.timeline.reset();
}

// The timeline that draws this file, or null when the native decoder does.
Timeline *MediaPlayer::single() {
    // (Read each time: a test measures both ways in one run.)
    if (!m_single.wanted || m_single.refused || qEnvironmentVariable("OMAEDIT_SOURCE_VIEWER") == QLatin1String("native")) return nullptr;
    if (!m_facts.valid || !m_facts.hasVideo || !m_colour.isEmpty()) return nullptr;
    if (!m_workingManagement.value("enabled").toBool() || m_workingManagement.value("engine") != QLatin1String("ocio")) return nullptr;
    if (m_single.timeline && m_single.management == m_workingManagement) return m_single.timeline.get();
    m_single.timeline.reset();
    m_single.management = m_workingManagement;
    const auto refuse = [this]() -> Timeline * { m_single.refused = true; return nullptr; };
    // A sensor recording is developed by the native decoder (RawPixels).
    if (m_facts.metadata.value(QStringLiteral("videoCodec")).toString() == QLatin1String("prores_raw")
        || m_facts.metadata.value(QStringLiteral("pixelFormat")).toString().contains(QLatin1String("bayer"))) return refuse();
    auto timeline = std::make_unique<Timeline>();
    timeline->setEngineOnCpu(m_single.cpu);
    // Its own shape: the frame and the clock follow the clip.
    QVariantMap project = m_single.project;
    project[QStringLiteral("custom")] = false;
    timeline->setProjectSettings(project);
    // The viewer's positions are the file's recorded pictures, whatever rate
    // Clip Attributes has it played at.
    QVariantMap sources = m_single.sources;
    QVariantMap own = sources.value(m_single.original).toMap();
    if (own.contains(QStringLiteral("forceFps"))) { own.remove(QStringLiteral("forceFps")); sources[m_single.original] = own; }
    timeline->setMediaSources(sources, m_single.mode.isEmpty() ? QStringLiteral("original") : m_single.mode);
    if (!timeline->append(m_single.original, -1, -1, 1.0, false, true)) return refuse();
    if (m_workingManagement != ColourPipeline::effective({})
        && !timeline->setGradeAt(0, 0, QVariantMap{{QStringLiteral("management"), m_workingManagement}})) return refuse();
    // A picture for a picture, on the file's clock, and all of it drawn by the engine.
    if (timeline->frameCount() != frameCount() || std::abs(timeline->fps() - fps()) > 1e-6 || !timeline->engineRendering()) return refuse();
    m_single.timeline = std::move(timeline);
    return m_single.timeline.get();
}

double MediaPlayer::fps() const {
    return m_document ? m_document->fps() : m_facts.valid ? m_facts.fps() : 0.0;
}

int MediaPlayer::frameCount() const {
    return m_document ? m_document->frameCount() : m_facts.valid ? m_facts.frames : 0;
}

int MediaPlayer::width() const {
    return m_document ? m_document->width() : m_facts.valid ? m_facts.width : 0;
}

int MediaPlayer::height() const {
    return m_document ? m_document->height() : m_facts.valid ? m_facts.height : 0;
}

// One frame's sound, cut as the timeline cuts its frames (on the float clock),
// as interleaved 16-bit samples; silence where the file has none.
QByteArray MediaPlayer::soundAt(int position, int frequency, int channels) {
    const double rate = double(float(fps()));
    if (rate <= 0 || frequency <= 0 || channels <= 0) return {};
    const auto at = [&](qint64 frame) { return qint64(double(frame) * frequency / rate + (frame < 0 ? -0.5 : 0.5)); };
    const qint64 first = at(position);
    const int count = int(at(position + 1) - first);
    if (count <= 0) return {};
    if (!m_soundTried && m_facts.hasAudio && m_colour.isEmpty()) {
        m_soundTried = true;
        QString error;
        m_sound = AudioSampleSource::Reader::open(m_decodePath, {}, &error);
    }
    QByteArray out(count * channels * int(sizeof(int16_t)), '\0');
    if (!m_sound) return out;
    QString error;
    const QVector<float> pcm = m_sound->read(first, count, frequency, channels, &error);
    if (!error.isEmpty() || RenderScope::cancelled()) {
        RenderScope::fail(error);
        return {};
    }
    auto *samples = reinterpret_cast<int16_t *>(out.data());
    for (int i = 0; i < count * channels && i < pcm.size(); ++i)
        samples[i] = int16_t(std::lrint(std::clamp(pcm[i], -1.f, 1.f) * 32767.f));
    return out;
}

QImage MediaPlayer::frameAt(int position) {
    return decodeAt(position, false).image;
}

MediaPlayer::Decoded MediaPlayer::decodeAt(int position, bool withAudio, int frequency, int channels,
                                      bool withImage) {
    if (m_facts.valid) {
        // Natively: the picture through the colour pipeline, the frame's sound.
        RenderScope scope;
        Decoded out;
        // A sound file has no picture: asking it for one is not a failure.
        if (withImage && !m_facts.hasVideo && m_facts.hasAudio && m_colour.isEmpty()) withImage = false;
        m_engineDrew = false;
        bool engineFailed = false;
        if (withImage) {
            if (Timeline *timeline = single()) {
                // The engine's picture of the file alone on its timeline: the
                // working frame, or as the timeline was told to draw for a viewer.
                const int frame = qBound(0, position, qMax(0, frameCount() - 1));
                MediaPlayer::Decoded made;
                timeline->setDrawSize(m_drawSize);
                timeline->setShownAs(m_shownAs);
                const auto asWorking = qScopeGuard([timeline] { timeline->setDrawSize({}); timeline->setShownAs({}); });
                if (m_single.proved) made = timeline->decodeAt(frame, false);
                else {
                    // Its first picture of this file, under a request of its
                    // own (cancelled with the caller's): should it fail, the
                    // native decoder is asked below under the caller's. If
                    // that makes the picture the file is the native decoder's
                    // from here on; if it fails too the failure is the file's.
                    const auto asked = RenderScope::current();
                    RenderScope attempt(std::make_shared<RenderRequest>([asked] { return asked->cancelled(); }));
                    made = timeline->decodeAt(frame, false);
                }
                if (made.cancelled || RenderScope::cancelled()) { out.cancelled = true; return out; }
                if (!made.image.isNull()) {
                    m_single.proved = true; m_engineDrew = true;
                    out.image = std::move(made.image); out.working = std::move(made.working);
                    withImage = false;
                } else if (m_single.proved) {
                    // It draws this file: the failure is this picture's.
                    out.error = made.error.isEmpty() ? QStringLiteral("The source picture could not be drawn.") : made.error;
                    RenderScope::fail(out.error);
                    out.error = RenderScope::current()->error();
                    return out;
                } else {
                    engineFailed = true;
                }
            }
        }
        if (withImage) {
            QImage pixels;
            if (!m_colour.isEmpty()) {
                pixels = QImage(width(), height(), QImage::Format_RGBA32FPx4);
                pixels.fill(QColor(m_colour));
            } else {
                // A test may fail or hold the read here (render::setReadHook).
                render::announceRead(m_path);
                if (RenderScope::cancelled() || !RenderScope::current()->error().isEmpty()) {
                    out.cancelled = RenderScope::cancelled();
                    if (!out.cancelled) out.error = RenderScope::current()->error();
                    return out;
                }
                if (!m_native) {
                    m_native = std::make_unique<NativeVideo>();
                    if (!m_native->open(m_decodePath, &out.error)) { m_native.reset(); return out; }
                }
                const double seconds = m_facts.still ? 0 : double(qBound(0, position, qMax(0, frameCount() - 1))) / fps();
                if (!m_media) pixels = m_native->frame(seconds, m_sourceMetadata, &out.error);
                if (pixels.isNull()) {
                    // A picture it does not take (a float still): the engine's reader, encoded.
                    if (!m_media) {
                        m_media = std::make_unique<render::MediaSource>();
                        if (!m_media->open(m_decodePath, &out.error)) { m_media.reset(); return out; }
                    }
                    out.error.clear();
                    render::Picture picture;
                    if (m_media->read(seconds, picture, &out.error)) pixels = picture.rgba64(&m_convert, &out.error);
                }
            }
            if (pixels.isNull()) return out;
            const auto lut = m_sourceMetadata.value("inputLut").toString();
            if (!lut.isEmpty()) pixels = ColourPipeline::applyLut(pixels, lut, &out.error);
            if (pixels.isNull()) return out;
            if (m_workingManagement.value("enabled").toBool()) {
                out.working = WorkingFrame::fromInput(pixels, m_workingManagement, &out.error);
                out.image = out.working.taggedImage();
            } else {
                // Shown as the file encodes it, through the default management.
                auto management = ColourPipeline::defaults();
                management["enabled"] = true;
                Decoded shown;
                shown.working = WorkingFrame::fromInput(pixels, management, &out.error);
                if (shown.working.isNull()) return out;
                out.image = shown.working.output(QStringLiteral("rec709/rec709")).convertToFormat(QImage::Format_RGBA8888);  // transparency kept
                out.image.setText("omaedit.pixelAspect", QString::number(double(m_facts.sarNum) / m_facts.sarDen, 'g', 17));
            }
        }
        // The native decoder made the picture the engine could not: the file is its.
        if (engineFailed && !out.image.isNull()) { m_single.refused = true; m_single.timeline.reset(); }
        if (withAudio) out.audio = soundAt(position, frequency, channels);
        RenderScope::fail(out.error);
        out.cancelled = RenderScope::cancelled();
        out.error = RenderScope::current()->error();
        if (out.cancelled || !out.error.isEmpty()) { out.image = {}; out.audio.clear(); out.working = {}; }
        if (out.cancelled) out.error.clear();
        return out;
    }
    if (!m_document)
        return Decoded();
    return m_document->decodeAt(position, withAudio, frequency, channels, withImage);
}

QVariantMap MediaPlayer::metadata() const {
    return m_facts.valid ? m_facts.metadata : QVariantMap();
}

QList<float> MediaPlayer::audioPeaksAt(int position) {
    QList<float> peaks;
    if (frameCount() <= 0)
        return peaks;
    const int at = qBound(0, position, frameCount() - 1);
    const QByteArray sound = m_document ? m_document->decodeAt(at, true, 48000, 2, false).audio
                                        : m_facts.valid ? soundAt(at, 48000, 2) : QByteArray();
    const auto *data = reinterpret_cast<const int16_t *>(sound.constData());
    const int samples = int(sound.size() / 4);
    if (samples <= 0) return peaks;
    int left = 0, right = 0;
    for (int i = 0; i < samples; ++i) {
        left = qMax(left, qAbs(int(data[i * 2])));
        right = qMax(right, qAbs(int(data[i * 2 + 1])));
    }
    peaks.append(float(left) / 32768.0f);
    peaks.append(float(right) / 32768.0f);
    return peaks;
}

void MediaPlayer::setInputTransform(const QVariantMap &settings) {
    m_sourceMetadata=settings;  // read by the native decode
}
