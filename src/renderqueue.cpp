#include "renderqueue.h"
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include "playerworker.h"
#include <QSet>
#include <QLockFile>
#include <vector>
#include <QCryptographicHash>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>

#include "exportspec.h"
#include "filepicker.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QUuid>

static QSettings appSettings() {
    return QSettings(QStringLiteral("omaedit"), QStringLiteral("omaedit"));
}

static QString jsonText(const QVariant &v) {
    return QString::fromUtf8(QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact));
}

static QVariant jsonValue(const QString &text) {
    return QJsonDocument::fromJson(text.toUtf8()).toVariant();
}

// ── RenderPresets ────────────────────────────────────────────────────────
static QVariantMap builtIn(const QString &id, const QString &name, const QString &group,
                           const QVariantMap &spec) {
    QVariantMap p;
    p.insert(QStringLiteral("id"), id);
    p.insert(QStringLiteral("name"), name);
    p.insert(QStringLiteral("group"), group);
    p.insert(QStringLiteral("builtin"), true);
    p.insert(QStringLiteral("spec"), ExportSpec::normalized(spec));
    return p;
}

static QVariantMap sp(std::initializer_list<QPair<QString, QVariant>> pairs) {
    QVariantMap m;
    for (const auto &kv : pairs)
        m.insert(kv.first, kv.second);
    return m;
}

