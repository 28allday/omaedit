#include "exportspec.h"
#include "clipcategories.h"
#include <QLocale>

#include <QCoreApplication>
#include <QDate>
#include <QRegularExpression>
#include <cmath>
#include <numeric>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QMutex>
#include <QStandardPaths>
#include <QThread>
#include <QTime>
#include <QtMath>


#include <cstdarg>
#include <cstdio>
#include <utility>

namespace ExportSpec {

QString outputKey(const QString &path) {
    const QFileInfo info(path);
    const auto parent = info.dir().canonicalPath();
    return QDir::cleanPath((parent.isEmpty() ? info.absolutePath() : parent) + '/' + info.fileName());
}

QStringList outputPaths(const QString &path, const QVariantMap &spec) {
    QStringList result{path};
    const auto mode = spec.value("subtitles").toString();
    const bool embed = spec.value("video", true).toBool() &&
        QStringList{"mp4", "mov", "mkv", "webm"}.contains(spec.value("container", "mp4").toString());
    if (!spec.value("captionCues").toList().isEmpty() && (mode == "sidecar" || (mode == "embed" && !embed))) {
        const QFileInfo file(path);
        result.append(file.dir().filePath(file.completeBaseName() + ".srt"));
    }
    return result;
}

QVariantList checkOutputs(const QVariantList &jobs, const QStringList &reserved) {
    QSet<QString> seen;
    for (const auto &path : reserved) seen.insert(outputKey(path));
    QVariantList checks;
    for (const auto &value : jobs) {
        const auto job = value.toMap();
        for (const auto &path : outputPaths(job.value("path").toString(), job.value("spec").toMap())) {
            const QFileInfo file(path);
            const auto key = outputKey(path);
            QVariantMap row{{"level", "ok"}, {"text", QCoreApplication::translate("ExportSpec", "Save location is writable")}};
            if (seen.contains(key)) row = {{"level", "error"}, {"text", QCoreApplication::translate("ExportSpec", "Another render already targets %1").arg(path)}};
            else if (path.isEmpty()) row = {{"level", "error"}, {"text", QCoreApplication::translate("ExportSpec", "No file name")}};
            else if (!file.dir().exists()) row = {{"level", "error"}, {"text", QCoreApplication::translate("ExportSpec", "Save location does not exist")}};
            else if (!QFileInfo(file.absolutePath()).isWritable()) row = {{"level", "error"}, {"text", QCoreApplication::translate("ExportSpec", "Save location is not writable")}};
            else if (file.isDir()) row = {{"level", "error"}, {"text", QCoreApplication::translate("ExportSpec", "A folder occupies %1").arg(path)}};
            else if (file.exists() || file.isSymLink()) row = {{"level", "warning"},
                {"text", QCoreApplication::translate("ExportSpec", "%1 exists and will be replaced").arg(path)}, {"overwritePath", key}};
            seen.insert(key);
            if (row.value("level") != "ok" || checks.isEmpty()) checks.append(row);
        }
    }
    return checks;
}

bool publishOutput(const QString &temporary, const QString &path, const QVariantMap &spec, QString *error) {
    // Legacy direct worker calls specify their destination themselves. Queued
    // jobs always carry the exact overwrite paths acknowledged at enqueue time.
    const bool replace = !spec.contains("overwritePaths") || spec.value("overwritePaths").toStringList().contains(outputKey(path));
    const bool ok = replace ? std::rename(QFile::encodeName(temporary).constData(), QFile::encodeName(path).constData()) == 0
                            : QFile::rename(temporary, path); // refuses an existing destination, including late arrivals
    if (!ok && error) *error = QCoreApplication::translate("ExportSpec", "Could not move the render into place: %1. The destination may already exist; review it before retrying.").arg(path);
    return ok;
}

// FFmpeg reads "23.976" as 23976/1000 and stamps a non-standard timebase on
// the file; the NTSC family must travel as its exact rational.
static QString rationalRate(double fps) {
    struct Rate { double value; const char *rational; };
    static const Rate ntsc[] = {{23.976, "24000/1001"}, {29.97, "30000/1001"}, {47.952, "48000/1001"},
                                {59.94, "60000/1001"}, {119.88, "120000/1001"}};
    for (const Rate &r : ntsc)
        if (qAbs(fps - r.value) < 0.01) return QString::fromLatin1(r.rational);
    return QString::number(fps, 'f', 3);
}

static QString str(const QVariantMap &m, const char *key, const QString &fallback = QString()) {
    const QString v = m.value(QLatin1String(key)).toString();
    return v.isEmpty() ? fallback : v;
}

QVariantList jobs(const QString &name,const QString &path,const QVariantMap &input,int frames,double fps,const QVariantList &tracks,QString *error,const QVariantList &categories) {
    const auto spec=normalized(input);QVariantList result;
    const int start=qMax(0,spec.value("rangeIn",-1).toInt());
    const int end=spec.value("rangeOut",-1).toInt()<0?frames-1:qMin(frames-1,spec.value("rangeOut").toInt());
    const double seconds=spec.value("splitSeconds").toDouble();
    if(!std::isfinite(seconds)||seconds<0||seconds>86400||fps<=0||start>end){if(error)*error=QCoreApplication::translate("ExportSpec", "The render range or split duration is invalid.");return {};}
    const int split=seconds>0?qMax(1,qRound(qMin(seconds,86400.)*fps)):end-start+1;
    // (`stem`: the track or category a stem job holds, as the editor named it.)
    auto add=[&](const QString &jobName,const QString &jobPath,QVariantMap settings,const QString &stem=QString()){
        settings.remove("stems");settings.remove("stemsBy");settings.remove("splitSeconds");
        QVariantMap job{{"name",jobName},{"path",jobPath},{"spec",settings}};
        if(!stem.isEmpty())job["stem"]=stem;
        result.append(job);
    };
    const QFileInfo info(path);
    // A stem's name as part of a file's: letters and digits of any script
    // stay, what no file system takes is a hyphen (as in the file's own
    // name), the rest an underscore, made tidy as every render's name is;
    // two that come out alike are told apart by a number, so no stem is
    // written over another.
    const auto labelOf=[](const QString &shown){
        QString label=shown;
        for(const QChar c:QStringLiteral("/\\:*?\"<>|"))label.replace(c,QLatin1Char('-'));
        static const QRegularExpression other("[^\\p{L}\\p{N}_-]",QRegularExpression::UseUnicodePropertiesOption);
        label.replace(other,"_");
        return tidyFileName(label);
    };
    QStringList labelsUsed;
    const auto fileLabel=[&](const QString &shown){
        QString label=labelOf(shown);
        if(label.isEmpty())label="stem";
        QString unique=label;for(int n=2;labelsUsed.contains(unique.toCaseFolded());++n)unique=label+"_"+QString::number(n);
        labelsUsed.append(unique.toCaseFolded());return unique;
    };
    const auto selected=spec.value("audioTracks").toList();
    if (spec.value("audio").toBool() && spec.value("audioSelection")=="custom" && selected.isEmpty()) { if(error)*error=QCoreApplication::translate("ExportSpec", "Select at least one audio track."); return {}; }
    for(int a=start,part=1;a<=end;a+=split,++part){
        if(result.size()>=1000){if(error)*error=QCoreApplication::translate("ExportSpec", "This batch exceeds 1,000 jobs; increase the split duration.");return {};}
        auto settings=spec;settings["rangeIn"]=a;settings["rangeOut"]=qMin(end,a+split-1);
        const QString suffix=split<end-start+1?QString("_part%1").arg(part,3,10,QChar('0')):QString();
        const QString base=info.dir().filePath(info.completeBaseName()+suffix);
        add(name+suffix,base+"."+info.suffix(),settings);
        labelsUsed.clear();
        const bool byCategory=spec.value("stemsBy").toString()=="category";
        // (The stem of the sounds with no category is named first: a
        // category of the project's own with its name takes the number.)
        QStringList labels(categories.size());
        for(const bool none:{true,false})for(int i=0;i<categories.size();++i){
            const auto category=categories.at(i).toMap();
            if((category.value("id").toString()==ClipCategories::uncategorisedStem())==none)labels[i]=fileLabel(category.value("name").toString());
        }
        if(byCategory&&spec.value("stems").toBool()&&spec.value("audio").toBool())for(int i=0;i<categories.size();++i){
            const auto category=categories.at(i).toMap();
            const QString &label=labels.at(i);
            auto stem=settings;stem["container"]="wav";stem["video"]=false;stem["audio"]=true;stem["acodec"]="pcm24";stem["channels"]=2;stem["loudness"]=false;
            stem["subtitles"]="none";stem["audioCategory"]=category.value("id");stem["categoryList"]=categories;stem["audioTrackMap"]=QVariantMap();
            stem["includeMaster"]=spec.value("stemsMaster",false);stem["hw"]="software";
            if(result.size()>=1000){if(error)*error=QCoreApplication::translate("ExportSpec", "This batch exceeds 1,000 jobs; increase the split duration.");return {};}
            add(name+suffix+"_"+label,base+"_"+label+".wav",stem,category.value("name").toString());
        }
        if(!byCategory&&spec.value("stems").toBool()&&spec.value("audio").toBool())for(const auto &v:tracks){
            const auto track=v.toMap();const int index=track.value("index").toInt();
            if(track.value("clips").toInt()<=0 || (track.contains("hasAudio")&&!track.value("hasAudio").toBool()))continue;
            bool include=selected.isEmpty();for(const auto &n:selected)include=include||n.toInt()==index;if(!include)continue;
            const QString named=labelOf(track.value("name").toString());
            const QString label=named.isEmpty()?QString("_T%1").arg(index):QString("_T%1_%2").arg(index).arg(named);
            auto stem=settings;stem["container"]="wav";stem["video"]=false;stem["audio"]=true;stem["acodec"]="pcm24";stem["channels"]=2;stem["loudness"]=false;
            stem["subtitles"]="none";stem["audioTracks"]=QVariantList{index};stem["audioTrackMap"]=QVariantMap();
            stem["includeMaster"]=spec.value("stemsMaster",false);stem["hw"]="software";
            if(result.size()>=1000){if(error)*error=QCoreApplication::translate("ExportSpec", "This batch exceeds 1,000 jobs; increase the split duration.");return {};}
            add(name+suffix+label,base+label+".wav",stem,track.value("name").toString());
        }
    }
    return result;
}

QVariantMap normalized(const QVariantMap &in) {
    QVariantMap s = in;
    auto def = [&](const char *key, const QVariant &value) {
        if (!s.contains(QLatin1String(key)) || !s.value(QLatin1String(key)).isValid()
            || (s.value(QLatin1String(key)).typeId() == QMetaType::QString
                && s.value(QLatin1String(key)).toString().isEmpty()
                && value.typeId() != QMetaType::QString))
            s.insert(QLatin1String(key), value);
    };
    def("container", QStringLiteral("mp4"));
    def("video", true);
    def("audio", true);
    def("vcodec", QStringLiteral("h264"));
    def("hw", QStringLiteral("software"));
    def("softwareFallback", true);
    def("splitSeconds", 0.0);
    def("stems", false);
    def("stemsBy", QStringLiteral("track"));
    def("includeMaster", true);
    def("stemsMaster", false);
    def("width", 0);
    def("height", 0);
    def("fps", 0.0);
    def("par", 1.0);
    def("scan", QStringLiteral("progressive"));
    def("rateMode", QStringLiteral("quality"));
    def("quality", 18);
    def("bitrate", 0);
    def("maxrate", 0);
    def("vprofile", QString());
    def("level", QString());
    def("gop", 0);
    // Keyframe spacing in seconds when no frame count is set (0: two
    // seconds); B-frames between references (-1: the encoder's own).
    def("gopSeconds", 0.0);
    def("bframes", -1);
    // The highest frame rate the destination takes (0: any): a faster edit
    // is delivered at the nearest whole division of its rate under it.
    def("maxFps", 0.0);
    def("bitDepth", 8);
    def("alpha", false);
    def("colorSpace", QStringLiteral("rec709"));
    def("toneMap", QString());
    def("range", QStringLiteral("tv"));
    def("acodec", QStringLiteral("aac"));
    def("arate", 48000);
    def("abitrate", 192);
    def("channels", 2);
    def("subtitles", QStringLiteral("none"));
    def("rangeIn", -1);
    def("rangeOut", -1);
    def("handles", 0);
    def("captionTrack", -1);
    // Where the file is going ("youtube", "instagram-reels", ...; empty:
    // anywhere): the Deliver checks hold it to that destination's limits.
    def("platform", QString());
    // Another shape than the edit's: "fit" (black bars), "fill" (cropped),
    // "blur" (fitted over a blurred copy); empty until chosen (fitted).
    def("framing", QString());
    // Level the delivered sound to a loudness target with a true-peak
    // ceiling, measured on the render itself; the project's mix is untouched.
    def("loudness", false);
    def("loudnessTarget", -14.0);
    def("truePeak", -1.0);
    if (!s.value(QStringLiteral("metadata")).canConvert<QVariantMap>())
        s.insert(QStringLiteral("metadata"), QVariantMap());
    if (!s.value(QStringLiteral("hdr")).canConvert<QVariantMap>())
        s.insert(QStringLiteral("hdr"), QVariantMap());
    // APV's profile names its depth: the settings say the depth it writes.
    if (s.value(QStringLiteral("vcodec")).toString() == QStringLiteral("apv"))
        s.insert(QStringLiteral("bitDepth"), apvProfile(s).endsWith(QStringLiteral("12")) ? 12 : 10);
    // Audio-only containers carry no picture whatever the toggle says.
    const QString c = s.value(QStringLiteral("container")).toString();
    if (c == QStringLiteral("m4a") || c == QStringLiteral("wav") || c == QStringLiteral("mp3")
        || c == QStringLiteral("flac"))
        s.insert(QStringLiteral("video"), false);
    if (c == QStringLiteral("wav"))
        s.insert(QStringLiteral("acodec"), s.value(QStringLiteral("acodec")).toString().startsWith(QStringLiteral("pcm"))
                                               ? s.value(QStringLiteral("acodec")) : QStringLiteral("pcm16"));
    if (c == QStringLiteral("mp3"))
        s.insert(QStringLiteral("acodec"), QStringLiteral("mp3"));
    if (c == QStringLiteral("flac"))
        s.insert(QStringLiteral("acodec"), QStringLiteral("flac"));
    if (c == QStringLiteral("m4a"))
        s.insert(QStringLiteral("acodec"), QStringLiteral("aac"));
    // A numbered image sequence: pictures only, at the timeline's rate, each
    // a file of the kind its container allows.
    if (c == QStringLiteral("png") || c == QStringLiteral("tiff") || c == QStringLiteral("exr") || c == QStringLiteral("jpg")) {
        const QString kind = s.value(QStringLiteral("vcodec")).toString();
        s.insert(QStringLiteral("vcodec"), c == QStringLiteral("png") ? (kind == QStringLiteral("png8") ? kind : QStringLiteral("png16"))
                                           : c == QStringLiteral("tiff") ? QStringLiteral("tiff16")
                                           : c == QStringLiteral("exr") ? QStringLiteral("exr") : QStringLiteral("jpeg"));
        s.insert(QStringLiteral("video"), true);
        s.insert(QStringLiteral("audio"), false);
        s.insert(QStringLiteral("subtitles"), QStringLiteral("none"));
        s.insert(QStringLiteral("hw"), QStringLiteral("software"));
        s.insert(QStringLiteral("fps"), 0.0);
        s.insert(QStringLiteral("maxFps"), 0.0);
        s.insert(QStringLiteral("loudness"), false);
        if (c == QStringLiteral("jpg")) s.insert(QStringLiteral("alpha"), false);
    }
    return s;
}

QString sequenceFormat(const QVariantMap &spec) {
    const QString c = spec.value(QStringLiteral("container")).toString();
    if (c == QStringLiteral("png")) return spec.value(QStringLiteral("vcodec")).toString() == QStringLiteral("png8") ? QStringLiteral("png8") : QStringLiteral("png16");
    if (c == QStringLiteral("tiff")) return QStringLiteral("tiff16");
    if (c == QStringLiteral("exr")) return QStringLiteral("exr");
    if (c == QStringLiteral("jpg")) return QStringLiteral("jpeg");
    return {};
}

QString sequenceFileName(const QString &stem, int frame, const QString &format) {
    const QString suffix = format.startsWith(QLatin1String("png")) ? QStringLiteral("png") : format == QLatin1String("tiff16") ? QStringLiteral("tif")
                           : format == QLatin1String("exr") ? QStringLiteral("exr") : QStringLiteral("jpg");
    return QStringLiteral("%1_%2.%3").arg(stem.isEmpty() ? QStringLiteral("frame") : stem).arg(qMax(0, frame), 6, 10, QLatin1Char('0')).arg(suffix);
}

QVariantMap fromLegacyPreset(const QString &preset) {
    QVariantMap s;
    if (preset == QStringLiteral("prores")) {
        s.insert(QStringLiteral("container"), QStringLiteral("mov"));
        s.insert(QStringLiteral("vcodec"), QStringLiteral("prores"));
        s.insert(QStringLiteral("vprofile"), QStringLiteral("hq"));
        s.insert(QStringLiteral("bitDepth"), 10);
        s.insert(QStringLiteral("acodec"), QStringLiteral("pcm16"));
    } else if (preset == QStringLiteral("audio")) {
        s.insert(QStringLiteral("container"), QStringLiteral("m4a"));
        s.insert(QStringLiteral("video"), false);
        s.insert(QStringLiteral("acodec"), QStringLiteral("aac"));
        s.insert(QStringLiteral("abitrate"), 256);
    } else {
        s.insert(QStringLiteral("container"), QStringLiteral("mp4"));
        s.insert(QStringLiteral("vcodec"), QStringLiteral("h264"));
        s.insert(QStringLiteral("quality"), preset == QStringLiteral("h264-medium") ? 23 : 18);
        s.insert(QStringLiteral("acodec"), QStringLiteral("aac"));
        s.insert(QStringLiteral("abitrate"), 192);
    }
    return normalized(s);
}

QString extension(const QVariantMap &spec) {
    // A sequence is a folder of files: its name has no suffix.
    if (!sequenceFormat(spec).isEmpty()) return {};
    return QLatin1Char('.') + str(spec, "container", QStringLiteral("mp4"));
}

QString videoEncoder(const QVariantMap &spec) {
    const QString v = str(spec, "vcodec", QStringLiteral("h264"));
    const QString hw = str(spec, "hw", QStringLiteral("software"));
    if (v == QStringLiteral("h264"))
        return hw == QStringLiteral("nvenc") ? QStringLiteral("h264_nvenc")
             : hw == QStringLiteral("vaapi") ? QStringLiteral("h264_vaapi")
                                             : QStringLiteral("libx264");
    if (v == QStringLiteral("hevc"))
        return hw == QStringLiteral("nvenc") ? QStringLiteral("hevc_nvenc")
             : hw == QStringLiteral("vaapi") ? QStringLiteral("hevc_vaapi")
                                             : QStringLiteral("libx265");
    if (v == QStringLiteral("av1"))
        return hw == QStringLiteral("nvenc") ? QStringLiteral("av1_nvenc")
             : hw == QStringLiteral("vaapi") ? QStringLiteral("av1_vaapi")
                                             : QStringLiteral("libsvtav1");
    if (v == QStringLiteral("vp9"))
        return QStringLiteral("libvpx-vp9");
    if (v == QStringLiteral("prores"))
        return hw == "vulkan" ? QStringLiteral("prores_ks_vulkan") : QStringLiteral("prores_ks");
    if (v == QStringLiteral("dnxhr"))
        return QStringLiteral("dnxhd");
    if (v == QStringLiteral("mpeg2"))
        return QStringLiteral("mpeg2video");
    if (v == QStringLiteral("ffv1"))
        return QStringLiteral("ffv1");
    if (v == QStringLiteral("apv"))
        return QStringLiteral("oapv_app_enc");
    return QStringLiteral("libx264");
}

QString audioEncoder(const QVariantMap &spec) {
    const QString a = str(spec, "acodec", QStringLiteral("aac"));
    if (a == QStringLiteral("opus")) return QStringLiteral("libopus");
    if (a == QStringLiteral("mp3")) return QStringLiteral("libmp3lame");
    if (a == QStringLiteral("pcm16")) return QStringLiteral("pcm_s16le");
    if (a == QStringLiteral("pcm24")) return QStringLiteral("pcm_s24le");
    if (a == QStringLiteral("flac")) return QStringLiteral("flac");
    if (a == QStringLiteral("vorbis")) return QStringLiteral("libvorbis");
    return QStringLiteral("aac");
}

QString convertedRate(const QVariantMap &spec, double timelineFps) {
    double fps = spec.value(QStringLiteral("fps")).toDouble();
    const double most = spec.value(QStringLiteral("maxFps")).toDouble();
    if (fps <= 0.0 && most > 0.0 && timelineFps > most + 0.01)
        fps = timelineFps / std::ceil(timelineFps / most - 0.001);  // 120 → 60, 100 → 50, 119.88 → 59.94
    return fps > 0.0 && timelineFps > 0.0 && qAbs(fps - timelineFps) > 0.01 ? rationalRate(fps) : QString();
}


bool preciseMezzanine(const QVariantMap &spec) {
    const QString v = str(spec, "vcodec");
    const QString cs = str(spec, "colorSpace");
    return spec.value(QStringLiteral("bitDepth")).toInt() >= 10 || cs == QStringLiteral("pq") || cs == QStringLiteral("hlg")
           || v == QStringLiteral("prores") || v == QStringLiteral("apv");
}

bool embedsSubtitles(const QVariantMap &spec) {
    return str(spec, "subtitles") == QStringLiteral("embed")
           && !spec.value(QStringLiteral("captionCues")).toList().isEmpty();
}

// ── Colour ───────────────────────────────────────────────────────────────
struct ColourTags { QString primaries, trc, matrix; };
static ColourTags colourTags(const QString &space) {
    if (space == QStringLiteral("rec601")) return {QStringLiteral("bt470bg"), QStringLiteral("bt709"), QStringLiteral("bt470bg")};
    if (space == QStringLiteral("srgb")) return {QStringLiteral("bt709"), QStringLiteral("iec61966-2-1"), QStringLiteral("bt709")};
    if (space == QStringLiteral("rec2020")) return {QStringLiteral("bt2020"), QStringLiteral("bt2020-10"), QStringLiteral("bt2020nc")};
    if (space == QStringLiteral("pq")) return {QStringLiteral("bt2020"), QStringLiteral("smpte2084"), QStringLiteral("bt2020nc")};
    if (space == QStringLiteral("hlg")) return {QStringLiteral("bt2020"), QStringLiteral("arib-std-b67"), QStringLiteral("bt2020nc")};
    return {QStringLiteral("bt709"), QStringLiteral("bt709"), QStringLiteral("bt709")};
}

// ProRes keeps its own colour fields in every frame header, and decoders read
// those, not the container's. FFmpeg's ProRes encoder fills them from the
// pictures, which the sink hands over untagged: an untagged BT.709 export
// decodes with the BT.601 matrix. The header names fewer values than the
// container: BT.601's matrix as smpte170m (the same matrix), Rec.2020's
// 10-bit transfer as bt709 (the same curve), sRGB's transfer as unknown.
static ColourTags proresHeaderTags(const QVariantMap &spec) {
    ColourTags ct = colourTags(str(spec, "colorSpace"));
    ct.trc = ct.trc == QStringLiteral("bt2020-10") ? QStringLiteral("bt709")
           : ct.trc == QStringLiteral("iec61966-2-1") ? QStringLiteral("unknown") : ct.trc;
    if (ct.matrix == QStringLiteral("bt470bg")) ct.matrix = QStringLiteral("smpte170m");
    return ct;
}


// x265's mastering display, G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min). It goes into
// x265-params, which splits on ':', so nothing but that exact form may pass:
// a preset file could otherwise add any x265 option (one that writes a file).
static bool validMasterDisplay(const QString &value) {
    static const QRegularExpression form(QStringLiteral(
        "^G\\(\\d+,\\d+\\)B\\(\\d+,\\d+\\)R\\(\\d+,\\d+\\)WP\\(\\d+,\\d+\\)L\\(\\d+,\\d+\\)$"));
    return form.match(value).hasMatch();
}

static bool isHdr(const QVariantMap &spec) {
    const QString cs = str(spec, "colorSpace");
    return cs == QStringLiteral("pq") || cs == QStringLiteral("hlg");
}

bool lossySound(const QVariantMap &spec) {
    const QString a = str(spec, "acodec", QStringLiteral("aac"));
    return a == QStringLiteral("aac") || a == QStringLiteral("opus") || a == QStringLiteral("mp3") || a == QStringLiteral("vorbis");
}

double promisedPeak(const QVariantMap &spec) {
    return qBound(-12.0, spec.value(QStringLiteral("truePeak"), -1.0).toDouble(), 0.0);
}

double levellingCeiling(const QVariantMap &spec) {
    // The ceiling the delivery's own sound check settled on, when it has run.
    const double checked = spec.value(QStringLiteral("loudnessCeiling"), qQNaN()).toDouble();
    if (std::isfinite(checked)) return qBound(-36.0, checked, promisedPeak(spec));
    // A lossy codec rebuilds the sound with peaks over what went in: a
    // start with some room, which the check of the encoded sound then
    // lowers as far as that sound needs (see PlayerWorker::startExportSink).
    return lossySound(spec) ? promisedPeak(spec) - 0.5 : promisedPeak(spec);
}

QString apvProfile(const QVariantMap &spec) {
    static const QStringList profiles{QStringLiteral("422-10"), QStringLiteral("422-12"), QStringLiteral("444-10"),
                                      QStringLiteral("444-12"), QStringLiteral("4444-10"), QStringLiteral("4444-12")};
    const QString vp = str(spec, "vprofile");
    if (profiles.contains(vp))
        return vp;
    return (spec.value(QStringLiteral("alpha")).toBool() ? QStringLiteral("4444-") : QStringLiteral("422-"))
           + (spec.value(QStringLiteral("bitDepth")).toInt() >= 12 ? QStringLiteral("12") : QStringLiteral("10"));
}

static QString apvPixelFormat(const QString &profile) {
    const QString depth = profile.endsWith(QStringLiteral("12")) ? QStringLiteral("12le") : QStringLiteral("10le");
    if (profile.startsWith(QStringLiteral("4444")))
        return QStringLiteral("yuva444p") + depth;
    if (profile.startsWith(QStringLiteral("444")))
        return QStringLiteral("yuv444p") + depth;
    return QStringLiteral("yuv422p") + depth;
}

static QString pixelFormat(const QVariantMap &spec) {
    const QString v = str(spec, "vcodec", QStringLiteral("h264"));
    if (v == QStringLiteral("apv"))
        return apvPixelFormat(apvProfile(spec));
    const bool ten = spec.value(QStringLiteral("bitDepth")).toInt() >= 10 || isHdr(spec);
    const bool alpha = spec.value(QStringLiteral("alpha")).toBool();
    const QString vp = str(spec, "vprofile");
    if (v == QStringLiteral("prores")) {
        if (vp == QStringLiteral("4444") || vp == QStringLiteral("4444xq"))
            return alpha ? QStringLiteral("yuva444p10le") : QStringLiteral("yuv444p10le");
        return QStringLiteral("yuv422p10le");
    }
    if (v == QStringLiteral("dnxhr")) {
        if (vp == QStringLiteral("444")) return QStringLiteral("yuv444p10le");
        if (vp == QStringLiteral("hqx")) return QStringLiteral("yuv422p10le");
        return QStringLiteral("yuv422p");
    }
    if (v == QStringLiteral("ffv1"))
        return alpha ? (ten ? QStringLiteral("yuva444p10le") : QStringLiteral("yuva420p")) : (ten ? QStringLiteral("yuv422p10le") : QStringLiteral("yuv420p"));
    if (v == QStringLiteral("vp9"))
        return alpha ? QStringLiteral("yuva420p") : (ten ? QStringLiteral("yuv420p10le") : QStringLiteral("yuv420p"));
    if (v == QStringLiteral("mpeg2"))
        return vp == QStringLiteral("422") ? QStringLiteral("yuv422p") : QStringLiteral("yuv420p");
    if (str(spec, "hw") == QStringLiteral("nvenc"))
        return ten ? QStringLiteral("p010le") : QStringLiteral("yuv420p");
    return ten ? QStringLiteral("yuv420p10le") : QStringLiteral("yuv420p");
}

static int proresProfileNumber(const QString &vp) {
    if (vp == QStringLiteral("proxy")) return 0;
    if (vp == QStringLiteral("lt")) return 1;
    if (vp == QStringLiteral("standard")) return 2;
    if (vp == QStringLiteral("4444")) return 4;
    if (vp == QStringLiteral("4444xq")) return 5;
    return 3; // hq
}

static QString dnxProfile(const QString &vp) {
    if (vp == QStringLiteral("lb")) return QStringLiteral("dnxhr_lb");
    if (vp == QStringLiteral("sq")) return QStringLiteral("dnxhr_sq");
    if (vp == QStringLiteral("hqx")) return QStringLiteral("dnxhr_hqx");
    if (vp == QStringLiteral("444")) return QStringLiteral("dnxhr_444");
    return QStringLiteral("dnxhr_hq");
}

// The per-encoder video options, as (key, value) pairs shared by the encoder
// sink (AVOptions) and ffmpeg (-key value) — the names are the
// AVOption names both understand.
static QList<QPair<QString, QString>> videoOptions(const QVariantMap &spec, double fps) {
    QList<QPair<QString, QString>> o;
    const QString v = str(spec, "vcodec", QStringLiteral("h264"));
    const QString hw = str(spec, "hw", QStringLiteral("software"));
    const QString mode = str(spec, "rateMode", QStringLiteral("quality"));
    const int q = qBound(0, spec.value(QStringLiteral("quality"), 18).toInt(), 51);
    const int kbps = spec.value(QStringLiteral("bitrate")).toInt();
    const int maxk = spec.value(QStringLiteral("maxrate")).toInt();
    const QString vp = str(spec, "vprofile");
    const QString level = str(spec, "level");
    int gop = spec.value(QStringLiteral("gop")).toInt();
    if (gop <= 0) {
        const double seconds = spec.value(QStringLiteral("gopSeconds")).toDouble();
        gop = qMax(1, qRound(fps * (seconds > 0 ? seconds : 2.0)));
    }
    const int bframes = spec.value(QStringLiteral("bframes"), -1).toInt();
    auto add = [&](const char *k, const QString &val) { o.append({QLatin1String(k), val}); };
    const QString kb = QString::number(kbps) + QStringLiteral("k");
    const QString mk = QString::number(maxk > 0 ? maxk : kbps) + QStringLiteral("k");

    if (v == QStringLiteral("prores")) {
        add("profile", QString::number(proresProfileNumber(vp)));
        add("vendor", QStringLiteral("apl0"));
        return o;
    }
    if (v == QStringLiteral("dnxhr")) {
        add("profile", dnxProfile(vp));
        return o;
    }
    if (v == QStringLiteral("ffv1")) {
        add("level", QStringLiteral("3"));
        add("g", QStringLiteral("1"));
        return o;
    }
    if (v == QStringLiteral("mpeg2")) {
        // 4:2:2 is its own profile (0), at high level (2) for HD and main
        // (5) below, as the standard numbers them for that profile.
        const bool fourTwoTwo = vp == QStringLiteral("422");
        if (fourTwoTwo) {
            add("profile", QStringLiteral("0"));
            add("level", spec.value(QStringLiteral("height")).toInt() > 0 && spec.value(QStringLiteral("height")).toInt() <= 608
                             && spec.value(QStringLiteral("width")).toInt() <= 720 ? QStringLiteral("5") : QStringLiteral("2"));
        }
        if (mode == QStringLiteral("quality")) {
            add("qscale", QString::number(2 + q * 29 / 51));
            add("g", QString::number(gop));
            return o;
        }
        add("b", kbps > 0 ? kb : QStringLiteral("15000k"));
        const int target = kbps > 0 ? kbps : 15000;
        const int peak = mode == QStringLiteral("cbr") ? target : qMax(target, maxk > 0 ? maxk : 20000);
        if (mode == QStringLiteral("cbr")) add("minrate", QString::number(target) + "k");
        add("maxrate", QString::number(peak) + "k");
        // Two seconds' worth of buffer, inside what 4:2:2 at high level
        // allows a decoder to hold (47,185,920 bits).
        const qint64 buffer = qint64(peak) * 2000;
        add("bufsize", QString::number(fourTwoTwo ? qMin<qint64>(buffer, 47185920) : buffer));
        add("g", QString::number(gop));
        return o;
    }
    if (v == QStringLiteral("vp9")) {
        if (mode == QStringLiteral("quality")) {
            add("crf", QString::number(qBound(0, q * 63 / 51, 63)));
            add("b", QStringLiteral("0"));
        } else {
            add("b", kb);
            if (mode == QStringLiteral("cbr")) { add("minrate", kb); add("maxrate", kb); }
            else if (maxk > 0) add("maxrate", mk);
        }
        add("g", QString::number(gop));
        add("row-mt", QStringLiteral("1"));
        add("deadline", QStringLiteral("good"));
        add("cpu-used", QStringLiteral("2"));
        return o;
    }
    if (hw == QStringLiteral("nvenc")) {
        add("preset", QStringLiteral("p5"));
        if (mode == QStringLiteral("quality")) {
            add("rc", QStringLiteral("vbr"));
            add("cq", QString::number(q));
            add("b", QStringLiteral("0"));
        } else {
            add("rc", mode == QStringLiteral("cbr") ? QStringLiteral("cbr") : QStringLiteral("vbr"));
            add("b", kb);
            add("maxrate", mode == QStringLiteral("cbr") ? kb : mk);
        }
        if (!vp.isEmpty() && v != QStringLiteral("av1"))
            add("profile", v == QStringLiteral("hevc") && vp == QStringLiteral("high10") ? QStringLiteral("main10") : vp);
        if (!level.isEmpty())
            add("level", level);
        add("g", QString::number(gop));
        return o;
    }
    if (hw == QStringLiteral("vaapi")) {
        if (mode == QStringLiteral("quality")) {
            add("rc_mode", QStringLiteral("CQP"));
            add("qp", QString::number(q));
        } else {
            add("rc_mode", mode == QStringLiteral("cbr") ? QStringLiteral("CBR") : QStringLiteral("VBR"));
            add("b", kb);
            add("maxrate", mode == QStringLiteral("cbr") ? kb : mk);
        }
        if (!vp.isEmpty())
            add("profile", vp);
        if (!level.isEmpty())
            add("level", level);
        add("g", QString::number(gop));
        if (bframes >= 0) add("bf", QString::number(bframes));
        return o;
    }
    if (v == QStringLiteral("av1")) {
        if (mode == QStringLiteral("quality")) {
            add("crf", QString::number(qBound(0, q * 63 / 51, 63)));
        } else {
            add("b", kb);
            if (mode == "cbr") {
                // SVT-AV1 CBR requires low-delay prediction, without the
                // max-bitrate parameter reserved for capped CRF.
                add("svtav1-params", "pred-struct=1:rc=2:max-bitrate=0");
                add("bufsize", QString::number(kbps * 2) + "k");
            }
        }
        add("preset", QStringLiteral("8"));
        add("g", QString::number(gop));
        return o;
    }
    // libx264 / libx265
    if (v == "h264" && str(spec, "scan", "progressive") != "progressive")
        add("x264-params", str(spec, "scan") == "bff" ? "bff=1" : "tff=1");
    // Left to itself x264 runs one and a half threads per core, each with
    // its own frames in flight: a 4K encode on 32 cores peaked at 4.4 GB.
    // Beyond 16 threads it gets no faster (16 took 2.4 GB and less time).
    // x265 sizes its own pool more frugally and is left alone.
    if (v == "h264")
        add("threads", QString::number(qBound(1, QThread::idealThreadCount(), 16)));
    add("preset", QStringLiteral("medium"));
    if (mode == QStringLiteral("quality")) {
        add("crf", QString::number(q));
        // A ceiling on top of the quality target (capped CRF): what a
        // destination that takes at most so many bits a second needs.
        if (maxk > 0) {
            add("maxrate", mk);
            add("bufsize", QString::number(maxk * 2) + QStringLiteral("k"));
        }
        if (bframes >= 0) add("bf", QString::number(bframes));
    } else {
        if (bframes >= 0) add("bf", QString::number(bframes));
        add("b", kb);
        if (mode == QStringLiteral("cbr")) {
            add("minrate", kb);
            add("maxrate", kb);
            add("bufsize", QString::number(kbps * 2) + QStringLiteral("k"));
        } else if (maxk > 0) {
            add("maxrate", mk);
            add("bufsize", QString::number(maxk * 2) + QStringLiteral("k"));
        }
    }
    if (!vp.isEmpty()) {
        if (v == QStringLiteral("hevc"))
            add("profile", vp == QStringLiteral("high10") || vp == QStringLiteral("main10")
                               ? QStringLiteral("main10") : QStringLiteral("main"));
        else
            add("profile", vp);
    }
    if (!level.isEmpty())
        add("level", level);
    add("g", QString::number(gop));
    if (v == QStringLiteral("hevc")) {
        QStringList params;
        params << QStringLiteral("keyint=%1").arg(gop);
        const QString scan = str(spec, "scan", "progressive");
        if (scan != "progressive") params << QString("interlace=%1").arg(scan == "tff" ? 1 : 2);
        const QVariantMap hdr = spec.value(QStringLiteral("hdr")).toMap();
        if (isHdr(spec)) {
            const QString md = hdr.value(QStringLiteral("masterDisplay")).toString();
            if (validMasterDisplay(md))
                params << QStringLiteral("master-display=%1").arg(md);
            const int cll = hdr.value(QStringLiteral("maxCll")).toInt();
            const int fall = hdr.value(QStringLiteral("maxFall")).toInt();
            if (cll > 0 || fall > 0)
                params << QStringLiteral("max-cll=%1,%2").arg(cll).arg(fall);
            params << QStringLiteral("hdr10=1");
        }
        add("x265-params", params.join(QLatin1Char(':')));
    }
    return o;
}

static QList<QPair<QString, QString>> audioOptions(const QVariantMap &spec) {
    QList<QPair<QString, QString>> o;
    const QString a = str(spec, "acodec", QStringLiteral("aac"));
    const int kbps = spec.value(QStringLiteral("abitrate"), 192).toInt();
    if (a == QStringLiteral("aac") || a == QStringLiteral("opus") || a == QStringLiteral("mp3")
        || a == QStringLiteral("vorbis"))
        o.append({QStringLiteral("ab"), QString::number(kbps) + QStringLiteral("k")});
    if (a == QStringLiteral("flac"))
        o.append({QStringLiteral("compression_level"), QStringLiteral("8")});
    return o;
}

static QString defaultVaapiDevice();
static QStringList apvEncoderOptions(const QVariantMap &spec, double fps);

QVariantList chaptersFromMarkers(const QVariantList &markers, int first, int last, double fps) {
    QVariantList out;
    if (fps <= 0 || last < first) return out;
    QList<QPair<int, QString>> starts;
    for (const QVariant &v : markers) {
        const QVariantMap marker = v.toMap();
        const int frame = marker.value(QStringLiteral("frame")).toInt();
        if (marker.value(QStringLiteral("type")).toString() != QStringLiteral("chapter") || frame < first || frame > last) continue;
        starts.append({frame, marker.value(QStringLiteral("text")).toString().simplified()});
    }
    std::sort(starts.begin(), starts.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    // Chapters cover the whole movie (an MP4's must begin with its first
    // frame): what comes before the first marker is a chapter of its own.
    if (!starts.isEmpty() && starts.first().first > first) starts.prepend({first, QStringLiteral("Start")});
    const auto ms = [&](int frame) { return qRound64((frame - first) * 1000.0 / fps); };
    for (int i = 0; i < starts.size(); ++i) {
        const int end = i + 1 < starts.size() ? starts.at(i + 1).first : last + 1;
        if (end <= starts.at(i).first) continue;  // two markers on one frame: the first names it
        out.append(QVariantMap{{QStringLiteral("startMs"), ms(starts.at(i).first)}, {QStringLiteral("endMs"), ms(end)},
                               {QStringLiteral("title"), starts.at(i).second.isEmpty()
                                                             ? QStringLiteral("Chapter %1").arg(out.size() + 1) : starts.at(i).second}});
    }
    return out;
}

QList<Chapter> chaptersOf(const QVariantMap &spec) {
    QList<Chapter> out;
    const QString c = spec.value(QStringLiteral("container"), QStringLiteral("mp4")).toString();
    if (!spec.value(QStringLiteral("video"), true).toBool()
        || !(c == QStringLiteral("mp4") || c == QStringLiteral("mov") || c == QStringLiteral("mkv") || c == QStringLiteral("webm")))
        return out;
    for (const QVariant &v : spec.value(QStringLiteral("chapters")).toList()) {
        const QVariantMap m = v.toMap();
        Chapter chapter{m.value(QStringLiteral("startMs")).toLongLong(), m.value(QStringLiteral("endMs")).toLongLong(),
                        m.value(QStringLiteral("title")).toString()};
        if (chapter.startMs >= 0 && chapter.endMs > chapter.startMs) out.append(chapter);
    }
    return out;
}

QString chapterMetadata(const QVariantMap &spec) {
    const QList<Chapter> chapters = chaptersOf(spec);
    if (chapters.isEmpty()) return {};
    QString out = QStringLiteral(";FFMETADATA1\n");
    for (const Chapter &chapter : chapters) {
        QString title = chapter.title;
        // The file's own escapes: = ; # \\ and a line end. (In Qt's replacement text only a
        // backslash before a digit is special.)
        title.replace(QRegularExpression(QStringLiteral("([=;#\\\\\n])")), QStringLiteral("\\\\1"));
        out += QStringLiteral("[CHAPTER]\nTIMEBASE=1/1000\nSTART=%1\nEND=%2\ntitle=%3\n").arg(chapter.startMs).arg(chapter.endMs).arg(title);
    }
    return out;
}

EncoderSettings encoderSettings(const QVariantMap &in, double fps, bool mezzanine) {
    const QVariantMap spec = normalized(in);
    EncoderSettings s;
    if (const QString sequence = sequenceFormat(spec); !sequence.isEmpty() && !mezzanine) {
        s.video = spec.value(QStringLiteral("video")).toBool();
        s.audio = false;
        if (spec.value(QStringLiteral("width")).toInt() > 0 && spec.value(QStringLiteral("height")).toInt() > 0) {
            s.width = spec.value(QStringLiteral("width")).toInt();
            s.height = spec.value(QStringLiteral("height")).toInt();
        }
        s.sequence = sequence;
        s.sequenceStem = spec.value(QStringLiteral("sequenceStem")).toString();
        s.sequenceAlpha = spec.value(QStringLiteral("alpha")).toBool();
        return s;
    }
    s.video = spec.value(QStringLiteral("video")).toBool();
    s.audio = spec.value(QStringLiteral("audio")).toBool();
    if (s.video) {
        const int w = spec.value(QStringLiteral("width")).toInt();
        const int h = spec.value(QStringLiteral("height")).toInt();
        if (w > 0 && h > 0) {
            s.width = w;
            s.height = h;
        }
        const double par = spec.value(QStringLiteral("par"), 1.0).toDouble();
        if (par > 0.0) {
            s.sarNum = int(qRound(par * 10000));
            s.sarDen = 10000;
        }
        const QString scan = str(spec, "scan", QStringLiteral("progressive"));
        if (!(scan == QStringLiteral("progressive") || mezzanine)) {
            s.progressive = false;
            s.topFieldFirst = scan == QStringLiteral("tff");
            s.fieldOrder = scan == QStringLiteral("tff") ? QStringLiteral("tt") : QStringLiteral("bb");
        }
        if (mezzanine) {
            // Lossless RGB keeps chroma, precision and transparency intact
            // when frame-rate conversion or VA-API needs a second stage.
            s.vcodec = QStringLiteral("ffv1");
            s.pixFmt = preciseMezzanine(spec) ? QStringLiteral("rgba64le") : QStringLiteral("bgra");
            s.videoOptions = {{QStringLiteral("level"), QStringLiteral("3")}, {QStringLiteral("g"), QStringLiteral("1")}};
        } else {
            s.pixFmt = pixelFormat(spec);
            s.vcodec = videoEncoder(spec);
            if (str(spec, "vcodec") == QStringLiteral("apv")) {
                // FFmpeg has no APV encoder: OpenAPV's, fed raw pictures.
                if (const QString encoder = apvEncoder(); !encoder.isEmpty())
                    s.pipeEncoder = QStringList{encoder} + apvEncoderOptions(spec, fps);
                s.pipeStream = QStringLiteral("apv");
                const QString container = str(spec, "container", QStringLiteral("mov"));
                if (container == QStringLiteral("mov") || container == QStringLiteral("mp4"))
                    s.movflags = QStringLiteral("+faststart");
            }
            const QString hw = str(spec, "hw");
            if (hw == QStringLiteral("vaapi")) {
                s.hw = hw;
                s.hwDevice = spec.value(QStringLiteral("vaapiDevice"), defaultVaapiDevice()).toString();
                s.hwFormat = spec.value(QStringLiteral("bitDepth")).toInt() >= 10 || isHdr(spec) ? QStringLiteral("p010le") : QStringLiteral("nv12");
            } else if (hw == QStringLiteral("vulkan")) {
                s.hw = hw;
                s.hwFormat = s.pixFmt;
            }
            s.videoOptions = videoOptions(spec, fps);
        }
        const ColourTags ct = colourTags(str(spec, "colorSpace"));
        const QString cs = str(spec, "colorSpace");
        s.colorspace = cs == "rec601" ? 601 : (cs == "rec2020" || cs == "pq" || cs == "hlg") ? 2020 : 709;
        s.primaries = ct.primaries;
        s.trc = ct.trc;
        s.matrix = ct.matrix;
        s.range = str(spec, "range", QStringLiteral("tv")) == QStringLiteral("pc") ? QStringLiteral("pc") : QStringLiteral("tv");
        if (str(spec, "vcodec") == QStringLiteral("hevc") || str(spec, "vcodec") == QStringLiteral("h264"))
            s.movflags = QStringLiteral("+faststart");
        if (!mezzanine && str(spec, "vcodec") == QStringLiteral("prores")) {
            const ColourTags header = proresHeaderTags(spec);
            s.headerPrimaries = header.primaries;
            s.headerTrc = header.trc;
            s.headerMatrix = header.matrix;
            const QString container = str(spec, "container", QStringLiteral("mov"));
            if (container == QStringLiteral("mov") || container == QStringLiteral("mp4"))
                s.movflags = QStringLiteral("+faststart");
        }
        if (!mezzanine && isHdr(spec)) {
            const QVariantMap hdr = spec.value(QStringLiteral("hdr")).toMap();
            if (const QString md = hdr.value(QStringLiteral("masterDisplay")).toString(); validMasterDisplay(md))
                s.masterDisplay = md;
            s.maxCll = qMax(0, hdr.value(QStringLiteral("maxCll")).toInt());
            s.maxFall = qMax(0, hdr.value(QStringLiteral("maxFall")).toInt());
        }
    }
    if (s.audio) {
        if (mezzanine) {
            s.acodec = QStringLiteral("pcm_s24le");
        } else {
            s.acodec = audioEncoder(spec);
            s.audioOptions = audioOptions(spec);
        }
        s.arate = spec.value(QStringLiteral("arate"), 48000).toInt();
        s.channels = spec.value(QStringLiteral("channels"), 2).toInt();
        // The level is measured on the render first (the job adds the gain
        // it found); without a measurement the sound goes out as mixed.
        if (!mezzanine && spec.value(QStringLiteral("loudness")).toBool() && spec.contains(QStringLiteral("loudnessGain"))) {
            const double gain = spec.value(QStringLiteral("loudnessGain")).toDouble();
            if (std::isfinite(gain)) {
                s.loudnessGain = qBound(-60.0, gain, 40.0);
                s.truePeakCeiling = levellingCeiling(spec);
            }
        }
    }
    if (!mezzanine) {
        const QVariantMap meta = spec.value(QStringLiteral("metadata")).toMap();
        for (auto it = meta.cbegin(); it != meta.cend(); ++it)
            if (!it.value().toString().isEmpty()) s.metadata.append({it.key(), it.value().toString()});
        s.chapters = chaptersOf(spec);
    }
    // FFV1 and PCM live in a Matroska intermediate.
    if (mezzanine) {
        s.muxer = QStringLiteral("matroska");
    } else {
        const QString c = str(spec, "container");
        s.muxer = c == QStringLiteral("mkv") ? QStringLiteral("matroska") : c == QStringLiteral("m4a") ? QStringLiteral("ipod")
                : c == QStringLiteral("mov") ? QStringLiteral("mov") : c == QStringLiteral("webm") ? QStringLiteral("webm")
                : c == QStringLiteral("wav") ? QStringLiteral("wav") : c == QStringLiteral("mp3") ? QStringLiteral("mp3")
                : c == QStringLiteral("flac") ? QStringLiteral("flac") : QStringLiteral("mp4");
    }
    return s;
}


// Before the hardware check has recommended a node: the first render node
// whose kernel driver offers VA-API encoding (the NVIDIA driver's VA-API has
// none), found at run time rather than assumed to be renderD128.
static QString defaultVaapiDevice() {
    const auto nodes = QDir(QStringLiteral("/dev/dri")).entryList({QStringLiteral("renderD*")}, QDir::System | QDir::Files, QDir::Name);
    for (const auto &node : nodes) {
        const QString driver = QFileInfo(QFileInfo(QStringLiteral("/sys/class/drm/") + node + QStringLiteral("/device/driver")).symLinkTarget()).fileName();
        if (driver != QStringLiteral("nvidia"))
            return QStringLiteral("/dev/dri/") + node;
    }
    return nodes.isEmpty() ? QStringLiteral("/dev/dri/renderD128") : QStringLiteral("/dev/dri/") + nodes.first();
}


// ── APV ──────────────────────────────────────────────────────────────────
QString apvEncoder() {
    QStringList candidates{qEnvironmentVariable("OMAEDIT_APV_ENCODER")};
    if (QCoreApplication::instance()) {
        const QString app = QCoreApplication::applicationDirPath();
        candidates << app + QStringLiteral("/../lib/omaedit/oapv_app_enc")
                   << app + QStringLiteral("/deps/openapv/bin/oapv_app_enc");
    }
    for (const QString &candidate : std::as_const(candidates)) {
        const QFileInfo file(candidate);
        if (!candidate.isEmpty() && file.isFile() && file.isExecutable())
            return file.absoluteFilePath();
    }
    return QStandardPaths::findExecutable(QStringLiteral("oapv_app_enc"));
}

static QString shellQuoted(const QString &text) {
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

static QString shellCommand(const QStringList &args) {
    QStringList quoted;
    for (const QString &arg : args)
        quoted << shellQuoted(arg);
    return quoted.join(QLatin1Char(' '));
}

// The encoder's colour aspects are ITU-T H.273 numbers.
static int h273(const QString &name) {
    static const QHash<QString, int> numbers{
        {QStringLiteral("bt709"), 1}, {QStringLiteral("bt470bg"), 5}, {QStringLiteral("bt2020"), 9},
        {QStringLiteral("bt2020nc"), 9}, {QStringLiteral("iec61966-2-1"), 13}, {QStringLiteral("bt2020-10"), 14},
        {QStringLiteral("smpte2084"), 16}, {QStringLiteral("arib-std-b67"), 18}};
    return numbers.value(name, 2); // 2 = unspecified
}

// The raw pictures and the encoder's arguments for `pictures` (an ffmpeg
// input) at width×height. -progress goes to fd 3, which the caller connects.
static int apvQp(const QVariantMap &spec, bool twelve) {
    const int q = qBound(0, spec.value(QStringLiteral("quality"), 18).toInt(), 51);
    return qMin(int(std::lround(q * 5 / 3.0)), twelve ? 75 : 63);
}

// The OpenAPV encoder's options for raw pictures at `fps` (everything but the
// input, the size and the output).
static QStringList apvEncoderOptions(const QVariantMap &spec, double fps) {
    const QString profile = apvProfile(spec);
    const bool twelve = profile.endsWith(QStringLiteral("12"));
    const ColourTags ct = colourTags(str(spec, "colorSpace"));
    const bool full = str(spec, "range") == QStringLiteral("pc");
    const QString csp = profile.startsWith(QStringLiteral("4444")) ? QStringLiteral("4")
                      : profile.startsWith(QStringLiteral("444")) ? QStringLiteral("3") : QStringLiteral("2");
    QStringList encode{QStringLiteral("-z"), rationalRate(fps), QStringLiteral("-d"), twelve ? QStringLiteral("12") : QStringLiteral("10"),
                       QStringLiteral("--input-csp"), csp, QStringLiteral("--profile"), profile,
                       QStringLiteral("--color-primaries"), QString::number(h273(ct.primaries)),
                       QStringLiteral("--color-transfer"), QString::number(h273(ct.trc)),
                       QStringLiteral("--color-matrix"), QString::number(h273(ct.matrix)),
                       QStringLiteral("--color-range"), full ? QStringLiteral("1") : QStringLiteral("0")};
    const QString mode = str(spec, "rateMode", QStringLiteral("quality"));
    const int kbps = spec.value(QStringLiteral("bitrate")).toInt();
    if (mode != QStringLiteral("quality") && kbps > 0) {
        encode << QStringLiteral("--bitrate") << QString::number(kbps) + QStringLiteral("k");
    } else {
        // The shared 0–51 quality scale (lower = better) as APV's QP, scaled
        // so the default High (18) lands on QP 30 and the best (0) on QP 0,
        // where a 10-bit ramp keeps every level. 12-bit QP reaches 75.
        encode << QStringLiteral("-q") << QString::number(apvQp(spec, twelve));
    }
    if (!str(spec, "level").isEmpty())
        encode << QStringLiteral("--level") << str(spec, "level");
    return encode;
}

static QString apvPipeline(const QVariantMap &spec, double timelineFps, int width, int height,
                           const QStringList &pictures, const QString &output) {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString encoder = apvEncoder();
    if (ffmpeg.isEmpty() || encoder.isEmpty() || width <= 0 || height <= 0)
        return {};
    const double fps = spec.value(QStringLiteral("fps")).toDouble() > 0.0
                           ? spec.value(QStringLiteral("fps")).toDouble() : timelineFps;
    const QString profile = apvProfile(spec);
    const QString format = apvPixelFormat(profile);
    const ColourTags ct = colourTags(str(spec, "colorSpace"));
    const bool full = str(spec, "range") == QStringLiteral("pc");
    QStringList vf;
    if (timelineFps > 0.0 && qAbs(fps - timelineFps) > 0.01)
        vf << QStringLiteral("fps=%1").arg(rationalRate(fps));
    // The mezzanine is RGB: name the delivery matrix and range, never a default.
    const QString matrix = ct.matrix == QStringLiteral("bt470bg") ? QStringLiteral("bt601")
                         : ct.matrix == QStringLiteral("bt2020nc") ? QStringLiteral("bt2020") : QStringLiteral("bt709");
    vf << QStringLiteral("scale=out_color_matrix=%1:out_range=%2").arg(matrix, full ? QStringLiteral("pc") : QStringLiteral("tv"))
       << QStringLiteral("format=%1").arg(format);
    QStringList decode{ffmpeg, QStringLiteral("-nostdin"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                       QStringLiteral("-progress"), QStringLiteral("pipe:3")};
    decode << pictures << QStringLiteral("-map") << QStringLiteral("0:v:0") << QStringLiteral("-vf") << vf.join(QLatin1Char(','))
           << QStringLiteral("-f") << QStringLiteral("rawvideo") << QStringLiteral("pipe:1");
    QStringList encode{encoder, QStringLiteral("-i"), QStringLiteral("/dev/stdin"),
                       QStringLiteral("-w"), QString::number(width), QStringLiteral("-h"), QString::number(height)};
    encode << apvEncoderOptions(spec, fps);
    encode << QStringLiteral("-o") << output << QStringLiteral("-v") << QStringLiteral("0");
    // pipefail where the shell has it, so a failed decode fails the stage.
    return QStringLiteral("set -e; (set -o pipefail) 2>/dev/null && set -o pipefail; { %1 3>&4 | %2 >/dev/null; } 4>&1")
        .arg(shellCommand(decode), shellCommand(encode));
}

static QStringList shellArgs(const QString &script) {
    QString shell = QStandardPaths::findExecutable(QStringLiteral("bash"));
    if (shell.isEmpty())
        shell = QStandardPaths::findExecutable(QStringLiteral("sh"));
    if (shell.isEmpty() || script.isEmpty())
        return {};
    return {shell, QStringLiteral("-c"), script};
}


static QString containerFormat(const QString &container) {
    return container == QStringLiteral("mkv") ? QStringLiteral("matroska")
         : container == QStringLiteral("mov") ? QStringLiteral("mov") : QStringLiteral("mp4");
}




// The encoder check runs ffmpeg in a process of its own: an encoder that
// does not open here (or a driver that crashes opening it) is found before
// the editor's own sink opens it.
static QStringList ffmpegEncodeArgs(const QVariantMap &in, double timelineFps,
                             const QString &input, const QString &output) {
    const QVariantMap spec = normalized(in);
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        return {};
    QStringList a{ffmpeg, QStringLiteral("-y"), QStringLiteral("-nostdin"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-progress"), QStringLiteral("pipe:1")};
    const bool vaapi = str(spec, "hw") == QStringLiteral("vaapi");
    const bool vulkan = str(spec, "hw") == "vulkan";
    if (vulkan)
        a << "-init_hw_device" << "vulkan=omaedit" << "-filter_hw_device" << "omaedit";
    if (vaapi)
        a << QStringLiteral("-vaapi_device") << spec.value("vaapiDevice", defaultVaapiDevice()).toString();
    a << QStringLiteral("-i") << input;
    const double fps = spec.value(QStringLiteral("fps")).toDouble() > 0.0
                           ? spec.value(QStringLiteral("fps")).toDouble() : timelineFps;
    QStringList vf;
    if (timelineFps > 0.0 && qAbs(fps - timelineFps) > 0.01)
        vf << QStringLiteral("fps=%1").arg(rationalRate(fps));
    if (vaapi) {
        vf << QStringLiteral("format=%1").arg(spec.value(QStringLiteral("bitDepth")).toInt() >= 10
                                                   ? QStringLiteral("p010") : QStringLiteral("nv12"))
           << QStringLiteral("hwupload");
    } else {
        vf << QStringLiteral("format=%1").arg(pixelFormat(spec));
        const QString scan = str(spec, "scan", "progressive");
        if (scan != "progressive") vf << QString("setfield=%1").arg(scan);
        if (vulkan) vf << "hwupload";
    }
    if (!vf.isEmpty())
        a << QStringLiteral("-vf") << vf.join(QLatin1Char(','));
    a << QStringLiteral("-r") << rationalRate(fps);
    a << QStringLiteral("-c:v") << videoEncoder(spec);
    const auto opts = videoOptions(spec, fps);
    for (const auto &kv : opts) {
        if (kv.first == QStringLiteral("b"))
            a << QStringLiteral("-b:v") << kv.second;
        else if (kv.first == QStringLiteral("qscale"))
            a << QStringLiteral("-q:v") << kv.second;
        else if (kv.first == QStringLiteral("profile"))
            a << QStringLiteral("-profile:v") << kv.second;
        else if (kv.first == QStringLiteral("level"))
            a << QStringLiteral("-level:v") << kv.second;
        else
            a << QLatin1Char('-') + kv.first << kv.second;
    }
    const ColourTags ct = colourTags(str(spec, "colorSpace"));
    a << QStringLiteral("-color_primaries") << ct.primaries
      << QStringLiteral("-color_trc") << ct.trc
      << QStringLiteral("-colorspace") << ct.matrix
      << QStringLiteral("-color_range") << (str(spec, "range") == QStringLiteral("pc") ? QStringLiteral("pc") : QStringLiteral("tv"));
    const QString scan = str(spec, "scan", QStringLiteral("progressive"));
    if (scan != QStringLiteral("progressive") && !vaapi)
        a << QStringLiteral("-flags") << QStringLiteral("+ildct+ilme")
          << QStringLiteral("-field_order") << (scan == QStringLiteral("tff") ? QStringLiteral("tt") : QStringLiteral("bb"));
    if (spec.value(QStringLiteral("audio")).toBool()) {
        a << QStringLiteral("-c:a") << audioEncoder(spec)
          << QStringLiteral("-ar") << QString::number(spec.value(QStringLiteral("arate"), 48000).toInt())
          << QStringLiteral("-ac") << QString::number(spec.value(QStringLiteral("channels"), 2).toInt());
        const auto ao = audioOptions(spec);
        for (const auto &kv : ao) {
            if (kv.first == QStringLiteral("ab"))
                a << QStringLiteral("-b:a") << kv.second;
            else
                a << QLatin1Char('-') + kv.first << kv.second;
        }
    } else {
        a << QStringLiteral("-an");
    }
    const QVariantMap meta = spec.value(QStringLiteral("metadata")).toMap();
    for (auto it = meta.cbegin(); it != meta.cend(); ++it)
        if (!it.value().toString().isEmpty())
            a << QStringLiteral("-metadata") << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
    const QString container = str(spec, "container", QStringLiteral("mp4"));
    if (container == QStringLiteral("mp4") || container == QStringLiteral("mov"))
        a << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    a << QStringLiteral("-f") << (container == QStringLiteral("mkv") ? QStringLiteral("matroska")
                                  : container == QStringLiteral("mov") ? QStringLiteral("mov")
                                  : container == QStringLiteral("webm") ? QStringLiteral("webm")
                                  : container == QStringLiteral("wav") ? QStringLiteral("wav")
                                  : container == QStringLiteral("mp3") ? QStringLiteral("mp3")
                                  : container == QStringLiteral("flac") ? QStringLiteral("flac")
                                  : container == QStringLiteral("m4a") ? QStringLiteral("ipod")
                                                                        : QStringLiteral("mp4"));
    a << output;
    return a;
}

static QStringList apvMuxArgs(const QVariantMap &in, double timelineFps, const QString &stream,
                       const QString &sound, const QString &output) {
    const QVariantMap spec = normalized(in);
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        return {};
    const double fps = spec.value(QStringLiteral("fps")).toDouble() > 0.0
                           ? spec.value(QStringLiteral("fps")).toDouble() : timelineFps;
    // A raw APV stream carries no timing: it travels as the exact rational.
    QStringList a{ffmpeg, QStringLiteral("-y"), QStringLiteral("-nostdin"), QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-progress"), QStringLiteral("pipe:1"),
                  QStringLiteral("-framerate"), rationalRate(fps), QStringLiteral("-f"), QStringLiteral("apv"), QStringLiteral("-i"), stream};
    const bool audio = spec.value(QStringLiteral("audio")).toBool() && !sound.isEmpty();
    if (audio)
        a << QStringLiteral("-i") << sound << QStringLiteral("-map") << QStringLiteral("0:v:0")
          << QStringLiteral("-map") << QStringLiteral("1:a:0?");
    a << QStringLiteral("-c:v") << QStringLiteral("copy");
    if (audio) {
        a << QStringLiteral("-c:a") << audioEncoder(spec)
          << QStringLiteral("-ar") << QString::number(spec.value(QStringLiteral("arate"), 48000).toInt())
          << QStringLiteral("-ac") << QString::number(spec.value(QStringLiteral("channels"), 2).toInt());
        for (const auto &kv : audioOptions(spec))
            a << (kv.first == QStringLiteral("ab") ? QStringLiteral("-b:a") : QLatin1Char('-') + kv.first) << kv.second;
    } else {
        a << QStringLiteral("-an");
    }
    const QVariantMap meta = spec.value(QStringLiteral("metadata")).toMap();
    for (auto it = meta.cbegin(); it != meta.cend(); ++it)
        if (!it.value().toString().isEmpty())
            a << QStringLiteral("-metadata") << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
    const QString container = str(spec, "container", QStringLiteral("mov"));
    if (container == QStringLiteral("mp4") || container == QStringLiteral("mov"))
        a << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    a << QStringLiteral("-f") << containerFormat(container) << output;
    return a;
}

QStringList encoderProbeArgs(const QVariantMap &input, int width, int height, double fps, const QString &output) {
    auto spec = normalized(input);
    if (spec.value(QStringLiteral("video")).toBool() && str(spec, "vcodec") == QStringLiteral("apv")) {
        // One black picture through the encoder, then into the container.
        const double rate = spec.value("fps").toDouble() > 0 ? spec.value("fps").toDouble() : fps;
        const int w = spec.value("width").toInt() > 0 ? spec.value("width").toInt() : width;
        const int h = spec.value("height").toInt() > 0 ? spec.value("height").toInt() : height;
        const QString stream = output + QStringLiteral(".apv");
        const QString pipeline = apvPipeline(spec, rate, w, h, {QStringLiteral("-f"), QStringLiteral("lavfi"), QStringLiteral("-i"),
            QStringLiteral("color=black:size=%1x%2:rate=%3,format=rgba64le").arg(qMax(2, w)).arg(qMax(2, h)).arg(rationalRate(rate)),
            QStringLiteral("-frames:v"), QStringLiteral("1")}, stream);
        auto mux = apvMuxArgs(spec, rate, stream, QString(), output);
        if (pipeline.isEmpty() || mux.isEmpty())
            return {};
        mux.removeAll(QStringLiteral("-progress"));
        mux.removeAll(QStringLiteral("pipe:1"));
        return shellArgs(QStringLiteral("{ %1; } >/dev/null && %2").arg(pipeline, shellCommand(mux)));
    }
    const double rate = spec.value("fps").toDouble() > 0 ? spec.value("fps").toDouble() : fps;
    const int w = spec.value("width").toInt() > 0 ? spec.value("width").toInt() : width;
    const int h = spec.value("height").toInt() > 0 ? spec.value("height").toInt() : height;
    auto args = ffmpegEncodeArgs(spec, rate, "probe-input", output);
    if (args.isEmpty()) return {};
    const int source = args.indexOf("-i");
    args.remove(source, 2);
    QStringList inputs{"-f", "lavfi", "-i", QString("color=black:size=%1x%2:rate=%3").arg(qMax(1,w)).arg(qMax(1,h)).arg(rationalRate(rate))};
    if (spec.value("audio").toBool()) {
        const int channels=spec.value("channels").toInt();
        inputs << "-f" << "lavfi" << "-i" << QString("anullsrc=r=%1:cl=%2")
            .arg(spec.value("arate").toInt()).arg(channels==1?"mono":channels==6?"5.1":"stereo");
    }
    for (int i=0;i<inputs.size();++i) args.insert(source+i, inputs[i]);
    args.removeLast();
    if (!spec.value("video").toBool()) args << "-vn";
    args << "-t" << QString::number(1.0/qMax(1.0,rate),'f',6) << "-frames:v" << "1" << output;
    return args;
}

QString startTimecode(qint64 frame, int fpsNum, int fpsDen, int toNum, int toDen) {
    if (fpsNum <= 0 || fpsDen <= 0) return {};
    if (toNum <= 0 || toDen <= 0) { toNum = fpsNum; toDen = fpsDen; }
    const qint64 counted = qMax(1, qRound(double(fpsNum) / fpsDen)), told = qMax(1, qRound(double(toNum) / toDen));
    const qint64 day = 86400 * counted, f = ((frame % day) + day) % day, seconds = f / counted;
    return QStringLiteral("%1:%2:%3:%4").arg(seconds / 3600, 2, 10, QLatin1Char('0')).arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0')).arg(qMin(told - 1, (f % counted) * told / counted), 2, 10, QLatin1Char('0'));
}

qint64 startTimeReference(qint64 frame, int fpsNum, int fpsDen, int sampleRate) {
    if (fpsNum <= 0 || fpsDen <= 0 || sampleRate <= 0) return -1;
    const qint64 day = 86400ll * qMax(1, qRound(double(fpsNum) / fpsDen)), f = ((frame % day) + day) % day;
    // The frame's own time: at 23.976 and 29.97 a counted second is a little longer than a clock's.
    return qRound64(double(f) * sampleRate * fpsDen / fpsNum);
}

void stampStart(EncoderSettings &s, qint64 frame, int fpsNum, int fpsDen, int toNum, int toDen) {
    s.timecode.clear();
    s.timeReference = -1;
    if (!s.sequence.isEmpty()) return;
    if (s.video && (s.muxer == QLatin1String("mov") || s.muxer == QLatin1String("mp4") || s.muxer == QLatin1String("matroska") || s.muxer == QLatin1String("webm")))
        s.timecode = startTimecode(frame, fpsNum, fpsDen, toNum, toDen);
    else if (s.audio && !s.video && s.muxer == QLatin1String("wav"))
        s.timeReference = startTimeReference(frame, fpsNum, fpsDen, s.arate);
}

QStringList startArgs(const EncoderSettings &s) {
    QStringList a;
    if (!s.timecode.isEmpty()) {
        a << QStringLiteral("-metadata") << QStringLiteral("timecode=") + s.timecode;
        // An MP4 holds a timecode track only when asked; a QuickTime file always.
        if (s.muxer == QLatin1String("mp4")) a << QStringLiteral("-write_tmcd") << QStringLiteral("1");
    }
    if (s.timeReference >= 0)
        a << QStringLiteral("-metadata") << QStringLiteral("time_reference=%1").arg(s.timeReference) << QStringLiteral("-write_bext") << QStringLiteral("1");
    return a;
}

QStringList ffmpegMuxSubtitleArgs(const QVariantMap &in, const QString &input,
                                  const QString &subtitles, const QString &output) {
    const QVariantMap spec = normalized(in);
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        return {};
    const QString container = str(spec, "container", QStringLiteral("mp4"));
    const QString scodec = container == QStringLiteral("mkv") ? QStringLiteral("srt")
                         : container == QStringLiteral("webm") ? QStringLiteral("webvtt")
                                                                : QStringLiteral("mov_text");
    QStringList a{ffmpeg, QStringLiteral("-y"), QStringLiteral("-nostdin"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"),
                  QStringLiteral("-i"), input, QStringLiteral("-i"), subtitles,
                  // The pictures and the sound: the file's timecode track is not a
                  // stream to copy, it is made again from the timecode the pictures carry.
                  QStringLiteral("-map"), QStringLiteral("0:v?"), QStringLiteral("-map"), QStringLiteral("0:a?"), QStringLiteral("-map"), QStringLiteral("1:0"),
                  QStringLiteral("-c"), QStringLiteral("copy"), QStringLiteral("-c:s"), scodec,
                  QStringLiteral("-metadata:s:s:0"), QStringLiteral("language=eng")};
    if (container == QStringLiteral("mp4") || container == QStringLiteral("mov"))
        a << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    if (container == QStringLiteral("mp4"))
        a << QStringLiteral("-write_tmcd") << QStringLiteral("1");
    a << QStringLiteral("-f") << (container == QStringLiteral("mkv") ? QStringLiteral("matroska")
                                  : container == QStringLiteral("mov") ? QStringLiteral("mov")
                                  : container == QStringLiteral("webm") ? QStringLiteral("webm")
                                                                        : QStringLiteral("mp4"));
    a << output;
    return a;
}

QStringList ffmpegConcatArgs(const QString &listFile, const QString &output, const QString &audio,
                             const QVariantMap &metadata, const QString &chapters, const QStringList &start) {
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty())
        return {};
    // The join writes a `.part` working file: the container is the name under it.
    QString named = output;
    if (named.endsWith(QStringLiteral(".part"), Qt::CaseInsensitive))
        named.chop(5);
    const QString ext = QFileInfo(named).suffix().toLower();
    // -copyts: the list's durations already place every segment; letting
    // ffmpeg rebase them on the first file's audio priming delays the picture.
    QStringList a{ffmpeg, QStringLiteral("-y"), QStringLiteral("-nostdin"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-copyts"),
                  QStringLiteral("-f"), QStringLiteral("concat"), QStringLiteral("-safe"), QStringLiteral("0"),
                  QStringLiteral("-i"), listFile};
    if (!audio.isEmpty())
        a << QStringLiteral("-i") << audio;
    // A metadata file holds no streams: it gives the chapters and nothing else.
    if (!chapters.isEmpty())
        a << QStringLiteral("-f") << QStringLiteral("ffmetadata") << QStringLiteral("-i") << chapters
          << QStringLiteral("-map_chapters") << QString::number(audio.isEmpty() ? 1 : 2);
    if (!audio.isEmpty())
        a << QStringLiteral("-map") << QStringLiteral("0:v") << QStringLiteral("-map") << QStringLiteral("1:a");
    a << QStringLiteral("-c") << QStringLiteral("copy");
    for (auto it = metadata.cbegin(); it != metadata.cend(); ++it)
        if (!it.value().toString().isEmpty())
            a << QStringLiteral("-metadata") << QStringLiteral("%1=%2").arg(it.key(), it.value().toString());
    a << start;
    if (ext == QStringLiteral("mp4") || ext == QStringLiteral("mov") || ext == QStringLiteral("m4a"))
        a << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    a << QStringLiteral("-f") << (ext == QStringLiteral("mkv") ? QStringLiteral("matroska")
                                  : ext == QStringLiteral("mov") ? QStringLiteral("mov")
                                  : ext == QStringLiteral("webm") ? QStringLiteral("webm")
                                  : ext == QStringLiteral("wav") ? QStringLiteral("wav")
                                  : ext == QStringLiteral("mp3") ? QStringLiteral("mp3")
                                  : ext == QStringLiteral("flac") ? QStringLiteral("flac")
                                  : ext == QStringLiteral("m4a") ? QStringLiteral("ipod")
                                                                 : QStringLiteral("mp4"));
    a << output;
    return a;
}

// ── Estimates ────────────────────────────────────────────────────────────
static double videoKbps(const QVariantMap &spec, int w, int h, double fps) {
    const QString v = str(spec, "vcodec", QStringLiteral("h264"));
    const QString mode = str(spec, "rateMode", QStringLiteral("quality"));
    const int kbps = spec.value(QStringLiteral("bitrate")).toInt();
    const double pixels = double(qMax(1, w)) * qMax(1, h) * qMax(1.0, fps);
    const QString vp = str(spec, "vprofile");
    if (v == QStringLiteral("prores")) {
        // Bits per pixel per frame, the codec's published targets.
        const double bpp = vp == QStringLiteral("proxy") ? 0.87 : vp == QStringLiteral("lt") ? 1.9
                         : vp == QStringLiteral("standard") ? 2.5 : vp == QStringLiteral("4444") ? 5.3
                         : vp == QStringLiteral("4444xq") ? 8.0 : 3.55;
        return pixels * bpp / 1000.0;
    }
    if (v == QStringLiteral("dnxhr")) {
        const double bpp = vp == QStringLiteral("lb") ? 0.7 : vp == QStringLiteral("sq") ? 2.2
                         : vp == QStringLiteral("hqx") ? 3.6 : vp == QStringLiteral("444") ? 7.0 : 3.5;
        return pixels * bpp / 1000.0;
    }
    if (v == QStringLiteral("ffv1"))
        return pixels * 6.0 / 1000.0;
    if (v == QStringLiteral("apv") && (mode == QStringLiteral("quality") || kbps <= 0)) {
        // Measured at QP 30 on detailed 10-bit footage; chroma and alpha add
        // samples, and the size doubles every eight QP steps down.
        const QString profile = apvProfile(spec);
        const double samples = profile.startsWith(QStringLiteral("4444")) ? 2.0
                             : profile.startsWith(QStringLiteral("444")) ? 1.5 : 1.0;
        const bool twelve = profile.endsWith(QStringLiteral("12"));
        return pixels * 4.8 * samples * (twelve ? 1.2 : 1.0)
               * qPow(2.0, (30.0 - apvQp(spec, twelve)) / 8.0) / 1000.0;
    }
    if (mode != QStringLiteral("quality") && kbps > 0)
        return kbps;
    if (v == QStringLiteral("mpeg2"))
        return kbps > 0 ? kbps : 15000;
    const int q = qBound(0, spec.value(QStringLiteral("quality"), 18).toInt(), 51);
    const double factor = v == QStringLiteral("hevc") ? 0.6 : v == QStringLiteral("av1") ? 0.5
                        : v == QStringLiteral("vp9") ? 0.7 : 1.0;
    // 0.07 bits/pixel/frame at crf 23 for x264, doubling every 6 crf steps.
    return factor * 0.07 * pixels / 1000.0 * qPow(2.0, (23.0 - q) / 6.0);
}

static double audioKbps(const QVariantMap &spec) {
    const QString a = str(spec, "acodec", QStringLiteral("aac"));
    const int rate = spec.value(QStringLiteral("arate"), 48000).toInt();
    const int ch = spec.value(QStringLiteral("channels"), 2).toInt();
    if (a == QStringLiteral("pcm16")) return rate * ch * 16 / 1000.0;
    if (a == QStringLiteral("pcm24")) return rate * ch * 24 / 1000.0;
    if (a == QStringLiteral("flac")) return rate * ch * 16 * 0.6 / 1000.0;
    return spec.value(QStringLiteral("abitrate"), 192).toInt();
}

qint64 estimateBytes(const QVariantMap &in, double seconds, int width, int height, double fps) {
    const QVariantMap spec = normalized(in);
    const int w = spec.value(QStringLiteral("width")).toInt() > 0 ? spec.value(QStringLiteral("width")).toInt() : width;
    const int h = spec.value(QStringLiteral("height")).toInt() > 0 ? spec.value(QStringLiteral("height")).toInt() : height;
    const double f = spec.value(QStringLiteral("fps")).toDouble() > 0.0 ? spec.value(QStringLiteral("fps")).toDouble() : fps;
    // An image sequence: a file a timeline frame, by what a pixel of that kind tends to take once packed.
    if (const QString kind = sequenceFormat(spec); !kind.isEmpty()) {
        const double perPixel = kind == QLatin1String("png8") ? 1.5 : kind == QLatin1String("jpeg") ? 0.4 : kind == QLatin1String("exr") ? 4.0 : 3.6;
        return qint64(perPixel * w * h * qMax(0.0, seconds) * fps);
    }
    double kbps = 0.0;
    if (spec.value(QStringLiteral("video")).toBool())
        kbps += videoKbps(spec, w, h, f);
    if (spec.value(QStringLiteral("audio")).toBool())
        kbps += audioKbps(spec);
    return qint64(kbps * 1000.0 / 8.0 * qMax(0.0, seconds));
}

// ── Validation ───────────────────────────────────────────────────────────
static QVariantMap row(const QString &level, const QString &text) {
    QVariantMap r;
    r.insert(QStringLiteral("level"), level);
    r.insert(QStringLiteral("text"), text);
    return r;
}

static QString codecTitle(const QString &v) {
    if (v == QStringLiteral("h264")) return QStringLiteral("H.264");
    if (v == QStringLiteral("hevc")) return QStringLiteral("H.265");
    if (v == QStringLiteral("prores")) return QStringLiteral("ProRes");
    if (v == QStringLiteral("dnxhr")) return QStringLiteral("DNxHR");
    if (v == QStringLiteral("vp9")) return QStringLiteral("VP9");
    if (v == QStringLiteral("av1")) return QStringLiteral("AV1");
    if (v == QStringLiteral("mpeg2")) return QStringLiteral("MPEG-2");
    if (v == QStringLiteral("ffv1")) return QStringLiteral("FFV1");
    if (v == QStringLiteral("apv")) return QStringLiteral("APV");
    return v;
}

static QString audioTitle(const QString &a) {
    if (a == QStringLiteral("aac")) return QStringLiteral("AAC");
    if (a == QStringLiteral("opus")) return QStringLiteral("Opus");
    if (a == QStringLiteral("mp3")) return QStringLiteral("MP3");
    if (a == QStringLiteral("pcm16")) return QStringLiteral("PCM 16-bit");
    if (a == QStringLiteral("pcm24")) return QStringLiteral("PCM 24-bit");
    if (a == QStringLiteral("flac")) return QStringLiteral("FLAC");
    if (a == QStringLiteral("vorbis")) return QStringLiteral("Vorbis");
    return a;
}

// Sizes as the estimate under the summary writes them (11.73 MB, 691.20 kB).
static QString sizeText(qint64 bytes) {
    return QLocale().formattedDataSize(qMax<qint64>(0, bytes), 2, QLocale::DataSizeSIFormat);
}

// Where a platform preset is going, and what that destination takes
// (2026 published limits; 0: no limit to hold it to).
struct PlatformRules {
    const char *id;
    const char *name;
    double minSeconds, maxSeconds;
    qint64 maxBytes;
    double minFps, maxFps;
    int maxLong, maxShort;  // picture sides, either way round
    bool hdr;
    const char *beyond;  // why a longer or larger file may still be fine
};
static const PlatformRules *platformRules(const QString &id) {
    static const PlatformRules rules[] = {
        {"youtube", QT_TRANSLATE_NOOP("ExportSpec", "YouTube"), 0, 12 * 3600, 256LL << 30, 0, 0, 0, 0, true,
         QT_TRANSLATE_NOOP("ExportSpec", "Over 15 minutes needs a verified YouTube account.")},
        {"youtube-shorts", QT_TRANSLATE_NOOP("ExportSpec", "YouTube Shorts"), 0, 180, 0, 0, 0, 0, 0, true, ""},
        {"vimeo", QT_TRANSLATE_NOOP("ExportSpec", "Vimeo"), 0, 0, 0, 0, 0, 0, 0, true, ""},
        {"instagram-reels", QT_TRANSLATE_NOOP("ExportSpec", "Instagram Reels"), 3, 15 * 60, 0, 23, 60, 0, 0, false,
         QT_TRANSLATE_NOOP("ExportSpec", "Some accounts can post up to 20 minutes from the app.")},
        {"instagram-feed", QT_TRANSLATE_NOOP("ExportSpec", "Instagram"), 3, 15 * 60, 0, 23, 60, 0, 0, false,
         QT_TRANSLATE_NOOP("ExportSpec", "Some accounts can post up to 20 minutes from the app.")},
        {"instagram-stories", QT_TRANSLATE_NOOP("ExportSpec", "Instagram Stories"), 3, 60, 0, 23, 60, 0, 0, false,
         QT_TRANSLATE_NOOP("ExportSpec", "Longer stories are cut into 60-second parts.")},
        {"tiktok", QT_TRANSLATE_NOOP("ExportSpec", "TikTok"), 3, 10 * 60, 4LL << 30, 23, 60, 4096, 4096, false,
         QT_TRANSLATE_NOOP("ExportSpec", "Some accounts can upload up to 60 minutes from a computer.")},
        {"facebook-reels", QT_TRANSLATE_NOOP("ExportSpec", "Facebook Reels"), 3, 0, 0, 23, 60, 0, 0, false, ""},
        {"facebook-feed", QT_TRANSLATE_NOOP("ExportSpec", "Facebook"), 1, 240 * 60, 4LL << 30, 23, 60, 0, 0, false, ""},
        {"x", QT_TRANSLATE_NOOP("ExportSpec", "X"), 0, 140, 512LL << 20, 0, 60, 1280, 1024, false,
         QT_TRANSLATE_NOOP("ExportSpec", "X Premium accounts can post longer and larger videos.")},
        {"linkedin", QT_TRANSLATE_NOOP("ExportSpec", "LinkedIn"), 3, 15 * 60, 5LL << 30, 10, 60, 4096, 2304, false, ""},
    };
    for (const PlatformRules &r : rules)
        if (id == QLatin1String(r.id)) return &r;
    return nullptr;
}
static QString shapeName(int w, int h) {
    if (w <= 0 || h <= 0) return {};
    const int g = std::gcd(w, h);
    const int a = w / g, b = h / g;
    if (a <= 21 && b <= 21) return QStringLiteral("%1:%2").arg(a).arg(b);
    return w >= h ? QStringLiteral("%1:1").arg(double(w) / h, 0, 'f', 2) : QStringLiteral("1:%1").arg(double(h) / w, 0, 'f', 2);
}
static QString durationText(double seconds) {
    // Under a minute, a length keeps its hundredths (2.40 s, as everywhere
    // else); a whole limit stays whole (3 s).
    if (seconds < 59.995) {
        const double hundredths = qRound(qMax(0.0, seconds) * 100) / 100.0;
        return hundredths == std::floor(hundredths)
                   ? QCoreApplication::translate("ExportSpec", "%1 s").arg(int(hundredths))
                   : QCoreApplication::translate("ExportSpec", "%1 s").arg(QString::number(hundredths, 'f', 2));
    }
    const int s = qMax(0, qRound(seconds));
    if (s < 3600) return QCoreApplication::translate("ExportSpec", "%1 min %2 s").arg(s / 60).arg(s % 60);
    return QCoreApplication::translate("ExportSpec", "%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60);
}

QVariantList validate(const QVariantMap &in, const QVariantMap &ctx) {
    const QVariantMap spec = normalized(in);
    QVariantList out;
    const QString container = str(spec, "container", QStringLiteral("mp4"));
    const QString v = str(spec, "vcodec", QStringLiteral("h264"));
    const QString a = str(spec, "acodec", QStringLiteral("aac"));
    const bool video = spec.value(QStringLiteral("video")).toBool();
    const bool audio = spec.value(QStringLiteral("audio")).toBool();
    const QVariantMap encoders = ctx.value(QStringLiteral("encoders")).toMap();
    const QString hw = str(spec, "hw", QStringLiteral("software"));

    // Output path.
    const QString path = ctx.value(QStringLiteral("path")).toString();
    if (ctx.contains("outputChecks"))
        out = ctx.value("outputChecks").toList();
    else if (path.isEmpty())
        out.append(row(QStringLiteral("error"), QCoreApplication::translate("ExportSpec", "No file name")));
    else if (!ctx.value(QStringLiteral("dirExists")).toBool())
        out.append(row(QStringLiteral("error"), QCoreApplication::translate("ExportSpec", "Save location does not exist")));
    else if (!ctx.value(QStringLiteral("dirWritable")).toBool())
        out.append(row(QStringLiteral("error"), QCoreApplication::translate("ExportSpec", "Save location is not writable")));
    else if (ctx.value(QStringLiteral("fileExists")).toBool())
        out.append(row(QStringLiteral("warning"), QCoreApplication::translate("ExportSpec", "%1 exists and will be replaced").arg(QFileInfo(path).fileName())));
    else
        out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "Save location is writable")));

    // Something to render.
    if (!ctx.value(QStringLiteral("hasVideo")).toBool())
        out.append(row(QStringLiteral("error"), QCoreApplication::translate("ExportSpec", "The timeline is empty")));
    if (!video && !audio)
        out.append(row(QStringLiteral("error"), QCoreApplication::translate("ExportSpec", "Nothing selected to render: video and audio are both off")));
    if (audio && !ctx.value(QStringLiteral("hasAudio"), true).toBool() && !video)
        out.append(row(QStringLiteral("warning"), QCoreApplication::translate("ExportSpec", "No clip on the timeline has audio")));

    // Format / codec combinations.
    bool formatOk = true;
    auto bad = [&](const QString &text) { out.append(row(QStringLiteral("error"), text)); formatOk = false; };
    auto warn = [&](const QString &text) { out.append(row(QStringLiteral("warning"), text)); };
    if (video) {
        if (container == QStringLiteral("mp4")) {
            if (v == QStringLiteral("prores") || v == QStringLiteral("dnxhr") || v == QStringLiteral("ffv1"))
                bad(QCoreApplication::translate("ExportSpec", "%1 cannot go in an MP4 — choose MOV or MKV").arg(codecTitle(v)));
            else if (v == QStringLiteral("vp9"))
                warn(QCoreApplication::translate("ExportSpec", "VP9 in MP4 plays in few players — WebM or MKV is the usual home"));
        } else if (container == QStringLiteral("webm")) {
            if (v != QStringLiteral("vp9") && v != QStringLiteral("av1"))
                bad(QCoreApplication::translate("ExportSpec", "WebM holds VP9 or AV1 only"));
        }
        if (container == "mov" && v == "ffv1") bad(QCoreApplication::translate("ExportSpec", "FFV1 requires MKV."));
        if (container == "mov" && (v == "vp9" || v == "av1")) bad(QCoreApplication::translate("ExportSpec", "VP9 and AV1 require MP4, MKV or WebM."));
        const QString profile = str(spec,"vprofile");
        if (v == "h264" && spec.value("bitDepth").toInt() >= 10 && !profile.isEmpty() && profile != "high10")
            bad("10-bit H.264 requires the High 10 profile or Auto.");
        if (v == "hevc" && (spec.value("bitDepth").toInt() >= 10 || isHdr(spec)) && profile == "main")
            bad("10-bit H.265 requires the Main 10 profile or Auto.");
        if (v == "vp9" && spec.value("alpha").toBool() && spec.value("bitDepth").toInt() >= 10)
            bad(QCoreApplication::translate("ExportSpec", "VP9 transparency requires 8-bit output."));
        if (str(spec,"scan") != "progressive" && QStringList{"vp9","av1","dnxhr","apv"}.contains(v))
            bad(QCoreApplication::translate("ExportSpec", "This codec requires progressive output."));
        if (str(spec,"rateMode") == "vbr" && spec.value("maxrate").toInt() > 0 && spec.value("maxrate").toInt() < spec.value("bitrate").toInt())
            bad(QCoreApplication::translate("ExportSpec", "Maximum bitrate must be at least the target bitrate."));
        if (v == "av1" && hw == "software" && str(spec,"rateMode") == "vbr" && spec.value("maxrate").toInt() > 0)
            bad(QCoreApplication::translate("ExportSpec", "Software AV1 variable bitrate does not support a separate maximum; set maximum bitrate to 0."));
        if (const QString md = spec.value("hdr").toMap().value("masterDisplay").toString(); !md.isEmpty() && !validMasterDisplay(md))
            bad(QCoreApplication::translate("ExportSpec", "Mastering display must be written G(x,y)B(x,y)R(x,y)WP(x,y)L(max,min), with whole numbers."));
        if (isHdr(spec) && (v != "hevc" || hw != "software") && !spec.value("hdr").toMap().isEmpty())
            warn(QCoreApplication::translate("ExportSpec", "HDR mastering metadata is written by software H.265 only; other encoders retain the output colour tags."));
        if (spec.value(QStringLiteral("alpha")).toBool() && v == QStringLiteral("vp9") && container == QStringLiteral("mp4"))
            bad(QCoreApplication::translate("ExportSpec", "VP9 keeps transparency only in WebM or MKV; MP4 would drop it."));
        if (spec.value(QStringLiteral("alpha")).toBool()) {
            const QString vp = str(spec, "vprofile");
            const bool can = (v == QStringLiteral("prores") && (vp == QStringLiteral("4444") || vp == QStringLiteral("4444xq")))
                             || v == QStringLiteral("ffv1") || v == QStringLiteral("vp9")
                             || (v == QStringLiteral("apv") && apvProfile(spec).startsWith(QStringLiteral("4444")));
            if (!can)
                bad(QCoreApplication::translate("ExportSpec", "Alpha needs ProRes 4444, APV 4444, FFV1 or VP9"));
        }
        if (ctx.contains("managedOutput") && !ctx.value("managedOutput").toBool()
            && QStringList{"pq", "hlg", "srgb", "rec2020"}.contains(str(spec, "colorSpace")))
            bad(QCoreApplication::translate("ExportSpec", "Enable OpenColorIO colour conversion in the Colour workspace before exporting this colour space, or choose Rec.709 / Rec.601."));
        if (isHdr(spec) && v != QStringLiteral("hevc") && v != QStringLiteral("av1") && v != QStringLiteral("apv"))
            bad(QCoreApplication::translate("ExportSpec", "HDR (PQ / HLG) needs H.265, AV1 or APV at 10-bit"));
        if (v == QStringLiteral("apv") && !str(spec, "vprofile").isEmpty() && apvProfile(spec) != str(spec, "vprofile"))
            bad(QCoreApplication::translate("ExportSpec", "Choose an APV profile: 4:2:2, 4:4:4 or 4:4:4:4 at 10 or 12 bits."));
        if (spec.value(QStringLiteral("bitDepth")).toInt() >= 10 && v == QStringLiteral("h264") && hw != QStringLiteral("software"))
            bad(QStringLiteral("10-bit H.264 needs the software encoder"));
        if (spec.value(QStringLiteral("bitDepth")).toInt() >= 10 && v == QStringLiteral("mpeg2"))
            bad(QCoreApplication::translate("ExportSpec", "MPEG-2 is 8-bit only"));
        if (str(spec, "scan") != QStringLiteral("progressive") && hw == QStringLiteral("vaapi"))
            warn(QCoreApplication::translate("ExportSpec", "VAAPI encodes progressive frames — the scan type is ignored"));
        if (hw == "vulkan" && (v != "prores" || spec.value("alpha").toBool()
            || QStringList{"4444", "4444xq"}.contains(str(spec,"vprofile")) || str(spec,"scan") != "progressive"))
            bad(QCoreApplication::translate("ExportSpec", "Vulkan encoding supports progressive ProRes Proxy, LT, 422 and HQ. Choose software for other formats."));
        if (hw != QStringLiteral("software") && ((v == QStringLiteral("prores") && hw != "vulkan") || v == QStringLiteral("dnxhr")
                                                  || v == QStringLiteral("vp9") || v == QStringLiteral("mpeg2") || v == QStringLiteral("ffv1")
                                                  || v == QStringLiteral("apv")))
            warn(QCoreApplication::translate("ExportSpec", "%1 has no hardware encoder — rendering in software").arg(codecTitle(v)));
        // Encoders present on this machine.
        const QString enc = videoEncoder(spec);
        if (v == QStringLiteral("apv") && encoders.contains(enc) && !encoders.value(enc).toBool())
            bad(QCoreApplication::translate("ExportSpec", "APV export needs the OpenAPV encoder (oapv_app_enc), which is not installed."));
        else if (!encoders.isEmpty()) {
            const QVariant have = encoders.value(enc);
            if (have.isValid() && !have.toBool()) {
                if (hw != QStringLiteral("software")) {
                    auto software=spec;software["hw"]="software";
                    if(spec.value("softwareFallback").toBool() && encoders.value(videoEncoder(software),true).toBool())
                        warn(QCoreApplication::translate("ExportSpec", "%1 is unavailable; this job will use software encoding.").arg(hw));
                    else bad(QCoreApplication::translate("ExportSpec", "%1 is unavailable and software fallback is disabled.").arg(hw));
                }
                else
                    bad(QCoreApplication::translate("ExportSpec", "The %1 encoder (%2) is not in this ffmpeg").arg(codecTitle(v), enc));
            }
        }
        const int w = spec.value(QStringLiteral("width")).toInt();
        const int h = spec.value(QStringLiteral("height")).toInt();
        if ((w > 0) != (h > 0) || w < 0 || h < 0)
            bad(QCoreApplication::translate("ExportSpec", "Resolution needs both a width and a height"));
        else if (w > 0 && (w % 2 || h % 2) && v != QStringLiteral("ffv1"))
            bad(QCoreApplication::translate("ExportSpec", "Width and height must be even for %1").arg(codecTitle(v)));
        const int tw = ctx.value(QStringLiteral("width")).toInt(), th = ctx.value(QStringLiteral("height")).toInt();
        if (w > 0 && tw > 0 && w > tw)
            warn(QCoreApplication::translate("ExportSpec", "Upscaling from %1×%2 — the timeline is smaller than the output").arg(tw).arg(th));
        const double fps = spec.value(QStringLiteral("fps")).toDouble();
        const double tf = ctx.value(QStringLiteral("fps")).toDouble();
        if (fps > 0.0 && tf > 0.0 && qAbs(fps - tf) > 0.01)
            warn(QCoreApplication::translate("ExportSpec", "Frame rate %1 differs from the timeline's %2 — frames are repeated or dropped")
                     .arg(QString::number(fps, 'g', 6), QString::number(tf, 'g', 6)));
        if (str(spec, "rateMode") != QStringLiteral("quality") && spec.value(QStringLiteral("bitrate")).toInt() <= 0
            && v != QStringLiteral("prores") && v != QStringLiteral("dnxhr") && v != QStringLiteral("ffv1"))
            bad(QCoreApplication::translate("ExportSpec", "Bitrate mode needs a bitrate"));
        if (formatOk)
            out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "Video settings are valid")));
    }
    if (audio) {
        bool audioOk = true;
        auto abad = [&](const QString &t) { bad(t); audioOk = false; };
        if (container == "mov" && (a == "opus" || a == "flac" || a == "vorbis"))
            abad(QCoreApplication::translate("ExportSpec", "This audio codec requires MP4, MKV or WebM; choose AAC or PCM for MOV."));
        const int sampleRate = spec.value("arate").toInt();
        if (a == "opus" && sampleRate != 48000) abad(QCoreApplication::translate("ExportSpec", "Choose 48 kHz for Opus audio."));
        if (a == "mp3" && sampleRate > 48000) abad(QCoreApplication::translate("ExportSpec", "MP3 supports sample rates up to 48 kHz."));
        if (a == "vorbis" && sampleRate > 48000) abad(QCoreApplication::translate("ExportSpec", "Choose 44.1 or 48 kHz for bitrate-controlled Vorbis audio."));
        if (container == QStringLiteral("webm") && a != QStringLiteral("opus") && a != QStringLiteral("vorbis"))
            abad(QCoreApplication::translate("ExportSpec", "WebM holds Opus or Vorbis audio only"));
        if (container == QStringLiteral("mp4") && a.startsWith(QStringLiteral("pcm")))
            warn(QCoreApplication::translate("ExportSpec", "PCM in MP4 plays in few players — AAC, or a MOV/MKV file"));
        if (container == QStringLiteral("mp4") && (a == QStringLiteral("flac") || a == QStringLiteral("vorbis")))
            warn(QCoreApplication::translate("ExportSpec", "%1 in MP4 plays in few players").arg(audioTitle(a)));
        if (a == QStringLiteral("mp3") && spec.value(QStringLiteral("channels")).toInt() > 2)
            abad(QCoreApplication::translate("ExportSpec", "MP3 is mono or stereo only"));
        if (!encoders.isEmpty()) {
            const QVariant have = encoders.value(audioEncoder(spec));
            if (have.isValid() && !have.toBool())
                abad(QCoreApplication::translate("ExportSpec", "The %1 encoder is not in this ffmpeg").arg(audioTitle(a)));
        }
        if (audioOk)
            out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "Audio settings are valid")));
    }

    const double split=spec.value("splitSeconds").toDouble();
    if(!std::isfinite(split)||split<0||split>86400)bad(QCoreApplication::translate("ExportSpec", "Split duration must be between 0 and 86400 seconds."));
    if(audio && spec.value("audioSelection")=="custom" && spec.value("audioTracks").toList().isEmpty())bad(QCoreApplication::translate("ExportSpec", "Select at least one audio track."));
    const auto mapping=spec.value("audioTrackMap").toMap();const int channels=spec.value("channels").toInt();
    if (audio) for(auto it=mapping.cbegin();it!=mapping.cend();++it){const QString mode=it.value().toString();
        if(!QStringList{"stereo","left","right","center","lfe","surroundLeft","surroundRight"}.contains(mode))bad(QCoreApplication::translate("ExportSpec", "Unknown audio channel mapping."));
        else if((mode=="right"&&channels<2)||((mode=="center"||mode=="lfe"||mode.startsWith("surround"))&&channels<6))bad(QCoreApplication::translate("ExportSpec", "The audio mapping needs more output channels."));}
    if(audio && ctx.contains("tracks"))for(const auto &v:spec.value("audioTracks").toList()) {
        const int t=v.toInt();if(t<0||t>=ctx.value("tracks").toList().size())bad(QCoreApplication::translate("ExportSpec", "A selected audio track no longer exists."));
    }
    // Subtitles.
    const QString subs = str(spec, "subtitles", QStringLiteral("none"));
    if (subs != QStringLiteral("none")) {
        if (!ctx.value(QStringLiteral("hasCaptions")).toBool())
            warn(QCoreApplication::translate("ExportSpec", "No captions on the timeline — the subtitle option does nothing"));
        else if (subs == QStringLiteral("embed") && !video)
            warn(QCoreApplication::translate("ExportSpec", "Subtitles cannot be embedded in an audio file — writing a sidecar instead"));
        else if (subs == QStringLiteral("embed") && (container == QStringLiteral("mkv") || container == QStringLiteral("mp4")
                                                     || container == QStringLiteral("mov") || container == QStringLiteral("webm")))
            out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "Captions will be embedded")));
        else if (subs == QStringLiteral("embed"))
            warn(QCoreApplication::translate("ExportSpec", "This container cannot embed subtitles — writing a sidecar instead"));
    }

    // Range.
    const int rin = spec.value(QStringLiteral("rangeIn"), -1).toInt();
    const int rout = spec.value(QStringLiteral("rangeOut"), -1).toInt();
    if (rin >= 0 || rout >= 0) {
        if (rin < 0 || rout < rin)
            bad(QCoreApplication::translate("ExportSpec", "The In/Out range is empty"));
        else
            out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "In/Out range is valid")));
    }

    // Media and effects.
    const QStringList missing = ctx.value(QStringLiteral("missingMedia")).toStringList();
    if (!missing.isEmpty())
        out.append(row(QStringLiteral("error"),
                       missing.size() == 1 ? QCoreApplication::translate("ExportSpec", "Offline media: %1").arg(missing.first())
                                           : QCoreApplication::translate("ExportSpec", "%1 clips are offline (%2…)").arg(missing.size()).arg(missing.first())));
    else if (ctx.value(QStringLiteral("hasVideo")).toBool())
        out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "All media is online")));
    const QStringList services = ctx.value(QStringLiteral("missingServices")).toStringList();
    if(video && !ctx.value("linearError").toString().isEmpty())out.append(row("error",ctx.value("linearError").toString()));
    for (const QString &s : services)
        out.append(row(QStringLiteral("error"), QCoreApplication::translate("ExportSpec", "Effect not installed: %1").arg(s)));
    // Grades keep a 10- or 12-bit delivery's precision; an effect that works
    // at 8 bits does not, so each clip that has one is named.
    static const QRegularExpression depthDigits(QStringLiteral("(\\d+)(?:le|be)?$"));
    const QRegularExpressionMatch depthMatch = depthDigits.match(pixelFormat(spec));
    const int depth = depthMatch.hasMatch() && depthMatch.captured(1).toInt() >= 10 && depthMatch.captured(1).toInt() <= 16
                          ? depthMatch.captured(1).toInt() : 8;
    if (video && depth > 8)
        for (const QVariant &v : ctx.value(QStringLiteral("eightBitEffects")).toList()) {
            const QVariantMap use = v.toMap();
            const QStringList effects = use.value(QStringLiteral("effects")).toStringList();
            warn((effects.size() == 1
                      ? QCoreApplication::translate("ExportSpec", "%1 on %2 works in 8 bits: that clip will not keep this %3-bit delivery's precision")
                      : QCoreApplication::translate("ExportSpec", "%1 on %2 work in 8 bits: that clip will not keep this %3-bit delivery's precision"))
                     .arg(effects.join(QStringLiteral(", ")), use.value(QStringLiteral("clip")).toString())
                     .arg(depth));
        }

    // The destination: its length, size, frame rate and shape, and the sound
    // levelled for it.
    if (const PlatformRules *pr = platformRules(str(spec, "platform"))) {
        const QString where = QCoreApplication::translate("ExportSpec", pr->name);
        const QString beyond = *pr->beyond ? QStringLiteral(" ") + QCoreApplication::translate("ExportSpec", pr->beyond) : QString();
        const double seconds = ctx.value(QStringLiteral("seconds")).toDouble();
        if (seconds > 0 && pr->maxSeconds > 0 && seconds > pr->maxSeconds + 0.01)
            warn(QCoreApplication::translate("ExportSpec", "%1 takes videos up to %2; this one is %3.")
                     .arg(where, durationText(pr->maxSeconds), durationText(seconds)) + beyond);
        if (seconds > 0 && seconds < pr->minSeconds)
            warn(QCoreApplication::translate("ExportSpec", "%1 needs at least %2; this one is %3.")
                     .arg(where, durationText(pr->minSeconds), durationText(seconds)));
        const qint64 size = ctx.contains("estimatedBytes") ? ctx.value("estimatedBytes").toLongLong()
                                                           : estimateBytes(spec, seconds, ctx.value(QStringLiteral("width")).toInt(),
                                                                           ctx.value(QStringLiteral("height")).toInt(), ctx.value(QStringLiteral("fps")).toDouble());
        if (pr->maxBytes > 0 && size > pr->maxBytes)
            warn(QCoreApplication::translate("ExportSpec", "%1 takes files up to %2; this one will be about %3.")
                     .arg(where, sizeText(pr->maxBytes), sizeText(size)) + beyond);
        if (video) {
            const double edit = ctx.value(QStringLiteral("fps")).toDouble();
            const QString converted = convertedRate(spec, edit);
            double delivered = edit;
            if (!converted.isEmpty()) {
                const int slash = converted.indexOf(QLatin1Char('/'));
                delivered = slash > 0 ? converted.left(slash).toDouble() / converted.mid(slash + 1).toDouble() : converted.toDouble();
            }
            if (delivered > 0 && pr->minFps > 0 && delivered < pr->minFps - 0.01)
                warn(QCoreApplication::translate("ExportSpec", "%1 takes %2 to %3 frames a second; this delivers %4.")
                         .arg(where).arg(pr->minFps).arg(pr->maxFps).arg(delivered, 0, 'g', 4));
            else if (delivered > 0 && pr->maxFps > 0 && delivered > pr->maxFps + 0.01)
                warn(QCoreApplication::translate("ExportSpec", "%1 takes at most %2 frames a second; this delivers %3.")
                         .arg(where).arg(pr->maxFps).arg(delivered, 0, 'g', 4));
            else if (delivered > 0 && qAbs(delivered - edit) > 0.01)
                out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "Delivered at %1 frames a second, as %2 takes it (the edit runs at %3)")
                                                         .arg(delivered, 0, 'g', 4).arg(where).arg(edit, 0, 'g', 4)));
            const int tw = ctx.value(QStringLiteral("width")).toInt(), th = ctx.value(QStringLiteral("height")).toInt();
            const int ow = spec.value(QStringLiteral("width")).toInt() > 0 ? spec.value(QStringLiteral("width")).toInt() : tw;
            const int oh = spec.value(QStringLiteral("height")).toInt() > 0 ? spec.value(QStringLiteral("height")).toInt() : th;
            if (pr->maxLong > 0 && (qMax(ow, oh) > pr->maxLong || qMin(ow, oh) > pr->maxShort))
                warn(QCoreApplication::translate("ExportSpec", "%1 takes pictures up to %2×%3; this one is %4×%5 and will be scaled down there.")
                         .arg(where).arg(ow >= oh ? pr->maxLong : pr->maxShort).arg(ow >= oh ? pr->maxShort : pr->maxLong).arg(ow).arg(oh));
            if (isHdr(spec) && !pr->hdr)
                warn(QCoreApplication::translate("ExportSpec", "%1 does not take HDR: choose Rec.709 so it looks as graded.").arg(where));
        }
    }
    // Reframe to Fit is still moving this timeline's clips.
    if (video && ctx.value(QStringLiteral("reframing")).toBool())
        out.append(row(QStringLiteral("error"), QCoreApplication::translate("ExportSpec", "Reframe to Fit is still working on this timeline's clips: render when it has finished")));
    // Another shape than the edit's: how it fills the frame is asked until
    // chosen (the Deliver page offers the choices and a reframed copy of the
    // timeline on this row).
    if (video) {
        const int tw = ctx.value(QStringLiteral("width")).toInt(), th = ctx.value(QStringLiteral("height")).toInt();
        const int ow = spec.value(QStringLiteral("width")).toInt() > 0 ? spec.value(QStringLiteral("width")).toInt() : tw;
        const int oh = spec.value(QStringLiteral("height")).toInt() > 0 ? spec.value(QStringLiteral("height")).toInt() : th;
        if (tw > 0 && th > 0 && ow > 0 && oh > 0 && qAbs(double(tw) / th - double(ow) / oh) > 0.01 * double(ow) / oh) {
            const QString edit = shapeName(tw, th), delivery = shapeName(ow, oh);
            const QString framing = str(spec, "framing");
            const bool wider = double(tw) / th > double(ow) / oh;
            QVariantMap r;
            if (framing == QLatin1String("fill"))
                r = row(QStringLiteral("ok"), (wider ? QCoreApplication::translate("ExportSpec", "The %1 edit fills the %2 frame; its sides are cropped")
                                                     : QCoreApplication::translate("ExportSpec", "The %1 edit fills the %2 frame; its top and bottom are cropped"))
                                                  .arg(edit, delivery));
            else if (framing == QLatin1String("blur"))
                r = row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "The %1 edit is fitted into the %2 frame over a blurred copy of itself").arg(edit, delivery));
            else if (framing == QLatin1String("fit"))
                r = row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "The %1 edit is fitted into the %2 frame with black bars").arg(edit, delivery));
            else
                r = row(QStringLiteral("warning"), QCoreApplication::translate("ExportSpec", "The edit is %1 and this delivery is %2. Choose how it fills the frame: "
                                                                                              "a %2 copy of the timeline reframed to follow each shot, "
                                                                                              "a blurred backdrop, filled with the edges cropped, or black bars")
                                                       .arg(edit, delivery));
            r.insert(QStringLiteral("shape"), true);
            r.insert(QStringLiteral("framing"), framing);
            r.insert(QStringLiteral("deliveryShape"), delivery);
            r.insert(QStringLiteral("deliveryWidth"), ow);
            r.insert(QStringLiteral("deliveryHeight"), oh);
            out.append(r);
        }
    }
    if (audio && spec.value(QStringLiteral("loudness")).toBool())
        out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "Sound levelled to %1 LUFS, true peaks under %2 dBTP (the project's mix is unchanged)")
                                                 .arg(spec.value(QStringLiteral("loudnessTarget")).toDouble(), 0, 'f', 0)
                                                 .arg(spec.value(QStringLiteral("truePeak")).toDouble(), 0, 'f', 0)));

    // Size against free space.
    const qint64 est = ctx.contains("estimatedBytes") ? ctx.value("estimatedBytes").toLongLong() : estimateBytes(spec, ctx.value(QStringLiteral("seconds")).toDouble(),
                                     ctx.value(QStringLiteral("width")).toInt(), ctx.value(QStringLiteral("height")).toInt(),
                                     ctx.value(QStringLiteral("fps")).toDouble());
    const qint64 freeBytes = ctx.value(QStringLiteral("freeBytes")).toLongLong();
    if (freeBytes > 0 && est > 0) {
        if (est > freeBytes)
            out.append(row(QStringLiteral("error"), QCoreApplication::translate("ExportSpec", "Insufficient disk space: about %1 needed, %2 free")
                                                        .arg(sizeText(est), sizeText(freeBytes))));
        else if (est * 5 > freeBytes * 4)
            warn(QCoreApplication::translate("ExportSpec", "Disk space is tight: about %1 needed, %2 free").arg(sizeText(est), sizeText(freeBytes)));
        else
            out.append(row(QStringLiteral("ok"), QCoreApplication::translate("ExportSpec", "Enough disk space (about %1 needed)").arg(sizeText(est))));
    }
    return out;
}

