#pragma once
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

// Every snapshot key, once: X(kind, name). The kinds are explained on ClipEntry.
#define OMAEDIT_CLIP_FIELDS(X) \
    X(plain, name) X(plain, path) X(plain, timelineFps) X(plain, start) \
    X(plain, frames) X(plain, in) X(plain, out) X(plain, length) \
    X(plain, lane) X(plain, track) X(plain, index) X(plain, gapBefore) \
    X(plain, mixIn) X(plain, mixOut) X(plain, mixStyle) X(plain, mixAlign) \
    X(optional, mixExtA) X(optional, mixExtB) X(plain, speed) X(plain, audioOnly) \
    X(plain, videoOnly) X(plain, gap) X(plain, disabled) X(flag, still) \
    X(flag, offline) X(flag, compound) X(flag, nest) X(flag, adjust) \
    X(flag, rendered) X(optional, compoundJson) X(optional, nestId) X(optional, generator) \
    X(optional, generatorColor) X(optional, sourceOrigin) X(optional, sourceTiming) X(optional, sourceMeta) \
    X(optional, retime) X(optional, audioTiming) X(optional, frameBlend) X(optional, title) \
    X(optional, titlePlace) X(optional, titleSize) X(optional, titleFont) X(optional, titleColor) \
    X(optional, titleAlign) X(optional, titleWeight) X(optional, titleBg) X(optional, titleItalic) \
    X(optional, titleOutline) X(optional, caption) X(optional, titleTracking) X(optional, titleLineSpacing) \
    X(optional, titlePixelSize) X(optional, titleMargin) X(optional, titleWidth) X(optional, titleLook) \
    X(optional, speaker) X(flag, ownStyle) X(flag, typed) X(plain, volume) X(plain, pan) X(plain, volumeKf) \
    X(plain, eq) X(plain, fadeIn) X(plain, fadeOut) X(optional, audioCurve) \
    X(optional, audioMap) X(flag, normalize) X(plain, opacity) X(plain, opacityKf) \
    X(plain, rectX) X(plain, rectY) X(plain, rectW) X(plain, rectH) \
    X(plain, rotation) X(plain, anchorX) X(plain, anchorY) X(plain, cropL) \
    X(plain, cropR) X(plain, cropT) X(plain, cropB) X(plain, cropFeather) \
    X(plain, cropInvert) X(flag, flipH) X(flag, flipV) X(optional, freeze) \
    X(optional, blend) X(optional, lensK1) X(optional, lensK2) X(optional, lensCx) \
    X(optional, lensCy) X(optional, stab) X(optional, scaling) X(optional, videoEffects) \
    X(plain, editKeys) X(optional, effectOrder) X(optional, bypass) X(plain, colorB) \
    X(plain, colorC) X(plain, colorS) X(optional, grade) X(optional, gradeVersions) \
    X(optional, gradeVersion) X(plain, label) X(optional, category) X(optional, link) \
    X(optional, mcam) X(optional, mcamAngle) X(optional, markers) \
    X(optional, original)

// The key names, for code that has to work on the maps themselves (it edits
// them, compares them or passes unknown keys through): ClipKey::start
// instead of a string that could be mistyped.
namespace ClipKey {
#define X(kind, key) inline const QString key = QStringLiteral(#key);
OMAEDIT_CLIP_FIELDS(X)
#undef X
// Three more an entry can carry that the snapshot table does not hold: the
// clip list marks a picture whose source has no sound, and a clip held as a
// picture whose source is a recording with none to show (a project made
// before recordings went to audio tracks); a precise sound cut keeps its
// identity.
inline const QString silent = QStringLiteral("silent");
inline const QString pictureless = QStringLiteral("pictureless");
inline const QString clipId = QStringLiteral("clipId");
// And one a copy can: the transition it shared with the clip copied with it
// (Timeline::clipsApart), for the paste to put back across the pair.
inline const QString mixCarry = QStringLiteral("mixCarry");
} // namespace ClipKey

// One clip of a timeline snapshot, typed.
//
// Snapshots travel as QVariantMaps: QML reads them, undo keeps them and the
// project file stores them. Inside the timeline that meant a string lookup
// for every value, where a mistyped key compiles and quietly reads as 0.
// This is the same clip with a field per key. The key names live in one
// place, the table above, which both reading and writing go through, so a
// name can no longer differ between the two.
//
// Three kinds of field, as the snapshot has always had them:
//   plain      always written; read with the default given here when absent
//   flag       a bool written only when true
//   optional   written only when set; absent reads as "not set"
struct ClipEntry {
    // Where the clip sits.
    QString name, path;
    double timelineFps = 0;
    int start = 0, frames = 0, in = -1, out = -1, length = 0;
    int lane = 0, track = 0, index = 0;
    int gapBefore = 0;   // empty track space before this clip
    int mixIn = 0, mixOut = 0;
    QString mixStyle, mixAlign;
    std::optional<int> mixExtA, mixExtB;
    double speed = 1.0;  // signed: negative plays backwards