QVariantList RenderPresets::builtIns() {
    using P = QPair<QString, QVariant>;
    QVariantList list;
    // Master
    list << builtIn(QStringLiteral("master-prores-hq"), QStringLiteral("ProRes 422 HQ"), QStringLiteral("master"),
                    sp({P{"container", "mov"}, P{"vcodec", "prores"}, P{"vprofile", "hq"}, P{"bitDepth", 10}, P{"acodec", "pcm24"}}));
    list << builtIn(QStringLiteral("master-prores-4444"), QStringLiteral("ProRes 4444 (alpha)"), QStringLiteral("master"),
                    sp({P{"container", "mov"}, P{"vcodec", "prores"}, P{"vprofile", "4444"}, P{"bitDepth", 10}, P{"alpha", true}, P{"acodec", "pcm24"}}));
    list << builtIn(QStringLiteral("master-apv-422-10"), QStringLiteral("APV 4:2:2 10-bit"), QStringLiteral("master"),
                    sp({P{"container", "mov"}, P{"vcodec", "apv"}, P{"vprofile", "422-10"}, P{"bitDepth", 10}, P{"quality", 18}, P{"acodec", "pcm24"}}));
    list << builtIn(QStringLiteral("master-apv-4444-12"), QStringLiteral("APV 4:4:4:4 12-bit (alpha)"), QStringLiteral("master"),
                    sp({P{"container", "mov"}, P{"vcodec", "apv"}, P{"vprofile", "4444-12"}, P{"bitDepth", 12}, P{"alpha", true}, P{"quality", 18}, P{"acodec", "pcm24"}}));
    list << builtIn(QStringLiteral("master-h265-10bit"), QStringLiteral("H.265 10-bit Master"), QStringLiteral("master"),
                    sp({P{"container", "mp4"}, P{"vcodec", "hevc"}, P{"vprofile", "main10"}, P{"bitDepth", 10}, P{"quality", 16}, P{"acodec", "aac"}, P{"abitrate", 320}}));
    list << builtIn(QStringLiteral("master-dnxhr-hqx"), QStringLiteral("DNxHR HQX"), QStringLiteral("master"),
                    sp({P{"container", "mov"}, P{"vcodec", "dnxhr"}, P{"vprofile", "hqx"}, P{"bitDepth", 10}, P{"acodec", "pcm24"}}));
    list << builtIn(QStringLiteral("master-ffv1"), QStringLiteral("Archive FFV1 (lossless)"), QStringLiteral("master"),
                    sp({P{"container", "mkv"}, P{"vcodec", "ffv1"}, P{"bitDepth", 10}, P{"acodec", "flac"}})); // 10-bit like the other masters, not the 8-bit default
    // Web: H.264 High 4:2:0 in a fast-start MP4, BT.709, the edit's own
    // frame rate up to 60, AAC at 48 kHz, and the sound levelled to the
    // usual streaming loudness (−14 LUFS, true peaks under −1 dBTP).
    const auto social = [](std::initializer_list<QPair<QString, QVariant>> pairs) {
        QVariantMap m = sp({P{"container", "mp4"}, P{"vcodec", "h264"}, P{"vprofile", "high"}, P{"quality", 20},
                            P{"maxFps", 60.0}, P{"gopSeconds", 2.0}, P{"bframes", 2}, P{"acodec", "aac"}, P{"arate", 48000},
                            P{"channels", 2}, P{"abitrate", 192}, P{"loudness", true}, P{"loudnessTarget", -14.0}, P{"truePeak", -1.0}});
        for (const auto &kv : pairs)
            m.insert(kv.first, kv.second);
        return m;
    };
    // YouTube asks for a closed GOP of half the frame rate and two B-frames.
    list << builtIn(QStringLiteral("web-youtube-2160p"), QStringLiteral("YouTube 2160p 4K"), QStringLiteral("web"),
                    social({P{"platform", "youtube"}, P{"width", 3840}, P{"height", 2160}, P{"quality", 18}, P{"gopSeconds", 0.5}, P{"abitrate", 384}}));
    list << builtIn(QStringLiteral("web-youtube-1080p"), QStringLiteral("YouTube 1080p"), QStringLiteral("web"),
                    social({P{"platform", "youtube"}, P{"width", 1920}, P{"height", 1080}, P{"quality", 18}, P{"gopSeconds", 0.5}, P{"abitrate", 384}}));
    list << builtIn(QStringLiteral("web-youtube-shorts"), QStringLiteral("YouTube Shorts 1080×1920"), QStringLiteral("web"),
                    social({P{"platform", "youtube-shorts"}, P{"width", 1080}, P{"height", 1920}, P{"quality", 18}, P{"gopSeconds", 0.5}, P{"abitrate", 384}}));
    list << builtIn(QStringLiteral("web-vimeo-1080p"), QStringLiteral("Vimeo 1080p"), QStringLiteral("web"),
                    social({P{"platform", "vimeo"}, P{"width", 1920}, P{"height", 1080}, P{"rateMode", "vbr"}, P{"bitrate", 20000}, P{"maxrate", 25000}, P{"abitrate", 320}}));
    list << builtIn(QStringLiteral("web-h265-1080p"), QStringLiteral("Web H.265 1080p"), QStringLiteral("web"),
                    sp({P{"container", "mp4"}, P{"vcodec", "hevc"}, P{"width", 1920}, P{"height", 1080}, P{"quality", 22}, P{"acodec", "aac"}, P{"abitrate", 160},
                        P{"loudness", true}, P{"loudnessTarget", -14.0}, P{"truePeak", -1.0}}));
    list << builtIn(QStringLiteral("web-vp9-1080p"), QStringLiteral("WebM VP9 1080p"), QStringLiteral("web"),
                    sp({P{"container", "webm"}, P{"vcodec", "vp9"}, P{"width", 1920}, P{"height", 1080}, P{"quality", 26}, P{"acodec", "opus"}, P{"abitrate", 128},
                        P{"loudness", true}, P{"loudnessTarget", -14.0}, P{"truePeak", -1.0}}));
    list << builtIn(QStringLiteral("web-av1-1080p"), QStringLiteral("AV1 1080p"), QStringLiteral("web"),
                    sp({P{"container", "mp4"}, P{"vcodec", "av1"}, P{"width", 1920}, P{"height", 1080}, P{"quality", 28}, P{"acodec", "aac"}, P{"abitrate", 160},
                        P{"loudness", true}, P{"loudnessTarget", -14.0}, P{"truePeak", -1.0}}));
    // Broadcast: delivered as ProRes 422 HQ, first, interlaced and
    // progressive, at the timeline's own rate. (The 1080i preset keeps the
    // id it had as ProRes 422, so a page left on it opens on it.)
    list << builtIn(QStringLiteral("broadcast-prores-1080i"), QStringLiteral("ProRes 422 HQ 1080i"), QStringLiteral("broadcast"),
                    sp({P{"container", "mov"}, P{"vcodec", "prores"}, P{"vprofile", "hq"}, P{"bitDepth", 10}, P{"width", 1920}, P{"height", 1080}, P{"scan", "tff"}, P{"acodec", "pcm24"}}));
    list << builtIn(QStringLiteral("broadcast-prores-1080p"), QStringLiteral("ProRes 422 HQ 1080p"), QStringLiteral("broadcast"),
                    sp({P{"container", "mov"}, P{"vcodec", "prores"}, P{"vprofile", "hq"}, P{"bitDepth", 10}, P{"width", 1920}, P{"height", 1080}, P{"scan", "progressive"}, P{"acodec", "pcm24"}}));
    // 50 Mb/s broadcast MPEG-2 is 4:2:2 (4:2:2 profile at high level), 8 bits.
    list << builtIn(QStringLiteral("broadcast-mpeg2-50"), QStringLiteral("MPEG-2 50 Mb/s 1080i"), QStringLiteral("broadcast"),
                    sp({P{"container", "mov"}, P{"vcodec", "mpeg2"}, P{"vprofile", "422"}, P{"rateMode", "cbr"}, P{"bitrate", 50000}, P{"width", 1920}, P{"height", 1080}, P{"scan", "tff"}, P{"acodec", "pcm16"}}));
    list << builtIn(QStringLiteral("broadcast-dnxhr-hq"), QStringLiteral("DNxHR HQ 1080p"), QStringLiteral("broadcast"),
                    sp({P{"container", "mov"}, P{"vcodec", "dnxhr"}, P{"vprofile", "hq"}, P{"width", 1920}, P{"height", 1080}, P{"acodec", "pcm24"}}));
    list << builtIn(QStringLiteral("broadcast-hdr10-4k"), QStringLiteral("HDR10 H.265 4K"), QStringLiteral("broadcast"),
                    sp({P{"container", "mp4"}, P{"vcodec", "hevc"}, P{"vprofile", "main10"}, P{"bitDepth", 10}, P{"width", 3840}, P{"height", 2160},
                        P{"colorSpace", "pq"}, P{"rateMode", "vbr"}, P{"bitrate", 40000}, P{"maxrate", 60000}, P{"acodec", "aac"}, P{"abitrate", 320},
                        P{"hdr", sp({P{"maxCll", 1000}, P{"maxFall", 400}, P{"masterDisplay", "G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000,1)"}})}}));
    // Social: sized and capped for each destination (Instagram takes at
    // most 25 Mb/s, X at most 1280×1024).
    list << builtIn(QStringLiteral("social-instagram-reels"), QStringLiteral("Instagram Reels 1080×1920"), QStringLiteral("social"),
                    social({P{"platform", "instagram-reels"}, P{"width", 1080}, P{"height", 1920}, P{"maxrate", 25000}, P{"abitrate", 128}}));
    list << builtIn(QStringLiteral("social-instagram-feed"), QStringLiteral("Instagram Feed 4:5 1080×1350"), QStringLiteral("social"),
                    social({P{"platform", "instagram-feed"}, P{"width", 1080}, P{"height", 1350}, P{"maxrate", 25000}, P{"abitrate", 128}}));
    list << builtIn(QStringLiteral("social-instagram-feed-34"), QStringLiteral("Instagram Feed 3:4 1080×1440"), QStringLiteral("social"),
                    social({P{"platform", "instagram-feed"}, P{"width", 1080}, P{"height", 1440}, P{"maxrate", 25000}, P{"abitrate", 128}}));
    list << builtIn(QStringLiteral("social-instagram-square"), QStringLiteral("Instagram 1080×1080"), QStringLiteral("social"),
                    social({P{"platform", "instagram-feed"}, P{"width", 1080}, P{"height", 1080}, P{"maxrate", 25000}, P{"abitrate", 128}}));
    list << builtIn(QStringLiteral("social-instagram-stories"), QStringLiteral("Instagram Stories 1080×1920"), QStringLiteral("social"),
                    social({P{"platform", "instagram-stories"}, P{"width", 1080}, P{"height", 1920}, P{"maxrate", 25000}, P{"abitrate", 128}}));
    list << builtIn(QStringLiteral("social-tiktok"), QStringLiteral("TikTok 1080×1920"), QStringLiteral("social"),
                    social({P{"platform", "tiktok"}, P{"width", 1080}, P{"height", 1920}}));
    list << builtIn(QStringLiteral("social-facebook-reels"), QStringLiteral("Facebook Reels 1080×1920"), QStringLiteral("social"),
                    social({P{"platform", "facebook-reels"}, P{"width", 1080}, P{"height", 1920}}));
    list << builtIn(QStringLiteral("social-facebook-feed"), QStringLiteral("Facebook Feed 4:5 1080×1350"), QStringLiteral("social"),
                    social({P{"platform", "facebook-feed"}, P{"width", 1080}, P{"height", 1350}}));
    list << builtIn(QStringLiteral("social-facebook-1080p"), QStringLiteral("Facebook 1080p"), QStringLiteral("social"),
                    social({P{"platform", "facebook-feed"}, P{"width", 1920}, P{"height", 1080}}));
    list << builtIn(QStringLiteral("social-x-720p"), QStringLiteral("X / Twitter 1280×720"), QStringLiteral("social"),
                    social({P{"platform", "x"}, P{"width", 1280}, P{"height", 720}, P{"rateMode", "vbr"}, P{"bitrate", 6000}, P{"maxrate", 8000}, P{"abitrate", 128}}));
    list << builtIn(QStringLiteral("social-x-vertical"), QStringLiteral("X / Twitter 720×1280"), QStringLiteral("social"),
                    social({P{"platform", "x"}, P{"width", 720}, P{"height", 1280}, P{"rateMode", "vbr"}, P{"bitrate", 6000}, P{"maxrate", 8000}, P{"abitrate", 128}}));
    list << builtIn(QStringLiteral("social-linkedin-1080p"), QStringLiteral("LinkedIn 1080p"), QStringLiteral("social"),
                    social({P{"platform", "linkedin"}, P{"width", 1920}, P{"height", 1080}, P{"maxrate", 30000}}));
    list << builtIn(QStringLiteral("social-linkedin-square"), QStringLiteral("LinkedIn 1080×1080"), QStringLiteral("social"),
                    social({P{"platform", "linkedin"}, P{"width", 1080}, P{"height", 1080}, P{"maxrate", 30000}}));
    // Audio only
    list << builtIn(QStringLiteral("audio-aac-320"), QStringLiteral("AAC 320 kbps"), QStringLiteral("audio"),
                    sp({P{"container", "m4a"}, P{"video", false}, P{"acodec", "aac"}, P{"abitrate", 320}}));
    list << builtIn(QStringLiteral("audio-mp3-320"), QStringLiteral("MP3 320 kbps"), QStringLiteral("audio"),
                    sp({P{"container", "mp3"}, P{"video", false}, P{"acodec", "mp3"}, P{"abitrate", 320}}));
    list << builtIn(QStringLiteral("audio-wav-24"), QStringLiteral("WAV 24-bit"), QStringLiteral("audio"),
                    sp({P{"container", "wav"}, P{"video", false}, P{"acodec", "pcm24"}}));
    list << builtIn(QStringLiteral("audio-flac"), QStringLiteral("FLAC"), QStringLiteral("audio"),
                    sp({P{"container", "flac"}, P{"video", false}, P{"acodec", "flac"}}));
    return list;
}