QString summary(const QVariantMap &in, int tw, int th, double tf) {
    const QVariantMap spec = normalized(in);
    QStringList parts;
    parts << str(spec, "container", QStringLiteral("mp4")).toUpper();
    if (spec.value(QStringLiteral("video")).toBool()) {
        parts << codecTitle(str(spec, "vcodec"));
        const int w = spec.value(QStringLiteral("width")).toInt() > 0 ? spec.value(QStringLiteral("width")).toInt() : tw;
        const int h = spec.value(QStringLiteral("height")).toInt() > 0 ? spec.value(QStringLiteral("height")).toInt() : th;
        if (w > 0 && h > 0)
            parts << QStringLiteral("%1×%2").arg(w).arg(h);
        else
            parts << QCoreApplication::translate("ExportSpec", "Timeline size");
        const double f = spec.value(QStringLiteral("fps")).toDouble() > 0.0 ? spec.value(QStringLiteral("fps")).toDouble() : tf;
        if (f > 0.0)
            parts << QCoreApplication::translate("ExportSpec", "%1 fps").arg(QString::number(f, 'g', 6));
    } else {
        parts << QCoreApplication::translate("ExportSpec", "Audio only");
    }
    if (spec.value(QStringLiteral("audio")).toBool())
        parts << audioTitle(str(spec, "acodec"));
    return parts.join(QStringLiteral(" · "));
}

