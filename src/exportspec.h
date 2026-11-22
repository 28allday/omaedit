#pragma once

#include <limits>

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// The Deliver page's render settings as one map — what a preset stores,
// what a queue job carries, what the worker turns into the encoder sink's
// settings (and, for a frame-rate change, an ffmpeg command).
//
// Keys (every one optional; `normalized()` fills the defaults):
//   container   mp4 | mov | mkv | webm | m4a | wav | mp3 | flac
//   video       bool — render the picture (false = audio only)
//   audio       bool — render the sound
//   vcodec      h264 | hevc | prores | dnxhr | vp9 | av1 | mpeg2 | ffv1 | apv
//   hw          software | nvenc | vaapi | vulkan (progressive ProRes 422)
//   width/height int, 0 = the timeline's
//   fps         double, 0 = the timeline's (a different rate re-times
//               through the ffmpeg post-stage)
//   par         double pixel aspect, 1.0 square
//   scan        progressive | tff | bff
//   rateMode    quality | vbr | cbr
//   quality     int (crf / cq 0–51; ProRes/DNxHR ignore it)
//   bitrate     int kbps (vbr/cbr), maxrate int kbps (0 = none)
//   vprofile    "" | baseline | main | high | high10 | main10 |
//               proxy | lt | standard | hq | 4444 | 4444xq (ProRes) |
//               lb | sq | hq | hqx | 444 (DNxHR) | 422 (MPEG-2's 4:2:2 profile) |
//               422-10 | 422-12 | 444-10 | 444-12 | 4444-10 | 4444-12 (APV;
//               the chroma and bit depth, 4444 carries alpha)
//   level       "" | "4.1" …
//   gop         int frames, 0 = two seconds
//   bitDepth    8 | 10 | 12 (APV: always its profile's depth)
//   alpha       bool — keep transparency (prores 4444 / ffv1 / vp9)
//   colorSpace  rec709 | rec601 | srgb | rec2020 | pq | hlg
//   toneMap     "" | "ACES 1.3" | "ACES 2.0": scene light to the output's light
//   range       tv | pc
//   hdr         {maxCll, maxFall, masterDisplay} (hevc/av1 10-bit)
//   acodec      aac | opus | mp3 | pcm16 | pcm24 | flac | vorbis
//   arate       int Hz, abitrate int kbps, channels 1 | 2 | 6
//   subtitles   none | burn | embed | sidecar
//   metadata    {title, artist, album, comment, copyright, description, date}
//   rangeIn/rangeOut  absolute timeline frames, −1 = whole timeline
//   handles     int frames either side (per-clip jobs)
//   captionTrack int — the timeline track that carries captions (−1 none)
//   captionCues  [{start, frames, text}] for embed / sidecar
namespace ExportSpec {

QVariantMap normalized(const QVariantMap &spec);
QVariantList jobs(const QString &name,const QString &path,const QVariantMap &spec,int frames,double fps,const QVariantList &tracks,QString *error=nullptr,const QVariantList &categories=QVariantList());
QString outputKey(const QString &path);
QStringList outputPaths(const QString &path, const QVariantMap &spec);
QVariantList checkOutputs(const QVariantList &jobs, const QStringList &reserved = {});
bool publishOutput(const QString &temporary, const QString &path, const QVariantMap &spec, QString *error);
// h264-high / h264-medium / prores / audio — the pre-Deliver presets.
QVariantMap fromLegacyPreset(const QString &preset);

QString extension(const QVariantMap &spec); // ".mp4"
// The ffmpeg encoder name this spec asks for ("libx264", "h264_nvenc"…).
QString videoEncoder(const QVariantMap &spec);
QString audioEncoder(const QVariantMap &spec);
// The delivery's frame rate as FFmpeg reads it ("30000/1001", "50.000") when
// it differs from the timeline's; empty when it does not.
QString convertedRate(const QVariantMap &spec, double timelineFps);
// The mezzanine keeps 16 bits per channel (10-bit, HDR, ProRes and APV).
bool preciseMezzanine(const QVariantMap &spec);
bool embedsSubtitles(const QVariantMap &spec);

// What the encoder and muxer are given for a spec (the editor's own sink). Names
// are FFmpeg's (encoders, pixel formats, AVOptions, colour tags).
// A chapter of a movie: where it starts and ends, in milliseconds from the
// delivery's first frame, and its name.
struct Chapter {
    qint64 startMs = 0, endMs = 0;
    QString title;
};
// The chapters a timeline's markers give a delivery of frames `first`..`last`
// at `fps`: each marker of type "chapter" inside the range starts one that
// runs to the next (the last to the end), named by the marker's text, or
// "Chapter N" when it has none. Frames before the first such marker are a
// chapter of their own, "Start": chapters cover the whole delivery. Stored in a spec as "chapters" (a list of
// {startMs, endMs, title}) and read back with chaptersOf.
QVariantList chaptersFromMarkers(const QVariantList &markers, int first, int last, double fps);
// A spec's chapters, when its container holds them (MP4, MOV, Matroska, WebM)
// and it has pictures.
QList<Chapter> chaptersOf(const QVariantMap &spec);
// The same as an FFmpeg metadata file's text (";FFMETADATA1" …), for a join
// that would otherwise lose them; empty when there are none.
QString chapterMetadata(const QVariantMap &spec);

struct EncoderSettings {
    bool video = true, audio = true;
    int width = 0, height = 0;           // 0: the timeline's size
    int sarNum = 0, sarDen = 0;          // 0: no sample aspect set
    bool progressive = true, topFieldFirst = true;
    QString fieldOrder;                  // "tt" / "bb" when interlaced
    QString muxer;                       // the container's muxer ("mp4", "mov", "matroska", ...)
    QString vcodec, pixFmt;
    // A hardware encoder's device ("vaapi", "vulkan"; empty: none): pictures are
    // converted to hwFormat on the CPU and uploaded to hwDevice (a VA-API render
    // node; empty for the default Vulkan device).
    QString hw, hwDevice, hwFormat;
    // A command-line encoder fed raw pixFmt pictures on its standard input
    // (OpenAPV's): its program and options, to which the sink adds the size
    // (-w, -h) and the raw stream it writes (-o); pipeStream names that
    // stream's format for the container.
    QStringList pipeEncoder;
    QString pipeStream;
    QList<QPair<QString, QString>> videoOptions;  // AVOptions ("profile", "b", "crf", ...)
    int colorspace = 709;                // the matrix as an ITU number (601, 709, 2020)
    QString primaries, trc, matrix, range;
    // The tags the encoder writes into each picture's own header when that
    // header has a narrower set than the container (ProRes); empty: the above.
    QString headerPrimaries, headerTrc, headerMatrix;
    // HDR mastering display (x265's G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min)) and
    // light levels for the container; empty / 0: none.
    QString masterDisplay;
    int maxCll = 0, maxFall = 0;
    QString movflags;
    QString acodec;
    QList<QPair<QString, QString>> audioOptions;
    // Levelled sound: this gain (dB; NaN: none) with true peaks held under
    // truePeakCeiling dBTP.
    double loudnessGain = std::numeric_limits<double>::quiet_NaN();
    double truePeakCeiling = -1.0;
    int arate = 48000, channels = 2;
    QList<QPair<QString, QString>> metadata;  // container tags (title, artist, ...)
    // Where the file starts on the timeline's clock (see stampStart). A movie
    // carries `timecode`, "hh:mm:ss:ff" counted non-drop at the delivery's
    // rate: a timecode track in QuickTime and MP4, a tag in Matroska and
    // WebM. A WAV carries `timeReference`, the broadcast-wave count of
    // samples since midnight at its first sample. Empty / -1: none.
    QString timecode;
    qint64 timeReference = -1;
    QList<Chapter> chapters;                  // see chaptersOf; empty for a container without them
    // A numbered image sequence instead of a movie (see sequenceFormat): the
    // files' kind, the name they share, the number of the first one written
    // (its timeline frame) and whether they keep transparency. Nothing above
    // but video, width and height applies.
    QString sequence, sequenceStem;
    int sequenceFirst = 0;
    bool sequenceAlpha = false;
};
// A spec whose container is a numbered image sequence — "png", "tiff", "exr",
// "jpg" — names the kind of file in vcodec: "png16", "png8", "tiff16", "exr"
// (scene-linear half float) or "jpeg". Empty for a movie or sound.
QString sequenceFormat(const QVariantMap &spec);
// One file of a sequence: "<stem>_<frame, six digits>.<suffix>". The frame is
// the timeline's, so a range of 100–199 writes _000100 to _000199.
QString sequenceFileName(const QString &stem, int frame, const QString &format);
// `mezzanine` = the lossless intermediate for a post-stage instead of the
// delivery; `fps` the timeline's rate.
EncoderSettings encoderSettings(const QVariantMap &spec, double fps, bool mezzanine);

// The timecode of `frame` (a timeline timecode's count of frames, at
// fpsNum/fpsDen) as a file delivered at toNum/toDen says it: non-drop, the
// frame number counted at the delivery's rate, starting again after a day.
QString startTimecode(qint64 frame, int fpsNum, int fpsDen, int toNum, int toDen);
// The same frame as a count of samples since midnight at `sampleRate`.
qint64 startTimeReference(qint64 frame, int fpsNum, int fpsDen, int sampleRate);
// Gives `settings` the start its container can hold, for a delivery whose
// first frame has the timeline timecode `frame`.
void stampStart(EncoderSettings &settings, qint64 frame, int fpsNum, int fpsDen, int toNum, int toDen);
// What ffmpeg is told so that a file it stream-copies carries the same start.
QStringList startArgs(const EncoderSettings &settings);

// Exercise encoder/container initialization outside the editor process.
QStringList encoderProbeArgs(const QVariantMap &spec, int width, int height, double fps, const QString &output);
// APV: FFmpeg decodes it but has no encoder. The OpenAPV command-line
// encoder (OMAEDIT_APV_ENCODER, the package's lib/omaedit, a bin/setup-openapv
// build beside the binary, then PATH), or empty when none is installed.
QString apvEncoder();
// Whether the delivery's sound codec rebuilds the sound only roughly (AAC,
// Opus, MP3, Vorbis), its peaks and loudness not quite those that went in.
bool lossySound(const QVariantMap &spec);
// The true peak (dBTP) a levelled delivery promises: the spec's truePeak.
double promisedPeak(const QVariantMap &spec);
// The true-peak ceiling (dBTP) the delivery's limiter holds the levelled
// sound under: the one its sound check settled on (loudnessCeiling), or
// before that the promised peak, less room for a lossy codec's own overshoot.
double levellingCeiling(const QVariantMap &spec);
// The APV profile a spec encodes ("422-10" …), from vprofile/bitDepth/alpha.
QString apvProfile(const QVariantMap &spec);
// Remux `input` with the subtitle file into `output`, stream-copied.
QStringList ffmpegMuxSubtitleArgs(const QVariantMap &spec, const QString &input,
                                  const QString &subtitles, const QString &output);
// Join stream-copied segments of one render (pause / resume). With `audio`,
// the segments give the picture and that file, rendered in one pass, the sound.
QStringList ffmpegConcatArgs(const QString &listFile, const QString &output,
                             const QString &audio = QString(),
                             const QVariantMap &metadata = {}, // the concat demuxer drops container metadata
                             const QString &chapters = QString(), // and chapters: an FFmpeg metadata file of them
                             const QStringList &start = {});     // and the start timecode: startArgs()

// A rough size: rate control when it fixes the rate, a bits-per-pixel
// heuristic otherwise. Bytes.
qint64 estimateBytes(const QVariantMap &spec, double seconds, int width,
                     int height, double fps);

// Pre-flight. `context` keys: fps, width, height, seconds, freeBytes,
// dirExists, dirWritable, fileExists, missingMedia (list of names),
// missingServices (list), encoders (map name → bool), hasCaptions,
// hasAudio, hasVideo, path, eightBitEffects ([{clip, effects}]: clips whose
// effects work at 8 bits). Returns [{level: error|warning|ok, text}].
QVariantList validate(const QVariantMap &spec, const QVariantMap &context);

// Human summary for a preset row: "MP4 · H.264 · 3840×2160 · 25 fps".
QString summary(const QVariantMap &spec, int timelineWidth, int timelineHeight,
                double timelineFps);

// A render's file name made tidy, the one rule for every name the Export
// page builds (the file, its stems, parts and clips): what no file system
// takes (/ \ : * ? " < > |) becomes "-", "×" becomes "x", and a run of
// spaces, underscores and hyphens becomes one: " - " when it holds a hyphen
// and a space, "-" for a hyphen without one, " " for a space with no hyphen,
// "_" for underscores alone; none is left at either end. Letters and digits
// of every script stay as they are.
QString tidyFileName(const QString &name);

// File-name tokens: {project} {sequence} {date} {time} {resolution}
// {fps} {preset} {version}. `version` is the first number that does not
// collide in `dir`. The name comes out tidy (tidyFileName).
QString expandFileName(const QString &pattern, const QVariantMap &tokens,
                       const QString &dir, const QString &ext);


} // namespace ExportSpec