RenderPresets::RenderPresets(QObject *parent) : QObject(parent) {
    m_chooser = new FileChooser(this);
    connect(m_chooser, &FileChooser::selected, this, [this](const QUrl &url) {
        if (!m_pendingExport.isEmpty()) {
            const QString id = m_pendingExport;
            m_pendingExport.clear();
            exportPreset(id, url);
        } else {
            importPresets(url);
        }
    });
    connect(m_chooser, &FileChooser::canceled, this, [this] { m_pendingExport.clear(); });
    connect(m_chooser, &FileChooser::failed, this, [this](const QString &message) {
        m_pendingExport.clear();
        emit failed(message);
    });
    load();
    rebuild();
}

void RenderPresets::load() {
    QSettings s = appSettings();
    m_custom = jsonValue(s.value(QStringLiteral("renderPresets")).toString()).toList();
    m_favorites = s.value(QStringLiteral("renderPresetFavorites")).toStringList();
}

void RenderPresets::save() {
    QSettings s = appSettings();
    s.setValue(QStringLiteral("renderPresets"), jsonText(m_custom));
    s.setValue(QStringLiteral("renderPresetFavorites"), m_favorites);
}

void RenderPresets::rebuild() {
    QVariantList list = builtIns();
    for (int i = 0; i < list.size(); ++i) {
        QVariantMap p = list.at(i).toMap();
        p.insert(QStringLiteral("favorite"), m_favorites.contains(p.value(QStringLiteral("id")).toString()));
        list[i] = p;
    }
    for (const QVariant &v : std::as_const(m_custom)) {
        QVariantMap p = v.toMap();
        p.insert(QStringLiteral("group"), QStringLiteral("custom"));
        p.insert(QStringLiteral("builtin"), false);
        p.insert(QStringLiteral("spec"), ExportSpec::normalized(p.value(QStringLiteral("spec")).toMap()));
        if (!p.contains(QStringLiteral("favorite")))
            p.insert(QStringLiteral("favorite"), false);
        list.append(p);
    }
    m_presets = list;
    emit presetsChanged();
}

int RenderPresets::customIndex(const QString &id) const {
    for (int i = 0; i < m_custom.size(); ++i)
        if (m_custom.at(i).toMap().value(QStringLiteral("id")).toString() == id)
            return i;
    return -1;
}

QVariantMap RenderPresets::preset(const QString &id) const {
    for (const QVariant &v : m_presets)
        if (v.toMap().value(QStringLiteral("id")).toString() == id)
            return v.toMap();
    return {};
}

