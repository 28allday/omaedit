#include "projectarchive.h"
#include "projectfile.h"
#include "projectsafety.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QUrl>
#include <functional>
#include <limits>

namespace {
using Stop = std::shared_ptr<std::atomic_bool>;
struct Range {
    int first = std::numeric_limits<int>::max();
    int last = 0;
    bool safe = true;
};
bool run(QProcess &p, const QString &program, const QStringList &args, const Stop &stop, QString *error,
         QByteArray *output = nullptr) {
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(program, args);
    if (!p.waitForStarted(5000)) {
        *error = "Could not start " + program;
        return false;
    }
    QByteArray data;
    while (!p.waitForFinished(100)) {
        data += p.readAll();
        if (data.size() > 1024 * 1024)
            data = data.right(1024 * 1024);
        if (stop->load()) {
            p.kill();
            p.waitForFinished(3000);
            *error = "Archive cancelled.";
            return false;
        }
    }
    data += p.readAll();
    if (output)
        *output = data;
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        *error = program + " failed: " + QString::fromUtf8(data.right(1800));
        return false;
    }
    return true;
}
// A multicam angle's source frame is the multicam frame minus its offset.
// A trimmed copy starts `start` frames into the source, so the same
// multicam frame lands `start` frames earlier in it: the offset grows by
// that, and the angle is only as long as the copy.
QVariantList shiftAngles(QVariantList angles, const QMap<QString, int> &starts,
                         const QMap<QString, int> &lengths) {
    for (auto &a : angles) {
        auto angle = a.toMap();
        const QString path = angle.value("path").toString();
        if (!starts.contains(path))
            continue;
        angle["offset"] = angle.value("offset").toInt() + starts[path];
        angle["frames"] = lengths[path];
        a = angle;
    }
    return angles;
}
QVariant shift(const QVariant &value, const QMap<QString, int> &starts, const QMap<QString, int> &lengths,
               double fps) {
    if (value.metaType().id() == QMetaType::QVariantList) {
        QVariantList out;
        for (const auto &v : value.toList())
            out.append(shift(v, starts, lengths, fps));
        return out;
    }
    if (value.metaType().id() != QMetaType::QVariantMap)
        return value;
    auto m = value.toMap();
    for (auto i = m.begin(); i != m.end(); ++i) {
        if (i.key() == "customMetadata")
            continue;
        if (i.key() == "mediaSources") {
            auto sources = i.value().toMap();
            for (auto s = sources.begin(); s != sources.end(); ++s) {
                auto meta = s.value().toMap();
                const bool hadPath = meta.contains("path");
                if (!hadPath) meta["path"] = s.key();
                meta = shift(meta, starts, lengths, fps).toMap();
                if (!hadPath) meta.remove("path");
                s.value() = meta;
            }
            i.value() = sources;
        } else if (i.key() == "compoundJson")
            i.value() = QString::fromUtf8(
                QJsonDocument::fromVariant(
                    shift(QJsonDocument::fromJson(i.value().toString().toUtf8()).toVariant(), starts, lengths,
                          fps))
                    .toJson(QJsonDocument::Compact));
        else if (i.key() == "library")
            i.value() = ProjectSafety::encodeLibrary(
                shift(ProjectSafety::libraryRecords(i.value().toStringList()), starts, lengths, fps)
                    .toList());
        else if (i.key() == "mcam" && !i.value().toString().isEmpty()) { // a placed multicam clip's tag
            auto tag = QJsonDocument::fromJson(i.value().toString().toUtf8()).toVariant().toMap();
            tag["angles"] = shiftAngles(tag.value("angles").toList(), starts, lengths);
            if (tag.contains("sound")) tag["sound"] = shiftAngles({tag.value("sound")}, starts, lengths).first();
            if (tag.contains("recordings")) tag["recordings"] = shiftAngles(tag.value("recordings").toList(), starts, lengths);
            i.value() = QString::fromUtf8(QJsonDocument::fromVariant(tag).toJson(QJsonDocument::Compact));
        } else if ((i.key() == "angles" || i.key() == "recordings") && m.value("multicam").toBool()) // a multicam library item, and the recordings lined up with it
            i.value() = shiftAngles(i.value().toList(), starts, lengths);
        else if (i.key() == "sound" && m.value("multicam").toBool()) // and its sound source
            i.value() = shiftAngles({i.value()}, starts, lengths).first();
        else
            i.value() = shift(i.value(), starts, lengths, fps);
    }
    const QString path = m.value("path").toString();
    if (!starts.contains(path))
        return m;
    const int offset = starts[path], length = lengths[path];
    for (const auto &key : QStringList{"in", "out", "rangeIn", "rangeOut"})
        if (m.contains(key) && m[key].toInt() >= 0)
            m[key] = qBound(0, m[key].toInt() - offset, qMax(0, length - 1));
    if (m.contains("length"))
        m["length"] = length;
    auto adjustMeta = [&](QVariantMap meta) {
        for (const auto &key : QStringList{"markers", "clipMarkers"})
            if (meta.contains(key)) {
                QVariantList list;
                for (const auto &v : meta[key].toList()) {
                    auto k = v.toMap();
                    k["frame"] = k.value("frame").toInt() - offset;
                    if (k["frame"].toInt() >= 0 && k["frame"].toInt() < length)
                        list.append(k);
                }
                meta[key] = list;
            }
        if (meta.contains("duration"))
            meta["duration"] = length;
        // Transcript timestamps are source-relative milliseconds.
        if (meta.contains("transcript")) {
            QVariantList lines;
            for (const auto &v : meta["transcript"].toList()) {
                auto line = v.toMap();
                line["start"] = qBound(0, line.value("start").toInt() - qRound(offset / fps * 1000), qRound(length / fps * 1000));
                line["end"] = qBound(0, line.value("end").toInt() - qRound(offset / fps * 1000), qRound(length / fps * 1000));
                // Its words move with it; those cut away go.
                if (line.contains("words")) {
                    QVariantList words;
                    for (const auto &w : line.value("words").toList()) {
                        auto word = w.toMap();
                        word["start"] = qBound(0, word.value("start").toInt() - qRound(offset / fps * 1000), qRound(length / fps * 1000));
                        word["end"] = qBound(0, word.value("end").toInt() - qRound(offset / fps * 1000), qRound(length / fps * 1000));
                        if (word["end"].toInt() > word["start"].toInt())
                            words.append(word);
                    }
                    line["words"] = words;
                }
                if (line["end"].toInt() > line["start"].toInt())
                    lines.append(line);
            }
            meta["transcript"] = lines;
        }
        return meta;
    };
    m = adjustMeta(m);
    for (const auto &key : QStringList{"meta", "sourceMeta"})
        if (m.contains(key))
            m[key] = adjustMeta(m[key].toMap());
    return m;
}
} // namespace
ProjectArchive::~ProjectArchive() {
    cancel();
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
}
void ProjectArchive::cancel() {
    if (m_stop)
        m_stop->store(true);
}
void ProjectArchive::start(const QVariantMap &document, const QString &folder, const QString &name,
                           bool includeUnused, bool trim, int handles) {
    if (busy())
        return;
    m_state = {{"progress", 0}, {"message", "Preparing archive…"}, {"path", QString()}, {"error", QString()}};
    m_stop = std::make_shared<std::atomic_bool>(false);
    const auto stop = m_stop;
    m_thread = QThread::create([this, document, folder, name, includeUnused, trim, handles, stop] {
        QString error, result;
        QVariantList manifest;
        QVariantMap finalState;
        const auto progress = [this](int percent, const QString &message) {
            QMetaObject::invokeMethod(
                this,
                [this, percent, message] {
                    m_state["progress"] = percent;
                    m_state["message"] = message;
                    emit changed();
                },
                Qt::QueuedConnection);
        };
        auto work = [&]() -> bool {
            if (name.trimmed().isEmpty() || name == "." || name == ".." || name.contains('/') ||
                name.contains('\\')) {
                error = "Choose a simple archive folder name.";
                return false;
            }
            if (!QDir(folder).exists()) {
                error = "The destination folder is unavailable.";
                return false;
            }
            result = QDir(folder).absoluteFilePath(name);
            if (QFileInfo::exists(result)) {
                error = "That archive folder already exists. Choose a different name.";
                return false;
            }
            QTemporaryDir stage(QDir(folder).filePath(".omaedit-archive-XXXXXX"));
            if (!stage.isValid()) {
                error = "Cannot create an archive in that folder.";
                return false;
            }
            QDir().mkpath(stage.filePath("Media"));
            auto doc = ProjectSafety::usedDocument(document, includeUnused);
            const auto profile = doc.value("profile").toMap();
            const double fps =
                double(profile.value("fpsNum", 25).toInt()) / qMax(1, profile.value("fpsDen", 1).toInt());
            QMap<QString, Range> ranges;
            QSet<QString> wholeSources;
            // A proxy and its original must keep identical time origins.
            // Sources from nested projects can also have a different timebase.
            const auto sources = doc.value("mediaSources").toMap();
            for (auto s = sources.begin(); s != sources.end(); ++s)
                if (!s.value().toMap().value("proxyPath").toString().isEmpty() ||
                    !s.value().toMap().value("originalPath").toString().isEmpty())
                    wholeSources.insert(s.key());
            const auto nested = doc.value("nestedDocuments").toMap();
            for (auto n = nested.begin(); n != nested.end(); ++n)
                wholeSources.unite(ProjectSafety::assets(n.value().toMap()));
            // Inactive timelines store ranges in their own timebase. Keep their
            // sources whole unless that rate is known to match the active one.
            for (const auto &record : ProjectSafety::libraryRecords(doc.value("library").toStringList())) {
                for (const auto &value : record.toMap().value("sequences").toMap().value("list").toList()) {
                    const auto sequence = value.toMap();
                    const auto settings = sequence.value("settings").toMap();
                    const int num = settings.value("fpsNum").toInt(), den = settings.value("fpsDen").toInt();
                    if (!settings.value("custom").toBool() || num <= 0 || den <= 0 ||
                        qAbs(double(num) / den - fps) > .0000001)
                        wholeSources.unite(ProjectSafety::assets({{"entries", sequence.value("snapshot")}}));
                }
            }
            // Versioned multicams retain every angle's source origin and
            // coverage so switching remains possible after consolidation.
            const auto keepCamera=[&](const QVariantMap &camera) {
                if(camera.value("version").toInt()!=2 && camera.value("multicamVersion").toInt()!=2)return;
                for(const auto &value:camera.value("angles").toList())wholeSources.insert(value.toMap().value("path").toString());
                if(const auto sound=camera.value("sound").toMap().value("path").toString();!sound.isEmpty())wholeSources.insert(sound);
                for(const auto &value:camera.value("recordings").toList())wholeSources.insert(value.toMap().value("path").toString());
            };
            for(const auto &value:ProjectSafety::libraryRecords(doc.value("library").toStringList()))
                keepCamera(value.toMap().value("logging").toMap());
            for(const auto &value:ProjectSafety::clipEntries(doc))
                keepCamera(QJsonDocument::fromJson(value.toMap().value("mcam").toString().toUtf8()).toVariant().toMap());
            for (const auto &v : ProjectSafety::clipEntries(doc)) {
                const auto e = v.toMap();
                const auto path = e.value("path").toString();
                auto &r = ranges[path];
                r.first = qMin(r.first, e.value("in").toInt());
                r.last = qMax(r.last, e.value("out").toInt());
                // in/out are source frames only for a plain clip: a speed map,
                // sample timing or a frame-rate interpretation reads elsewhere.
                const auto meta = e.value("sourceMeta").toMap();
                const double forced = meta.value("forceFps").toDouble();
                const double native = meta.value("sourceFps", meta.value("fps")).toDouble();
                const bool interpreted = forced > 0 && native > 0 && !qFuzzyCompare(forced, native);
                r.safe = r.safe && qFuzzyCompare(e.value("speed", 1.).toDouble(), 1.) &&
                         !e.contains("retime") && !e.contains("audioTiming") && !interpreted &&
                         !e.contains("freeze") && !e.contains("sourceOrigin") && e.value("stab").toMap().isEmpty() &&
                         !e.contains("renderOriginal") &&
                         !wholeSources.contains(path) && e.value("proxyPath").toString().isEmpty() &&
                         e.value("originalPath").toString().isEmpty() &&
                         e.value("sourceMeta").toMap().value("proxyPath").toString().isEmpty() &&
                         e.value("sourceMeta").toMap().value("originalPath").toString().isEmpty();
            }
            auto paths = ProjectSafety::assets(doc).values();
            std::sort(paths.begin(), paths.end());
            // Resolve everything before copying. Missing dependencies must never
            // produce an archive which merely looks successful.
            for (const auto &path : paths)
                if (!QFileInfo(path).isFile()) {
                    error = "Missing archive dependency: " + path;
                    return false;
                }
            QVariantMap mapping;
            QMap<QString, int> starts, lengths;
            int done = 0;
            for (const auto &path : paths) {
                if (stop->load()) {
                    error = "Archive cancelled.";
                    return false;
                }
                const QFileInfo info(path);
                const auto sourceSize = info.size();
                const auto sourceModified = info.lastModified();
                const auto hash = QString::fromLatin1(
                    QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
                QString fileName = hash + '-' + info.fileName();
                bool trimmed = false;
                int first = 0, last = 0;
                const auto r = ranges.value(path);
                const bool candidate = trim && !includeUnused && ranges.contains(path) && r.safe &&
                                       r.first >= 0 && r.last >= r.first;
                if (candidate) {
                    QProcess probe;
                    QByteArray bytes;
                    QString probeError;
                    if (run(probe, "ffprobe",
                            {"-v", "error", "-show_streams", "-show_format", "-of", "json", path}, stop,
                            &probeError, &bytes)) {
                        const auto meta = QJsonDocument::fromJson(bytes).toVariant().toMap();
                        bool video = false, audio = false, matching = true;
                        int videoStreams = 0, audioStreams = 0;
                        double duration = meta.value("format").toMap().value("duration").toDouble();
                        for (const auto &s : meta.value("streams").toList()) {
                            const auto stream = s.toMap();
                            const auto kind = stream.value("codec_type").toString();
                            audio |= kind == "audio";
                            if (kind == "audio") ++audioStreams;
                            if (kind == "video") {
                                ++videoStreams;
                                video = true;
                                const auto rate = stream.value("avg_frame_rate").toString().split('/');
                                const double sfps =
                                    rate.size() == 2 ? rate[0].toDouble() / qMax(1., rate[1].toDouble()) : 0.;
                                matching = matching && qAbs(sfps - fps) < .001 &&
                                           stream.value("nb_frames").toString() != "1";
                            }
                        }
                        first = qMax(0, r.first - qMax(0, handles));
                        last = qMin(qMax(0, qRound(duration * fps) - 1), r.last + qMax(0, handles));
                        if ((video || audio) && videoStreams <= 1 && audioStreams <= 1 && matching && duration > 0 && last >= r.last &&
                            (first > 0 || last + 1 < qRound(duration * fps))) {
                            fileName = hash + '-' + info.completeBaseName() + ".mkv";
                            QStringList args{"-hide_banner", "-loglevel", "error", "-nostdin",
                                             "-y",           "-i",        path};
                            if (video)
                                args << "-map" << "0:v:0" << "-vf"
                                     << QString("trim=start_frame=%1:end_frame=%2,setpts=PTS-STARTPTS")
                                            .arg(first)
                                            .arg(last + 1)
                                     << "-c:v" << "ffv1" << "-level" << "3";
                            if (audio)
                                args << "-map" << "0:a:0" << "-af"
                                     << QString("atrim=start=%1:end=%2,asetpts=PTS-STARTPTS")
                                            .arg(first / fps, 0, 'f', 9)
                                            .arg((last + 1) / fps, 0, 'f', 9)
                                     << "-c:a" << "pcm_s24le";
                            args << stage.filePath("Media/" + fileName);
                            QProcess encode;
                            progress(qRound(95. * done / qMax(1, paths.size())),
                                     "Trimming " + info.fileName());
                            if (!run(encode, "ffmpeg", args, stop, &error))
                                return false;
                            trimmed = true;
                            starts[path] = first;
                            lengths[path] = last - first + 1;
                        }
                    } else if (stop->load()) {
                        error = "Archive cancelled.";
                        return false;
                    }
                }
                if (!trimmed) {
                    QFile source(path), target(stage.filePath("Media/" + fileName));
                    if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
                        error = "Cannot copy " + info.fileName();
                        return false;
                    }
                    while (!source.atEnd()) {
                        if (stop->load()) {
                            error = "Archive cancelled.";
                            return false;
                        }
                        const auto chunk = source.read(1024 * 1024);
                        if (chunk.isEmpty() || target.write(chunk) != chunk.size()) {
                            error = "Copy failed for " + info.fileName() + ": " + target.errorString();
                            return false;
                        }
                        progress(qRound(95. * (done + double(source.pos()) / qMax(qint64(1), info.size())) /
                                        qMax(1, paths.size())),
                                 "Copying " + info.fileName());
                    }
                    if (!target.flush()) {
                        error = "Could not finish copying " + info.fileName();
                        return false;
                    }
                }
                const QString relative = "./Media/" + fileName;
                const QFileInfo after(path);
                if (after.size() != sourceSize || after.lastModified() != sourceModified) {
                    error = "Source changed while archiving: " + info.fileName();
                    return false;
                }
                mapping[path] = relative;
                manifest.append(
                    QVariantMap{{"source", path},
                                {"archived", relative},
                                {"trimmed", trimmed},
                                {"sourceStart", trimmed ? first : 0},
                                {"bytes", QFileInfo(stage.filePath("Media/" + fileName)).size()}});
                ++done;
            }
            if (stop->load()) {
                error = "Archive cancelled.";
                return false;
            }
            doc = shift(doc, starts, lengths, fps).toMap();
            doc = ProjectSafety::rewriteDocument(doc, mapping);
            doc.remove("recovery");
            doc["archive"] =
                QVariantMap{{"version", 1},
                            {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                            {"handles", trim ? handles : 0}};
            if (!ProjectSafety::writeDocument(stage.filePath("Project" + ProjectFile::extension()), doc, &error))
                return false;
            QSaveFile report(stage.filePath("Archive.json"));
            const auto bytes = QJsonDocument::fromVariant(QVariantMap{{"version", 1},
                                                                      {"files", manifest},
                                                                      {"includeUnused", includeUnused},
                                                                      {"trimRequested", trim}})
                                   .toJson(QJsonDocument::Indented);
            if (!report.open(QIODevice::WriteOnly) || report.write(bytes) != bytes.size() ||
                !report.commit()) {
                error = "Could not save the archive manifest.";
                return false;
            }
            if (stop->load()) {
                error = "Archive cancelled.";
                return false;
            }
            if (!QDir().rename(stage.path(), result)) {
                error = "Could not finalize the archive folder.";
                return false;
            }
            stage.setAutoRemove(false);
            return true;
        };
        const bool ok = work();
        QMetaObject::invokeMethod(
            this,
            [this, ok, result, error, manifest] {
                m_state = {{"progress", ok ? 100 : 0},
                           {"message", ok ? "Archive complete" : error},
                           {"path", ok ? result : QString()},
                           {"error", error},
                           {"files", manifest}};
            },
            Qt::QueuedConnection);
        QMetaObject::invokeMethod(
            this, [this, ok, result, error] { emit finished(ok, ok ? result : QString(), error); },
            Qt::QueuedConnection);
    });
    connect(m_thread, &QThread::finished, this, [this] {
        auto *thread = m_thread;
        m_thread = nullptr;
        thread->deleteLater();
        m_stop.reset();
        emit changed();
    });
    emit changed();
    m_thread->start();
}