QString tidyFileName(const QString &in) {
    static const QString forbidden = QStringLiteral("/\\:*?\"<>|");
    QString s;
    s.reserve(in.size());
    for (const QChar c : in) {
        if (forbidden.contains(c)) s += QLatin1Char('-');
        // A size written the way it reads, 1080×1920, is typed 1080x1920 in a
        // file's name: the multiplication sign is awkward in a terminal and
        // on upload forms.
        else if (c == QChar(0x00D7)) s += QLatin1Char('x');
        else if (c.isSpace() || c.category() == QChar::Other_Control) s += QLatin1Char(' ');
        else s += c;
    }
    const auto separator = [](QChar c) { return c == QLatin1Char(' ') || c == QLatin1Char('_') || c == QLatin1Char('-'); };
    QString out;
    out.reserve(s.size());
    for (qsizetype i = 0; i < s.size();) {
        if (!separator(s.at(i))) { out += s.at(i++); continue; }
        bool space = false, hyphen = false;
        for (; i < s.size() && separator(s.at(i)); ++i) {
            space |= s.at(i) == QLatin1Char(' ');
            hyphen |= s.at(i) == QLatin1Char('-');
        }
        out += hyphen ? (space ? QStringLiteral(" - ") : QStringLiteral("-")) : space ? QStringLiteral(" ") : QStringLiteral("_");
    }
    qsizetype first = 0, last = out.size();
    while (first < last && separator(out.at(first))) ++first;
    while (last > first && separator(out.at(last - 1))) --last;
    return out.mid(first, last - first);
}