bool RenderPresets::nameTaken(const QString &name) const {
    for (const QVariant &v : m_presets)
        if (v.toMap().value(QStringLiteral("name")).toString().compare(name, Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

QString RenderPresets::savePreset(const QString &nameIn, const QVariantMap &spec) {
    const QString name = nameIn.trimmed();
    if (name.isEmpty() || nameTaken(name))
        return QString();
    QVariantMap p;
    const QString id = QStringLiteral("custom-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    p.insert(QStringLiteral("id"), id);
    p.insert(QStringLiteral("name"), name);
    QVariantMap s = ExportSpec::normalized(spec);
    // A preset is settings, not a job: no range, no cues.
    s.remove(QStringLiteral("rangeIn"));
    s.remove(QStringLiteral("rangeOut"));
    s.remove(QStringLiteral("captionCues"));
    s.remove(QStringLiteral("captionTrack"));
    s.remove(QStringLiteral("overwritePaths"));
    p.insert(QStringLiteral("spec"), s);
    p.insert(QStringLiteral("favorite"), false);
    m_custom.append(p);
    save();
    rebuild();
    return id;
}

bool RenderPresets::updatePreset(const QString &id, const QVariantMap &spec) {
    const int i = customIndex(id);
    if (i < 0)
        return false;
    QVariantMap p = m_custom.at(i).toMap();
    QVariantMap s = ExportSpec::normalized(spec);
    s.remove(QStringLiteral("rangeIn"));
    s.remove(QStringLiteral("rangeOut"));
    s.remove(QStringLiteral("captionCues"));
    s.remove(QStringLiteral("captionTrack"));
    s.remove(QStringLiteral("overwritePaths"));
    p.insert(QStringLiteral("spec"), s);
    m_custom[i] = p;
    save();
    rebuild();
    return true;
}

QString RenderPresets::duplicatePreset(const QString &id) {
    const QVariantMap src = preset(id);
    if (src.isEmpty())
        return QString();
    QString name = src.value(QStringLiteral("name")).toString() + QStringLiteral(" copy");
    for (int n = 2; nameTaken(name); ++n)
        name = QStringLiteral("%1 copy %2").arg(src.value(QStringLiteral("name")).toString()).arg(n);
    return savePreset(name, src.value(QStringLiteral("spec")).toMap());
}

bool RenderPresets::renamePreset(const QString &id, const QString &nameIn) {
    const QString name = nameIn.trimmed();
    const int i = customIndex(id);
    if (i < 0 || name.isEmpty())
        return false;
    QVariantMap p = m_custom.at(i).toMap();
    if (p.value(QStringLiteral("name")).toString() != name && nameTaken(name))
        return false;
    p.insert(QStringLiteral("name"), name);
    m_custom[i] = p;
    save();
    rebuild();
    return true;
}

bool RenderPresets::deletePreset(const QString &id) {
    const int i = customIndex(id);
    if (i < 0)
        return false;
    m_custom.removeAt(i);
    save();
    rebuild();
    return true;
}

void RenderPresets::setFavorite(const QString &id, bool on) {
    const int i = customIndex(id);
    if (i >= 0) {
        QVariantMap p = m_custom.at(i).toMap();
        p.insert(QStringLiteral("favorite"), on);
        m_custom[i] = p;
    } else {
        if (on && !m_favorites.contains(id))
            m_favorites.append(id);
        else if (!on)
            m_favorites.removeAll(id);
    }
    save();
    rebuild();
}

bool RenderPresets::exportPreset(const QString &id, const QUrl &url) {
    const QVariantMap p = preset(id);
    if (p.isEmpty())
        return false;
    QString path = url.toLocalFile();
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
        path += QStringLiteral(".json");
    QVariantMap doc;
    doc.insert(QStringLiteral("omaeditPreset"), 1);
    doc.insert(QStringLiteral("name"), p.value(QStringLiteral("name")));
    doc.insert(QStringLiteral("spec"), p.value(QStringLiteral("spec")));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit failed(QStringLiteral("Could not write %1").arg(QFileInfo(path).fileName()));
        return false;
    }
    f.write(QJsonDocument::fromVariant(doc).toJson(QJsonDocument::Indented));
    emit exported(path);
    return true;
}

int RenderPresets::importPresets(const QUrl &url) {
    QFile f(url.toLocalFile());
    if (!f.open(QIODevice::ReadOnly)) {
        emit failed(QStringLiteral("Could not read %1").arg(QFileInfo(f.fileName()).fileName()));
        return 0;
    }
    const QVariant doc = QJsonDocument::fromJson(f.readAll()).toVariant();
    QVariantList items;
    if (doc.canConvert<QVariantList>())
        items = doc.toList();
    else
        items << doc;
    int count = 0;
    for (const QVariant &v : std::as_const(items)) {
        const QVariantMap m = v.toMap();
        if (!m.contains(QStringLiteral("spec")))
            continue;
        QString name = m.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty())
            name = QStringLiteral("Imported preset");
        for (int n = 2; nameTaken(name); ++n)
            name = QStringLiteral("%1 %2").arg(m.value(QStringLiteral("name")).toString().trimmed()).arg(n);
        if (!savePreset(name, m.value(QStringLiteral("spec")).toMap()).isEmpty())
            ++count;
    }
    if (count == 0)
        emit failed(QStringLiteral("No presets found in %1").arg(QFileInfo(f.fileName()).fileName()));
    else
        emit imported(count);
    return count;
}

void RenderPresets::exportPresetDialog(const QString &id) {
    const QVariantMap p = preset(id);
    if (p.isEmpty())
        return;
    m_pendingExport = id;
    m_chooser->saveFile(QStringLiteral("Export Preset"), p.value(QStringLiteral("name")).toString() + QStringLiteral(".json"),
                        QStringLiteral("OmaEdit preset"), {QStringLiteral("*.json")});
}

void RenderPresets::importPresetsDialog() {
    m_pendingExport.clear();
    m_chooser->openFile(QStringLiteral("Import Presets"), QStringLiteral("OmaEdit preset"), {QStringLiteral("*.json")});
}

// ── RenderQueue ──────────────────────────────────────────────────────────
// The newest queue state waiting to be written. The interface thread only
// swaps in its (implicitly shared) containers; the serial snapshot worker
// takes whatever is newest when it gets there, so bursts coalesce and a
// later state can never be overwritten by an earlier one.
struct RenderQueue::SaveState {
    QMutex mutex;
    QVariantList jobs;
    QString afterCompletion, group;
    bool pending = false;   // a state waits to be written
    bool scheduled = false; // a worker job will pick it up
};

RenderQueue::RenderQueue(QObject *parent) : QObject(parent), m_saveState(std::make_shared<SaveState>()) {
    m_snapshotNamespace = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_snapshotPool.setMaxThreadCount(1);
    m_savePool.setMaxThreadCount(1);
    m_ticker.setInterval(1000);
    connect(&m_ticker, &QTimer::timeout, this, &RenderQueue::tick);
    load();
}

int RenderQueue::indexOf(int id) const {
    for (int i = 0; i < m_jobs.size(); ++i)
        if (m_jobs.at(i).toMap().value(QStringLiteral("id")).toInt() == id)
            return i;
    return -1;
}

int RenderQueue::pendingCount() const {
    int n = 0;
    for (const QVariant &v : m_jobs) {
        const QString s = v.toMap().value(QStringLiteral("status")).toString();
        if (s == QStringLiteral("queued") || s == QStringLiteral("rendering") || s == QStringLiteral("paused"))
            ++n;
    }
    return n;
}

void RenderQueue::setAfterCompletion(const QString &action) {
    if (m_afterCompletion == action)
        return;
    m_afterCompletion = action;
    emit afterCompletionChanged();
}

QVariantMap RenderQueue::job(int id) const {
    const int i = indexOf(id);
    return i < 0 ? QVariantMap() : m_jobs.at(i).toMap();
}

QStringList RenderQueue::jobLog(int id) const {
    return job(id).value(QStringLiteral("log")).toStringList();
}

void RenderQueue::touch() {
    save();
    emit jobsChanged();
}

int RenderQueue::enqueue(const QString &name, const QString &path, const QVariantMap &spec,
                         const QString &presetName, const QStringList &warnings) {
    if (!m_queueLock || !m_queueLock->isLocked()) {
        emit failed(m_storageError);
        return -1;
    }
    if (path.isEmpty())
        return -1;
    QVariantMap j;
    const int id = m_nextId++;
    j.insert(QStringLiteral("id"), id);
    j.insert(QStringLiteral("name"), name.isEmpty() ? QFileInfo(path).completeBaseName() : name);
    j.insert(QStringLiteral("preset"), presetName);
    auto settings = ExportSpec::normalized(spec);
    if (!settings.contains("overwritePaths")) settings["overwritePaths"] = QStringList();
    j.insert(QStringLiteral("spec"), settings);
    j.insert(QStringLiteral("path"), path);
    j.insert(QStringLiteral("status"), QStringLiteral("queued"));
    j.insert(QStringLiteral("stage"), QString());
    j.insert(QStringLiteral("percent"), 0);
    j.insert(QStringLiteral("elapsed"), 0);
    j.insert(QStringLiteral("remaining"), -1);
    j.insert(QStringLiteral("warnings"), warnings);
    j.insert(QStringLiteral("log"), QStringList());
    j.insert(QStringLiteral("error"), QString());
    j.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    m_jobs.append(j);
    touch();
    return id;
}

void RenderQueue::renderAll() {
    m_released.reset();
    m_queuePaused = false;
    m_anyFailed = false;
    m_anyCancelled = false;
    pump();
    emit jobsChanged();
}

void RenderQueue::renderJobs(const QList<int> &ids) {
    if (m_released)
        *m_released += QSet<int>(ids.begin(), ids.end());
    else if (m_activeJob < 0)
        m_released = QSet<int>(ids.begin(), ids.end());
    // Otherwise the whole queue is already running, these jobs included.
    m_queuePaused = false;
    m_anyFailed = false;
    m_anyCancelled = false;
    pump();
    emit jobsChanged();
}

void RenderQueue::pump() {
    if (m_activeJob >= 0 || m_queuePaused)
        return;
    for (int i = 0; i < m_jobs.size(); ++i) {
        QVariantMap j = m_jobs.at(i).toMap();
        if (j.value(QStringLiteral("status")).toString() != QStringLiteral("queued"))
            continue;
        if (m_released && !m_released->contains(j.value(QStringLiteral("id")).toInt()))
            continue;
        if(!j.value("spec").toMap().value("snapshotReady",true).toBool()) {m_waitingSnapshot=true;return;}
        m_waitingSnapshot=false;
        m_activeJob = j.value(QStringLiteral("id")).toInt();
        j.insert(QStringLiteral("status"), QStringLiteral("rendering"));
        j.insert(QStringLiteral("percent"), 0);
        j.insert(QStringLiteral("elapsed"), 0);
        j.insert(QStringLiteral("remaining"), -1);
        j.insert(QStringLiteral("started"), QDateTime::currentDateTime().toString(Qt::ISODate));
        j.insert(QStringLiteral("log"), QStringList());
        j.insert(QStringLiteral("error"), QString());
        m_jobs[i] = j;
        m_elapsed.restart();
        m_elapsedBefore = 0;
        m_jobPaused = false;
        m_ticker.start();
        m_ranThisRun = true;
        touch();
        emit startRequested(j.value(QStringLiteral("path")).toString(), j.value(QStringLiteral("spec")).toMap());
        return;
    }
    // Nothing left to start.
    m_ticker.stop();
    if (m_ranThisRun) {
        m_ranThisRun = false;
        emit queueDrained(!m_anyFailed && !m_anyCancelled);
    }
}

void RenderQueue::tick() {
    const int i = indexOf(m_activeJob);
    if (i < 0)
        return;
    QVariantMap j = m_jobs.at(i).toMap();
    const qint64 elapsed = m_elapsedBefore + (m_jobPaused ? 0 : m_elapsed.elapsed());
    j.insert(QStringLiteral("elapsed"), int(elapsed / 1000));
    const int p = j.value(QStringLiteral("percent")).toInt();
    j.insert(QStringLiteral("remaining"), p > 2 ? int(elapsed / 1000.0 * (100 - p) / p) : -1);
    m_jobs[i] = j;
    emit jobsChanged();
}

void RenderQueue::addWarning(const QString &warning) {
    const int i=indexOf(m_activeJob);if(i<0)return;auto j=m_jobs[i].toMap();auto warnings=j.value("warnings").toStringList();warnings.append(warning);j["warnings"]=warnings;m_jobs[i]=j;touch();
}
struct RenderQueue::SnapshotSave {
    int id;
    quint64 version;
    QString file;
    bool saved = false;
};

RenderQueue::~RenderQueue() {
    disconnect(this, nullptr, nullptr, nullptr);
    // Finish atomic writes before the receiver disappears. Publish their file
    // references as well, so a normal close cannot strand a queued export.
    m_snapshotPool.waitForDone();
    const auto pending = m_snapshotSaves;
    for (const auto &result : pending) finishSnapshot(result, false);
    cleanSnapshots();
    m_snapshotPool.waitForDone();
    // The final state lands before the object goes: the worker writes it and
    // is joined here, so a normal close never loses the last change.
    save();
    m_savePool.waitForDone();
}

void RenderQueue::attachSnapshot(int id, const QVariantMap &snapshot) {
    if (indexOf(id) < 0 || id == m_activeJob) return;
    auto result = std::make_shared<SnapshotSave>();
    result->id = id;
    result->version = ++m_snapshotVersion;
    m_snapshotVersions[id] = result->version;
    m_snapshotSaves.append(result);
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                              + QStringLiteral("/render-snapshots");
    const QString prefix = m_snapshotNamespace + QLatin1Char('-');
    // QVariant containers share their storage; serialization, hashing and disk
    // access all happen on this serial worker, away from the interface thread.
    m_snapshotPool.start(QRunnable::create([this, result, snapshot, directory, prefix] {
        const QByteArray bytes = QJsonDocument::fromVariant(snapshot).toJson(QJsonDocument::Compact);
        result->file = directory + QLatin1Char('/') + prefix
                       + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
                       + QStringLiteral(".json");
        QDir().mkpath(directory);
        result->saved = QFileInfo::exists(result->file);
        if (!result->saved) {
            QSaveFile output(result->file);
            result->saved = output.open(QIODevice::WriteOnly)
                            && output.write(bytes) == bytes.size() && output.commit();
        }
        QMetaObject::invokeMethod(this, [this, result] { finishSnapshot(result); }, Qt::QueuedConnection);
    }));
}

void RenderQueue::finishSnapshot(const std::shared_ptr<SnapshotSave> &result, bool notify) {
    if (!m_snapshotSaves.removeOne(result)) return;
    if (result->saved) m_snapshotFiles.insert(result->file);
    const int i = indexOf(result->id);
    if (i >= 0 && m_snapshotVersions.value(result->id) == result->version) {
        auto job = m_jobs.at(i).toMap();
        auto spec = job.value(QStringLiteral("spec")).toMap();
        if (result->saved) {
            spec[QStringLiteral("snapshotFile")] = result->file;
            spec[QStringLiteral("snapshotReady")] = true;
            job[QStringLiteral("spec")] = spec;
        } else {
            job[QStringLiteral("status")] = QStringLiteral("failed");
            job[QStringLiteral("error")] = tr("Could not save the queued project snapshot.");
            m_anyFailed = true;
        }
        m_jobs[i] = job;
        m_snapshotVersions.remove(result->id);
        if (notify) touch();
    }
    if (notify) {
        cleanSnapshots();
        if (m_waitingSnapshot) pump();
    }
}

void RenderQueue::cleanSnapshots() {
    // Wait until every new file has been published. Otherwise cleanup could
    // delete a completed write whose queued reply has not reached us yet.
    if (!m_snapshotSaves.isEmpty()) return;
    QSet<QString> referenced;
    for (const auto &value : m_jobs)
        referenced.insert(value.toMap().value(QStringLiteral("spec")).toMap()
                              .value(QStringLiteral("snapshotFile")).toString());
    const auto obsolete = m_snapshotFiles - referenced;
    if (obsolete.isEmpty()) return;
    m_snapshotFiles.subtract(obsolete);
    // Only remove files created by this queue; never scan/delete other running
    // instances' snapshots. Cleanup stays serial with snapshot writes (a new
    // write of the same content must not race the delete) and first waits for
    // the queue saves already issued, so the stored queue no longer names a
    // file by the time it goes.
    auto *saves = &m_savePool;
    m_snapshotPool.start(QRunnable::create([obsolete, saves] {
        saves->waitForDone();
        for (const auto &path : obsolete) QFile::remove(path);
    }));
}

void RenderQueue::onProgress(int percent) {
    const int i = indexOf(m_activeJob);
    if (i < 0)
        return;
    QVariantMap j = m_jobs.at(i).toMap();
    if (j.value(QStringLiteral("percent")).toInt() == percent)
        return;
    j.insert(QStringLiteral("percent"), qBound(0, percent, 100));
    m_jobs[i] = j;
    emit jobsChanged();
}

void RenderQueue::onStage(const QString &stage) {
    const int i = indexOf(m_activeJob);
    if (i < 0)
        return;
    QVariantMap j = m_jobs.at(i).toMap();
    j.insert(QStringLiteral("stage"), stage);
    m_jobs[i] = j;
    emit jobsChanged();
}

void RenderQueue::onPaused(bool paused) {
    const int i = indexOf(m_activeJob);
    if (i < 0)
        return;
    QVariantMap j = m_jobs.at(i).toMap();
    j.insert(QStringLiteral("status"), paused ? QStringLiteral("paused") : QStringLiteral("rendering"));
    m_jobs[i] = j;
    if (paused) {
        m_elapsedBefore += m_elapsed.elapsed();
        m_jobPaused = true;
        m_ticker.stop();
    } else {
        m_elapsed.restart();
        m_jobPaused = false;
        m_ticker.start();
    }
    touch();
}

void RenderQueue::onEnded(bool ok, const QString &path, const QString &reason, const QStringList &log) {
    const int i = indexOf(m_activeJob);
    const int id = m_activeJob;
    m_activeJob = -1;
    m_ticker.stop();
    if (i >= 0) {
        QVariantMap j = m_jobs.at(i).toMap();
        const qint64 elapsed = m_elapsedBefore + (m_jobPaused ? 0 : m_elapsed.elapsed());
        j.insert(QStringLiteral("elapsed"), int(elapsed / 1000));
        j.insert(QStringLiteral("remaining"), 0);
        j.insert(QStringLiteral("stage"), QString());
        j.insert(QStringLiteral("log"), log);
        j.insert(QStringLiteral("finished"), QDateTime::currentDateTime().toString(Qt::ISODate));
        if (ok) {
            j.insert(QStringLiteral("percent"), 100);
            j.insert(QStringLiteral("status"), j.value(QStringLiteral("warnings")).toStringList().isEmpty()
                                                    ? QStringLiteral("completed") : QStringLiteral("warning"));
            j.insert(QStringLiteral("bytes"), QFileInfo(path).size());
        } else if (reason == QStringLiteral("Cancelled")) {
            j.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
            m_anyCancelled = true;
        } else {
            j.insert(QStringLiteral("status"), QStringLiteral("failed"));
            j.insert(QStringLiteral("error"), reason);
            m_anyFailed = true;
        }
        m_jobs[i] = j;
    }
    if (m_released)
        m_released->remove(id);
    touch();
#ifdef __GLIBC__
    // What the job's encoders freed goes back to the system now. Left to the
    // allocator, each of their threads' arenas keeps it, and a queue of
    // renders held gigabytes it no longer used, more with each turn.
    malloc_trim(0);
#endif
    emit jobFinished(id, ok, path, reason);
    // Cancelling stops the queue where it is; a failure carries on.
    if (reason != QStringLiteral("Cancelled") || !m_queuePaused)
        pump();
}

void RenderQueue::pauseAll() {
    m_queuePaused = true;
    if (m_activeJob >= 0 && !m_jobPaused)
        emit pauseRequested();
    touch();
}

void RenderQueue::resumeAll() {
    m_queuePaused = false;
    if (m_activeJob >= 0 && m_jobPaused)
        emit resumeRequested();
    else
        pump();
    touch();
}

void RenderQueue::pauseJob(int id) {
    if (id == m_activeJob && !m_jobPaused)
        emit pauseRequested();
}

void RenderQueue::resumeJob(int id) {
    if (id == m_activeJob && m_jobPaused)
        emit resumeRequested();
}

void RenderQueue::cancelJob(int id) {
    if (id == m_activeJob) {
        // Only this job: the rest of the queue carries on, unless it is
        // paused (Pause All, or quitting), which the cancel leaves alone.
        emit cancelRequested();
        return;
    }
    const int i = indexOf(id);
    if (i < 0)
        return;
    QVariantMap j = m_jobs.at(i).toMap();
    if (j.value(QStringLiteral("status")).toString() == QStringLiteral("queued")) {
        j.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
        m_jobs[i] = j;
        touch();
    }
}

void RenderQueue::retryJob(int id) {
    const int i = indexOf(id);
    if (i < 0)
        return;
    QVariantMap j = m_jobs.at(i).toMap();
    const QString s = j.value(QStringLiteral("status")).toString();
    if (s == QStringLiteral("rendering") || s == QStringLiteral("paused") || s == QStringLiteral("queued"))
        return;
    j.insert(QStringLiteral("status"), QStringLiteral("queued"));
    j.insert(QStringLiteral("percent"), 0);
    j.insert(QStringLiteral("error"), QString());
    j.insert(QStringLiteral("log"), QStringList());
    m_jobs[i] = j;
    if (m_released)
        m_released->insert(id); // asking for a retry asks for it to run
    touch();
    // A job that failed before its snapshot was captured (the editor closed)
    // still has none; pump() would wait on it for ever.
    if (!j.value(QStringLiteral("spec")).toMap().value(QStringLiteral("snapshotReady"), true).toBool())
        emit snapshotNeeded(id);
    pump();
}

void RenderQueue::removeJob(int id) {
    if (id == m_activeJob)
        return; // cancel first
    const int i = indexOf(id);
    if (i < 0)
        return;
    m_jobs.removeAt(i);
    m_snapshotVersions.remove(id);
    touch();
    cleanSnapshots();
}

int RenderQueue::duplicateJob(int id) {
    const QVariantMap j = job(id);
    if (j.isEmpty())
        return -1;
    const QFileInfo original(j.value("path").toString());
    QString path;
    for(int n=1;;++n) {
        path=original.dir().filePath(original.completeBaseName()+QString("_copy%1.").arg(n)+original.suffix());
        bool used=QFileInfo::exists(path);
        for(const auto &v:m_jobs)used=used||v.toMap().value("path").toString()==path;
        if(!used)break;
    }
    // The copy renders the project as it is now, with its own snapshot —
    // never the original's file (which may not exist yet, or be stale).
    QVariantMap spec = j.value(QStringLiteral("spec")).toMap();
    spec.remove(QStringLiteral("snapshotFile"));
    spec.insert(QStringLiteral("snapshotReady"), false);
    const int copy = enqueue(j.value(QStringLiteral("name")).toString(), path, spec,
                             j.value(QStringLiteral("preset")).toString(), j.value(QStringLiteral("warnings")).toStringList());
    if (copy >= 0)
        emit snapshotNeeded(copy);
    return copy;
}

void RenderQueue::moveJob(int id, int delta) {
    const int i = indexOf(id);
    if (i < 0 || delta == 0)
        return;
    const int to = qBound(0, i + delta, m_jobs.size() - 1);
    if (to == i)
        return;
    m_jobs.move(i, to);
    touch();
}

void RenderQueue::clearFinished() {
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        const QString s = m_jobs.at(i).toMap().value(QStringLiteral("status")).toString();
        if (s == QStringLiteral("completed") || s == QStringLiteral("warning") || s == QStringLiteral("failed")
            || s == QStringLiteral("cancelled"))
            m_jobs.removeAt(i);
    }
    touch();
    cleanSnapshots();
}

void RenderQueue::clearAll() {
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        const int id = m_jobs.at(i).toMap().value(QStringLiteral("id")).toInt();
        if (id == m_activeJob)
            continue;
        m_jobs.removeAt(i);
        m_snapshotVersions.remove(id);
    }
    touch();
    cleanSnapshots();
}

