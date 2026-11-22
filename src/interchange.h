#pragma once
#include <atomic>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// Timeline interchange (File ▸ Import Timeline… / Export As ▸ Timeline…):
// one neutral document that the editor's entries convert to and from, and
// one codec per format. Times are integer frames at the document's rate;
// media positions are frames at the same rate counted from the start of
// the file (the editor's producer frames), so a format that speaks source
// timecode adds the file's start timecode on the way out and removes it on
// the way in.
namespace Interchange {

struct Marker { int frame = 0; QString text; QString type; };

struct Item {
    enum Kind { Clip, Gap };
    Kind kind = Clip;
    QString name;
    QString path;                 // local file; empty for a gap or a generator
    int start = 0;                // timeline frame
    int duration = 0;             // timeline frames (visible, transitions included on the outgoing side)
    int sourceIn = 0;             // producer frame the visible range starts at
    int sourceLength = 0;         // producer frames the file holds (0 = unknown)
    double speed = 1.0;           // user retime; negative = reversed
    QVariantMap retime;           // native boundary map; sourceIn then names its output frame
    int sampling = 0;             // 0 floor, 1 frame blend, 2 optical flow
    int transitionOut = 0;        // dissolve frames into the next item (the outgoing side owns it)
    bool enabled = true;
    bool audioOnly = false;
    bool videoOnly = false;
    qint64 sourceTimecodeFrames = -1; // the file's start timecode when known
    double sourceTimecodeSeconds = -1; // exact XML timecode, possibly between sequence frames
    QString generator;            // "title", "solid", "bars", … when not a file
    QList<Marker> markers;        // producer-frame positions
    QVariantMap extras;           // editor-only attributes carried through OTIO
};

struct Track {
    enum Kind { Video, Audio };
    Kind kind = Video;
    QString name;
    bool locked = false, muted = false, disabled = false;
    QList<Item> items;            // sorted by start, non-overlapping
};

struct Document {
    QString name;
    int fpsNum = 25, fpsDen = 1;
    int width = 1920, height = 1080;
    qint64 startTimecodeFrames = 0;
    QList<Track> tracks;          // video tracks first (V1 = the storyline), then audio
    QList<Marker> markers;        // timeline markers
    QStringList notes;            // what a codec could not carry
    double fps() const { return fpsDen > 0 ? double(fpsNum) / fpsDen : 25.0; }
};

// ── editor ↔ document ────────────────────────────────────────────────────
// `storyline` + `connected` are Backend::timeline()/connected() entries;
// `tracks` is Backend::tracks(); `library` supplies names, lengths and start
// timecodes by path.
Document fromTimeline(const QVariantList &storyline, const QVariantList &connected, const QVariantList &tracks,
                      const QVariantList &markers, const QVariantMap &project, const QVariantList &library);
struct Placement {
    QVariantList entries;         // restore()-ready, lanes numbered from 0
    QStringList trackNames;       // per lane
    QVariantList markers;         // {frame,text,type}
    QStringList mediaPaths;       // every file the entries reference
    QStringList problems;         // items that could not be placed
    QString error;                // timing cannot be preserved; do not place any entries
};
// `library` again supplies start timecodes (to turn source timecode into
// producer frames) and lengths; entries for unknown files still place.
Placement toEntries(const Document &document, double projectFps, const QVariantList &library);

// ── codecs ────────────────────────────────────────────────────────────────
QString formatOf(const QString &path);                     // "otio" | "edl" | "fcpxml" | "xmeml" | "aaf" | ""
QStringList formats();                                      // the five, import and export
QString formatName(const QString &format);                  // "OpenTimelineIO"…
QStringList formatPatterns(const QString &format);          // {"*.otio"}…
bool available(const QString &format, QString *why = nullptr); // AAF needs python-aaf2 at runtime
// Whether available() can answer for the format without starting anything.
bool availabilityKnown(const QString &format);
// Looks again the next time it is asked, as a new start of the app would.
void forgetAvailability();
bool write(const Document &document, const QString &path, const QString &format, QString *error);
// An EDL names no frame rate: it is read at `edlFpsNum`/`edlFpsDen`.
bool read(const QString &path, const QString &format, Document *document, QString *error, int edlFpsNum = 25, int edlFpsDen = 1);

// per-format entry points (also used by tests)
bool writeOtio(const Document &, const QString &path, QString *error);
bool readOtio(const QString &path, Document *, QString *error);
QString writeEdl(const Document &, const QString &title, QString *error);
bool readEdl(const QString &text, Document *, QString *error, int fpsNum = 25, int fpsDen = 1);
QString writeFcpxml(const Document &, QString *error);
bool readFcpxml(const QString &xml, Document *, QString *error);
// The older interchange XML (xmeml, .xml) that most editors still exchange.
QString writeXmeml(const Document &, QString *error);
bool readXmeml(const QString &xml, Document *, QString *error);
bool writeAaf(const Document &, const QString &path, QString *error, const std::atomic_bool *cancel = nullptr);
QStringList lastWriteNotes();                                // what this thread's last write could not carry (AAF helper)
bool readAaf(const QString &path, Document *, QString *error, const std::atomic_bool *cancel = nullptr);

// helpers shared by codecs
QString timecode(qint64 frames, double fps, bool dropFrame = false);
qint64 timecodeFrames(const QString &tc, double fps, bool *ok = nullptr, bool forceDrop = false);
// A length for people to read — "6.12 s", "1:23.40", "1:02:03.40" — the same
// form the interface shows for every duration.
QString durationText(qint64 frames, double fps);
QVariantMap toVariant(const Document &);                    // JSON-able (the AAF helper speaks this)
Document fromVariant(const QVariantMap &);
// Until each writer implements mapped timing, reject it before touching a file.
bool requiresMappedTiming(const Document &, QString *error);
}