QString expandFileName(const QString &pattern, const QVariantMap &tokens, const QString &dir, const QString &ext) {
    QString name = pattern;
    auto sub = [&](const char *token, const QString &value) {
        name.replace(QStringLiteral("{%1}").arg(QLatin1String(token)), value, Qt::CaseInsensitive);
    };
    sub("project", tokens.value(QStringLiteral("project"), QStringLiteral("Untitled")).toString());
    sub("sequence", tokens.value(QStringLiteral("sequence"), QStringLiteral("Sequence 1")).toString());
    sub("preset", tokens.value(QStringLiteral("preset"), QStringLiteral("Custom")).toString());
    sub("resolution", tokens.value(QStringLiteral("resolution")).toString());
    sub("fps", tokens.value(QStringLiteral("fps")).toString());
    sub("date", tokens.value(QStringLiteral("date"), QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"))).toString());
    sub("time", tokens.value(QStringLiteral("time"), QTime::currentTime().toString(QStringLiteral("HHmm"))).toString());
    name = tidyFileName(name);
    if (name.contains(QStringLiteral("{version}"), Qt::CaseInsensitive)) {
        for (int v = 1; v < 10000; ++v) {
            QString candidate = name;
            candidate.replace(QStringLiteral("{version}"), QStringLiteral("v%1").arg(v), Qt::CaseInsensitive);
            if (dir.isEmpty() || !QFileInfo::exists(QDir(dir).filePath(candidate + ext))) {
                name = std::move(candidate);
                break;
            }
        }
    }
    name = name.trimmed();
    if (name.endsWith(ext, Qt::CaseInsensitive))
        name.chop(ext.size());
    return name;
}

} // namespace ExportSpec