void RenderQueue::load() {
    QSettings s = appSettings();
    QDir().mkpath(QFileInfo(s.fileName()).absolutePath());
    const auto claim = [&](const QString &group) {
        const auto key = QCryptographicHash::hash(group.toUtf8(), QCryptographicHash::Sha256).toHex();
        auto lock = std::make_unique<QLockFile>(s.fileName() + ".render-queue-" + QString::fromLatin1(key) + ".lock");
        lock->setStaleLockTime(0); // age alone never makes a live editor stale
        if (!lock->tryLock()) lock.reset();
        return lock;
    };
    m_queueLock = claim({});
    if (!m_queueLock) {
        m_settingsGroup = "renderQueueSessions/" + m_snapshotNamespace;
        m_queueLock = claim(m_settingsGroup);
    }
    if (!m_queueLock) {
        m_storageError = tr("The render queue cannot be saved. Check access to your settings folder.");
        return;
    }
    s.sync();
    const auto prefix = m_settingsGroup.isEmpty() ? QString() : m_settingsGroup + '/';
    m_jobs = jsonValue(s.value(prefix + "renderQueue").toString()).toList();
    m_afterCompletion = s.value(prefix + "renderAfterCompletion", QStringLiteral("none")).toString();
    int maxId = 0;
    for (const auto &v : m_jobs) maxId = qMax(maxId, v.toMap().value("id").toInt());
    // Keep leases until the merge and removal are published together by
    // QSettings' atomic sync. Live windows never share mutable queue records.
    std::vector<std::unique_ptr<QLockFile>> recovered;
    QStringList recoveredGroups;
    if (m_settingsGroup.isEmpty()) {
        s.beginGroup("renderQueueSessions");
        const auto groups = s.childGroups();
        s.endGroup();
        for (const auto &id : groups) {
            if (QUuid(id).isNull()) continue;
            const auto group = "renderQueueSessions/" + id;
            auto lock = claim(group);
            if (!lock) continue;
            // The owner may have saved and exited after our initial sync.
            // Read its final records only after acquiring its lease.
            s.sync();
            for (const auto &value : jsonValue(s.value(group + "/renderQueue").toString()).toList()) {
                auto job = value.toMap();
                job["id"] = ++maxId;
                m_jobs.append(job);
            }
            recoveredGroups.append(group);
            recovered.push_back(std::move(lock));
        }
    }
    for (int i = 0; i < m_jobs.size(); ++i) {
        QVariantMap j = m_jobs.at(i).toMap();
        maxId = qMax(maxId, j.value(QStringLiteral("id")).toInt());
        auto settings = j.value("spec").toMap();
        if (!settings.contains("overwritePaths")) settings["overwritePaths"] = QStringList();
        j["spec"] = settings;
        const QString st = j.value(QStringLiteral("status")).toString();
        if (st == QStringLiteral("rendering") || st == QStringLiteral("paused")) {
            // Caught mid-render at the last shutdown: it starts over.
            j.insert(QStringLiteral("status"), QStringLiteral("queued"));
            j.insert(QStringLiteral("percent"), 0);
            // Older versions put temporary files beside the output. Their
            // names do not prove ownership, so leave them untouched.
        }
        if(!j.value("spec").toMap().value("snapshotReady",true).toBool()) {j["status"]="failed";j["error"]="The application closed before the project snapshot was captured. Add this job again.";}
        m_jobs[i] = j;
    }
    m_nextId = maxId + 1;
    // Snapshots these jobs name came from earlier sessions: adopt them, so they
    // go once no job references them (trimmed history, Clear Finished). Files
    // no job names are left alone; another instance may be writing them.
    const QString snapshots = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                              + QStringLiteral("/render-snapshots/");
    for (const auto &value : m_jobs) {
        const QString file = value.toMap().value(QStringLiteral("spec")).toMap().value(QStringLiteral("snapshotFile")).toString();
        if (!file.isEmpty() && QFileInfo(file).absoluteFilePath().startsWith(snapshots))
            m_snapshotFiles.insert(file);
    }
    // Keep history short.
    int history=0;
    for(int i=m_jobs.size()-1;i>=0;--i){const QString state=m_jobs[i].toMap().value("status").toString();
        if(state!="queued"&&state!="rendering"&&state!="paused"&&++history>60)m_jobs.removeAt(i);}
    if (!recovered.empty()) {
        for (const auto &group : recoveredGroups) s.remove(group);
        s.setValue("renderQueue", jsonText(m_jobs));
        s.sync();
    }
}