    // What kind of clip it is.
    bool audioOnly = false, videoOnly = false, gap = false, disabled = false;
    bool still = false, offline = false, compound = false, nest = false, adjust = false, rendered = false;
    std::optional<QString> compoundJson, nestId, generator, generatorColor;

    // Source.
    std::optional<double> sourceOrigin;
    std::optional<QVariant> sourceTiming, sourceMeta, retime, audioTiming;
    std::optional<int> frameBlend;

    // Title.
    std::optional<QString> title, titlePlace, titleSize, titleFont, titleColor, titleAlign, titleWeight, titleBg;
    std::optional<bool> titleItalic, titleOutline, caption;
    std::optional<QVariant> titleTracking, titleLineSpacing, titlePixelSize, titleMargin, titleWidth;
    // The fuller look (EditEngine::titleLookKeys: shadow, outline colour and
    // width, box padding and corners); absent = the look titles always had.
    std::optional<QVariantMap> titleLook;
    // A caption's speaker ("" or absent = none), whether it keeps a look
    // of its own rather than its caption track's style, and whether its
    // words were typed by hand (Update Captions asks before replacing them).
    std::optional<QString> speaker;
    bool ownStyle = false;
    bool typed = false;

    // Sound.
    double volume = 0, pan = 0;
    QVariantList volumeKf;
    QString eq;
    int fadeIn = 0, fadeOut = 0;
    std::optional<QString> audioCurve, audioMap;
    bool normalize = false;

    // Picture.
    double opacity = 1.0;
    QVariantList opacityKf;
    double rectX = 0, rectY = 0, rectW = 1, rectH = 1, rotation = 0, anchorX = 0.5, anchorY = 0.5;
    double cropL = 0, cropR = 0, cropT = 0, cropB = 0, cropFeather = 0;
    bool cropInvert = false, flipH = false, flipV = false;
    std::optional<int> freeze, blend;
    std::optional<double> lensK1, lensK2, lensCx, lensCy;
    std::optional<QVariantMap> stab;
    std::optional<QString> scaling;
    std::optional<QVariantList> videoEffects;
    QVariantMap editKeys;
    std::optional<QStringList> effectOrder, bypass;

    // Colour.
    double colorB = 0, colorC = 1, colorS = 1;
    std::optional<QVariantMap> grade;
    std::optional<QVariantList> gradeVersions;
    std::optional<int> gradeVersion;

    // Organisation.
    QString label;
    std::optional<QString> category, link, mcam;
    std::optional<int> mcamAngle;
    std::optional<QVariantList> markers;
    std::optional<QVariantMap> original;

    // The map a read entry came from, for the helpers that still take one.
    // Empty on an entry being built.
    QVariantMap raw;

    // Every key, once. `Self` is ClipEntry or const ClipEntry.
    template <class Self, class Visitor>
    static void fields(Self &e, Visitor &&v) {
#define X(kind, key) v.kind(ClipKey::key, e.key);
        OMAEDIT_CLIP_FIELDS(X)
#undef X
    }

    // A snapshot map as a typed entry. Keys this does not know stay in raw.
    static ClipEntry read(const QVariantMap &map) {
        ClipEntry entry;
        fields(entry, Reader{map});
        entry.raw = map;
        return entry;
    }

    static QList<ClipEntry> readList(const QVariantList &list) {
        QList<ClipEntry> entries;
        entries.reserve(list.size());
        for (const QVariant &v : list) entries.append(read(v.toMap()));
        return entries;
    }

    // The snapshot map for an entry built here. raw is not part of it.
    QVariantMap toVariant() const {
        QVariantMap map;
        fields(*this, Writer{map});
        return map;
    }

    // An optional text or number as the snapshot's readers have always taken
    // an absent one: empty, or zero.
    template <class T> static T orDefault(const std::optional<T> &field) { return field ? *field : T(); }
    // The same for a value handed on as a QVariant: absent stays invalid.
    template <class T> static QVariant orInvalid(const std::optional<T> &field) { return field ? QVariant::fromValue(*field) : QVariant(); }
    QVariantMap retimeMap() const { return retime ? retime->toMap() : QVariantMap(); }

private:
    struct Reader {
        const QVariantMap &map;
        template <class T> void plain(const QString &key, T &field) const {
            const auto it = map.constFind(key);
            if (it != map.constEnd()) field = it->template value<T>();
        }
        void flag(const QString &key, bool &field) const { field = map.value(key).toBool(); }
        template <class T> void optional(const QString &key, std::optional<T> &field) const {
            const auto it = map.constFind(key);
            if (it != map.constEnd()) field = it->template value<T>();
        }
    };
    struct Writer {
        QVariantMap &map;
        template <class T> void plain(const QString &key, const T &field) const { map.insert(key, QVariant::fromValue(field)); }
        void flag(const QString &key, bool field) const { if (field) map.insert(key, true); }
        template <class T> void optional(const QString &key, const std::optional<T> &field) const {
            if (field) map.insert(key, QVariant::fromValue(*field));
        }
    };
};