void RenderQueue::save() {
    if (!m_queueLock || !m_queueLock->isLocked()) return;
    auto state = m_saveState;
    {
        QMutexLocker lock(&state->mutex);
        state->jobs = m_jobs;
        state->afterCompletion = m_afterCompletion;
        state->group = m_settingsGroup;
        state->pending = true;
        if (state->scheduled) return; // the queued worker job takes this newer state
        state->scheduled = true;
    }
    // Trimming the logs, encoding the JSON (caption cues can be long) and the
    // settings write run on their own serial worker, so a status change never
    // waits behind a large snapshot write. A job only names its snapshot file
    // once that file is written; cleanup waits for these saves.
    m_savePool.start(QRunnable::create([this, state] {
        QVariantList jobs;
        QString afterCompletion, group;
        {
            QMutexLocker lock(&state->mutex);
            state->scheduled = false;
            if (!state->pending) return;
            state->pending = false;
            jobs = std::move(state->jobs);
            state->jobs = QVariantList();
            afterCompletion = state->afterCompletion;
            group = state->group;
        }
        // The caption cues can be long; they are the job's business only.
        QVariantList slim;
        for (const QVariant &v : std::as_const(jobs)) {
            QVariantMap j = v.toMap();
            QStringList log = j.value(QStringLiteral("log")).toStringList();
            if (log.size() > 60)
                log = log.mid(log.size() - 60);
            j.insert(QStringLiteral("log"), log);
            slim.append(j);
        }
        const QString text = jsonText(slim);
        QSettings s = appSettings();
        if (!group.isEmpty()) s.beginGroup(group);
        s.setValue(QStringLiteral("renderQueue"), text);
        s.setValue(QStringLiteral("renderAfterCompletion"), afterCompletion);
        s.sync();
        if (s.status() != QSettings::NoError)
            QMetaObject::invokeMethod(this, [this] {
                emit failed(tr("Could not save the render queue. Check free space and access to your settings folder."));
            }, Qt::QueuedConnection);
    }));
}
