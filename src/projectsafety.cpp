#include "projectsafety.h"
#include "markeredit.h"
#include "rendersnapshot.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include "storagepreferences.h"
#include "keptmedia.h"
#include "projectfile.h"

#include <QUrl>
#include <QUuid>
#include <QXmlStreamReader>
#include <functional>

namespace {
bool assetKey(const QString &key) {
    return QStringList{"path", "resource", "proxyPath", "originalPath", "source", "externalMatte", "results", "inputLut"}
        .contains(key);
}
QVariant walk(const QVariant &value, const std::function<QString(const QString &)> &path) {
    if (value.metaType().id() == QMetaType::QVariantList || value.metaType().id() == QMetaType::QStringList) {
        QVariantList out;
        for (const auto &v : value.toList())
            out.append(walk(v, path));
        return out;
    }
    if (value.metaType().id() != QMetaType::QVariantMap)
        return value;
    auto map = value.toMap();
    for (auto it = map.begin(); it != map.end(); ++it) {
        // These keys are supplied by the user, even when named path/source.
        if (it.key() == "customMetadata")
            continue;
        if (assetKey(it.key()) && it.value().metaType().id() == QMetaType::QString)
            it.value() = path(it.value().toString());
        // A multicam clip's tag names every angle's file, not only the one
        // it shows: those are media the project needs too.
        // The text is kept as it was unless a path in it changed: a caller
        // compares a multicam tag as a string, and re-serialising reorders it.
        else if (it.key() == "compoundJson" || it.key() == "mcam") {
            const auto before = QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant();
            const auto after = walk(before, path);
            if (after != before)
                it.value() = QString::fromUtf8(QJsonDocument::fromVariant(after).toJson(QJsonDocument::Compact));
        }
        else if (it.key() == "library")
            it.value() = ProjectSafety::encodeLibrary(
                walk(ProjectSafety::libraryRecords(it.value().toStringList()), path).toList());
        else if (it.key() == "mediaSources" || it.key() == "nestedDocuments") {
            QVariantMap renamed;
            const auto values = it.value().toMap();
            for (auto item = values.begin(); item != values.end(); ++item)
                renamed[path(item.key())] = walk(item.value(), path);
            it.value() = renamed;
        } else
            it.value() = walk(it.value(), path);
    }
    return map;
}
// The library items placed multicam clips come from, as their library
// paths ("multicam:<id>"): a clip's tag names its item by id.
void placedMulticams(const QVariant &value, QSet<QString> *paths) {
    if (value.metaType().id() == QMetaType::QVariantList) {
        for (const auto &v : value.toList())
            placedMulticams(v, paths);
        return;
    }
    if (value.metaType().id() != QMetaType::QVariantMap)
        return;
    const auto map = value.toMap();
    for (auto it = map.begin(); it != map.end(); ++it) {
        if (it.key() == "customMetadata" || it.key() == "library")
            continue;
        if (it.key() == "mcam" || it.key() == "compoundJson") {
            const auto tag = QJsonDocument::fromJson(it.value().toString().toUtf8()).toVariant();
            if (it.key() == "mcam" && !tag.toMap().value("id").toString().isEmpty())
                paths->insert("multicam:" + tag.toMap().value("id").toString());
            placedMulticams(tag, paths);
        } else
            placedMulticams(it.value(), paths);
    }
}
QString digest(const QByteArray &value) {
    return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex());
}
} // namespace

QString ProjectSafety::root() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/project-safety";
}
bool ProjectSafety::isFileReference(const QString &path) {
    // Colour resources also back titles/captions, in both numeric
    // forms (#RRGGBB[AA] and 0xRRGGBBAA). They are generated pixels,
    // not relative media filenames. An actual ./#RRGGBB file stays a path.
    static const QRegularExpression colorResource(
        QStringLiteral("^(#[0-9a-fA-F]{6}([0-9a-fA-F]{2})?|0x[0-9a-fA-F]{8})$"));
    // The editor's own generator identifiers (Timeline::generatorResource).
    // The timeline itself reads a clip path spelled like this as the
    // generator, so such a path never names a media file in a project.
    static const QStringList generators{"noise", "count", "frei0r.test_pat_B"};
    if (colorResource.match(path).hasMatch() || generators.contains(path)) return false;
    return !path.isEmpty() && !path.startsWith(':') && !path.contains("://") &&
           (QFileInfo(path).isAbsolute() || !path.contains(':'));
}
qint64 ProjectSafety::documentLimit() {
    // Far above any real project (measured: a graded clip weighs about 3 KB
    // in the open timeline and 5 KB in each other one, so 64 MiB, the limit
    // the reader alone once had, is some fifteen thousand clips of alternate
    // cuts). It only keeps a file that is not a project from being read whole.
    return qint64(1024) * 1024 * 1024;
}
QVariantMap ProjectSafety::readDocument(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > documentLimit()) {
        if (error)
            *error = "Cannot read project: " + path;
        return {};
    }
    const QByteArray bytes = file.readAll();
    QVariantMap doc;
    bool broken = false;
    if (bytes.trimmed().startsWith('{')) {
        QJsonParseError parse;
        doc = QJsonDocument::fromJson(bytes, &parse).toVariant().toMap();
        broken = parse.error != QJsonParseError::NoError;
    } else {
        // Earlier builds wrapped the same document in XML, as one property.
        QXmlStreamReader xml(bytes);
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == "property" &&
                (xml.attributes().value("name") == "omavid:snapshot" ||
                 xml.attributes().value("name") == "omavid:emptyProject"))
                doc = QJsonDocument::fromJson(xml.readElementText().toUtf8()).toVariant().toMap();
        }
        broken = xml.hasError();
    }
    const int version=doc.value("version").toInt();
    if (broken || (version!=1 && version!=2) || !doc.contains("library") ||
        !doc.contains("entries")) {
        if (error)
            *error = "The project copy is incomplete or unsupported: " + path;
        return {};
    }
    return KeptMedia::named(bytes) ? withKeptMedia(doc) : doc;
}
QVariantMap ProjectSafety::withKeptMedia(const QVariantMap &document) {
    return walk(document, [](const QString &p) { return KeptMedia::resolve(p); }).toMap();
}
bool ProjectSafety::isDocument(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = file.read(4096).trimmed();
    if (!head.startsWith('{'))
        return head.contains("omavid:snapshot") || head.contains("omavid:emptyProject");
    if (head.contains("\"library\"") || head.contains("\"version\""))
        return true;
    // The writer puts the keys in alphabetical order: the clip list comes
    // before both, so a project of any size has them past the first few
    // kilobytes. "version" is the last key, at the very end of the file.
    if (file.size() <= 4096 || !file.seek(file.size() - 4096))
        return false;
    return file.read(4096).contains("\"version\"");
}
bool ProjectSafety::writeDocument(const QString &path, const QVariantMap &document, QString *error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument::fromVariant(document).toJson(QJsonDocument::Compact);
    // The reader's limit is the writer's too: a save that could not be opened
    // again is refused here, with the file already on disk left as it was.
    if (bytes.size() > documentLimit()) {
        file.cancelWriting();
        if (error)
            *error = "The project is too large to save.";
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString();
        return false;
    }
    return true;
}
QString ProjectSafety::encodeLibraryPath(const QString &path) {
    // The version tag distinguishes new escaping from legacy literal %25 names.
    if (path.contains('%') || path.contains('\t') || path.contains('\n') || path.contains('\r') ||
        path.startsWith("@omaedit-path-v2:"))
        return QStringLiteral("@omaedit-path-v2:") + QString::fromLatin1(QUrl::toPercentEncoding(path));
    return path;
}
QString ProjectSafety::decodeLibraryPath(const QString &field) {
    const QString prefix = QStringLiteral("@omaedit-path-v2:");
    if (field.startsWith(prefix))
        return QUrl::fromPercentEncoding(field.mid(prefix.size()).toLatin1());
    QString out = field;
    out.replace(QStringLiteral("%09"), QStringLiteral("\t")).replace(QStringLiteral("%0A"), QStringLiteral("\n")).replace(QStringLiteral("%0D"), QStringLiteral("\r"));
    return out;
}
QVariantList ProjectSafety::libraryRecords(const QStringList &library) {
    QVariantList out;
    for (const auto &line : library) {
        if (line.startsWith("::sequences=")) {
            out.append(QVariantMap{{"sequences", QJsonDocument::fromJson(
                                                     QByteArray::fromPercentEncoding(line.mid(12).toLatin1()))
                                                     .toVariant()}});
        } else if (line.startsWith("::"))
            out.append(QVariantMap{{"raw", line}});
        else {
            const auto fields = line.split('\t');
            QVariantMap r{{"path", decodeLibraryPath(fields.first())}};
            if (fields.size() > 1)
                r["in"] = fields[1].toInt();
            if (fields.size() > 2)
                r["out"] = fields[2].toInt();
            if (fields.size() > 3 && fields[3].startsWith("meta="))
                r["meta"] =
                    QJsonDocument::fromJson(QByteArray::fromPercentEncoding(fields[3].mid(5).toLatin1()))
                        .toVariant();
            out.append(r);
        }
    }
    return out;
}
QVariantList ProjectSafety::decodeLibraryState(const QStringList &library) {
    // Built on libraryRecords, the one parser of the line format; this adds
    // the editor-only lines and the clip's file check.
    QVariantList out;
    for (const QVariant &value : libraryRecords(library)) {
        const QVariantMap r = value.toMap();
        if (r.contains(QStringLiteral("sequences"))) {
            const QVariantMap doc = r.value(QStringLiteral("sequences")).toMap();
            // Each stashed timeline carries its markers in its snapshot's
            // trailing sentinel.
            QVariantList list = doc.value(QStringLiteral("list")).toList();
            for (QVariant &item : list) {
                QVariantMap sequence = item.toMap();
                QVariantList snapshot = sequence.value(QStringLiteral("snapshot")).toList();
                QVariantMap last = snapshot.isEmpty() ? QVariantMap() : snapshot.last().toMap();
                if (!last.contains(QStringLiteral("omavid:markers")))
                    continue;
                last.insert(QStringLiteral("omavid:markers"),
                            MarkerEdit::knownTypes(last.value(QStringLiteral("omavid:markers")).toList()));
                snapshot.last() = last;
                sequence.insert(QStringLiteral("snapshot"), snapshot);
                item = sequence;
            }
            out.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("sequences")},
                                   {QStringLiteral("list"), list},
                                   {QStringLiteral("active"), doc.value(QStringLiteral("active")).toInt()}});
            continue;
        }
        if (r.contains(QStringLiteral("raw"))) {
            const QString line = r.value(QStringLiteral("raw")).toString();
            if (line.startsWith(QStringLiteral("::media="))) {
                out.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("media")},
                                       {QStringLiteral("document"), QJsonDocument::fromJson(
                                            QByteArray::fromPercentEncoding(line.mid(8).toLatin1())).object().toVariantMap()}});
            } else if (line.startsWith(QStringLiteral("::markers="))) {
                // File order; the editor sorts its merged marker list once.
                QVariantList markers;
                for (const QString &part : line.mid(10).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    const QStringList bits = part.split(QLatin1Char(':'));
                    QVariantMap marker;
                    marker.insert(QStringLiteral("frame"), bits.value(0).toInt());
                    marker.insert(QStringLiteral("type"),
                                  bits.size() > 1 && !bits.at(1).isEmpty() ? bits.at(1) : QStringLiteral("standard"));
                    marker.insert(QStringLiteral("text"),
                                  QString::fromUtf8(QByteArray::fromPercentEncoding(bits.value(2).toUtf8())));
                    // notes: a fourth, percent-encoded field ("" when absent,
                    // so a reloaded marker equals the one that was saved)
                    marker.insert(QStringLiteral("notes"),
                                  QString::fromUtf8(QByteArray::fromPercentEncoding(bits.value(3).toUtf8())));
                    // colour: a fifth field, one of the marker colours
                    if (MarkerEdit::colours().contains(bits.value(4)))
                        marker.insert(QStringLiteral("colour"), bits.value(4));
                    markers.append(marker);
                }
                out.append(QVariantMap{{QStringLiteral("kind"), QStringLiteral("markers")},
                                       {QStringLiteral("markers"), MarkerEdit::knownTypes(markers)}});
            }
            continue;
        }
        const QString path = r.value(QStringLiteral("path")).toString();
        // A range needs both ends, exactly as the line format writes it.
        const bool ranged = r.contains(QStringLiteral("out"));
        QVariantMap record{{QStringLiteral("kind"), QStringLiteral("clip")},
                           {QStringLiteral("path"), path},
                           {QStringLiteral("rangeIn"), ranged ? r.value(QStringLiteral("in")).toInt() : -1},
                           {QStringLiteral("rangeOut"), ranged ? r.value(QStringLiteral("out")).toInt() : -1},
                           {QStringLiteral("exists"), QFileInfo::exists(path)}};
        if (r.contains(QStringLiteral("meta")))
            record.insert(QStringLiteral("logging"), r.value(QStringLiteral("meta")).toMap());
        out.append(record);
    }
    return out;
}
QStringList ProjectSafety::encodeLibrary(const QVariantList &records) {
    QStringList out;
    for (const auto &v : records) {
        const auto r = v.toMap();
        if (r.contains("raw"))
            out << r["raw"].toString();
        else if (r.contains("sequences"))
            out << "::sequences=" +
                       QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(
                           QJsonDocument::fromVariant(r["sequences"]).toJson(QJsonDocument::Compact))));
        else {
            QString line = encodeLibraryPath(r.value("path").toString());
            if (r.contains("in") || r.contains("out") || r.contains("meta"))
                line += '\t' + QString::number(r.value("in", -1).toInt()) + '\t' +
                        QString::number(r.value("out", -1).toInt());
            if (r.contains("meta"))
                line +=
                    "\tmeta=" + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(
                                    QJsonDocument::fromVariant(r["meta"]).toJson(QJsonDocument::Compact))));
            out << line;
        }
    }
    return out;
}
QVariantList ProjectSafety::clipEntries(const QVariant &value) {
    QVariantList out;
    if (value.metaType().id() == QMetaType::QVariantList)
        for (const auto &v : value.toList())
            out += clipEntries(v);
    else if (value.metaType().id() == QMetaType::QVariantMap) {
        const auto m = value.toMap();
        if (m.contains("path") && m.contains("in") && m.contains("out"))
            out.append(m);
        for (auto i = m.begin(); i != m.end(); ++i) {
            if (i.key() == "customMetadata")
                continue;
            if (i.key() == "compoundJson")
                out += clipEntries(QJsonDocument::fromJson(i.value().toString().toUtf8()).toVariant());
            else if (i.key() == "library") {
                for (const auto &r : libraryRecords(i.value().toStringList()))
                    if (r.toMap().contains("sequences"))
                        out += clipEntries(r.toMap()["sequences"]);
            } else
                out += clipEntries(i.value());
        }
    }
    return out;
}
QVariantMap ProjectSafety::freezeNested(QVariantMap document) {
    auto nested = document.value("nestedDocuments").toMap();
    const auto found = RenderSnapshot::nestedDocuments(clipEntries(document));
    for (auto i = found.begin(); i != found.end(); ++i)
        if (!nested.contains(i.key()))
            nested[i.key()] = i.value();
    if (!nested.isEmpty())
        document["nestedDocuments"] = nested;
    return document;
}
QVariantMap ProjectSafety::rewriteDocument(const QVariantMap &document, const QVariantMap &paths) {
    return walk(document, [&](const QString &p) { return paths.value(p, p).toString(); }).toMap();
}
QSet<QString> ProjectSafety::assets(const QVariantMap &document) {
    QSet<QString> paths;
    walk(document, [&](const QString &p) {
        if (isFileReference(p))
            paths.insert(p);
        return p;
    });
    const auto nested = document.value("nestedDocuments").toMap();
    for (auto i = nested.begin(); i != nested.end(); ++i)
        paths.remove(i.key());
    return paths;
}
QVariantMap ProjectSafety::usedDocument(QVariantMap document, bool includeUnused) {
    if (includeUnused)
        return document;
    auto nested = document.value("nestedDocuments").toMap();
    for (auto i = nested.begin(); i != nested.end(); ++i)
        i.value() = usedDocument(i.value().toMap(), false);
    if (!nested.isEmpty()) document["nestedDocuments"] = nested;
    auto used = document;
    used.remove("mediaSources");
    QVariantList specials;
    for (const auto &r : libraryRecords(document.value("library").toStringList()))
        if (!r.toMap().contains("path"))
            specials.append(r);
    used["library"] = encodeLibrary(specials);
    auto needed = assets(used);
    for (auto i = nested.begin(); i != nested.end(); ++i) needed.insert(i.key());
    placedMulticams(used, &needed); // a used multicam clip keeps its item
    QVariantList records;
    for (const auto &r : libraryRecords(document.value("library").toStringList()))
        if (!r.toMap().contains("path") || needed.contains(r.toMap()["path"].toString()))
            records.append(r);
    document["library"] = encodeLibrary(records);
    auto sources = document.value("mediaSources").toMap();
    for (auto i = sources.begin(); i != sources.end();)
        if (!needed.contains(i.key()))
            i = sources.erase(i);
        else
            ++i;
    document["mediaSources"] = sources;
    return document;
}
QVariantMap ProjectSafety::resolveDocument(QVariantMap document, const QString &directory, QString *error) {
    document =
        walk(document, [&](const QString &p) {
            return isFileReference(p) && QDir::isRelativePath(p) ? QDir::cleanPath(QDir(directory).absoluteFilePath(p)) : p;
        }).toMap();
    const auto nested = document.value("nestedDocuments").toMap();
    if (nested.isEmpty())
        return document;
    const QString folder = StoragePreferences::cacheRoot() +
                           "/recovered-nests/" +
                           digest(QJsonDocument::fromVariant(nested).toJson(QJsonDocument::Compact));
    if (!QDir().mkpath(folder)) {
        *error = "Cannot prepare nested sequences.";
        return {};
    }
    QVariantMap paths;
    for (auto i = nested.begin(); i != nested.end(); ++i)
        paths[i.key()] = QDir(folder).filePath(digest(i.key().toUtf8()).left(16) + ProjectFile::extension());
    for (auto i = nested.begin(); i != nested.end(); ++i) {
        auto doc = rewriteDocument(i.value().toMap(), paths);
        doc.remove("nestedDocuments");
        if (!writeDocument(paths[i.key()].toString(), doc, error))
            return {};
    }
    document = rewriteDocument(document, paths);
    document.remove("nestedDocuments");
    return document;
}
QString ProjectSafety::backupDirectory(const QString &project) {
    return root() + "/backups/" + digest(QFileInfo(project).absoluteFilePath().toUtf8());
}
QString ProjectSafety::makeBackup(const QString &project, QString *error) {
    if (!QFileInfo::exists(project))
        return {};
    const auto dir = backupDirectory(project);
    if (!QDir().mkpath(dir)) {
        *error = "Cannot create the project backup folder.";
        return {};
    }
    const auto path = dir + '/' + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + '-' +
                      QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) + ProjectFile::extension();
    QFile source(project);
    QSaveFile target(path);
    if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) {
        *error = "Cannot preserve the previous saved version.";
        return {};
    }
    while (!source.atEnd()) {
        const auto chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() || target.write(chunk) != chunk.size()) {
            *error = "Could not finish the project backup.";
            return {};
        }
    }
    if (!target.commit()) {
        *error = target.errorString();
        return {};
    }
    return path;
}
void ProjectSafety::pruneBackups(const QString &project, int limit) {
    auto dir = QDir(backupDirectory(project));
    const auto files = dir.entryList(ProjectFile::openPatterns(), QDir::Files, QDir::Name);
    for (int i = 0; i < files.size() - qMax(1, limit); ++i)
        QFile::remove(dir.filePath(files[i]));
}
QVariantList ProjectSafety::backups(const QString &project) {
    QVariantList out;
    if (project.isEmpty())
        return out;
    auto dir = QDir(backupDirectory(project));
    for (const auto &f : dir.entryInfoList(ProjectFile::openPatterns(), QDir::Files, QDir::Name | QDir::Reversed))
        out.append(QVariantMap{{"path", f.absoluteFilePath()},
                               {"name", QFileInfo(project).completeBaseName()},
                               {"time", f.lastModified().toUTC().toString(Qt::ISODateWithMs)},
                               {"bytes", f.size()}});
    return out;
}

QVariantMap ProjectSafety::libraryLogging(const QVariantMap &entry) {
    QVariantMap logging;
    for (const char *key : {"libraryId", "binId", "duplicateRef", "size", "duration", "fps", "width",
                            "height", "hasVideo", "hasAudio", "created", "imported", "checksum", "cameraCard", "proxyPath", "proxy", "inputLut", "inputSpace", "rawExposure", "rawRed", "rawBlue", "preferProxy", "fpsNum", "fpsDen", "pixelAspect", "timecode", "timecodeFrames", "forceFps", "sourceFps", "sourceDuration", "startTimecodeFrames", "customMetadata", "stackId", "stackName"})
        if (entry.contains(QLatin1String(key))) logging.insert(QLatin1String(key), entry.value(QLatin1String(key)));
    const int rating = entry.value(QStringLiteral("rating")).toInt();
    if (rating > 0)
        logging.insert(QStringLiteral("rating"), rating);
    for (const char *flag : {"favorite", "rejected", "subclip"})
        if (entry.value(QLatin1String(flag)).toBool())
            logging.insert(QLatin1String(flag), true);
    for (const char *list : {"tags", "keywords"}) {
        const QStringList values = entry.value(QLatin1String(list)).toStringList();
        if (!values.isEmpty())
            logging.insert(QLatin1String(list), values);
    }
    for (const char *text : {"notes", "reel", "scene", "shot", "take", "angle", "comments"}) {
        const QString value = entry.value(QLatin1String(text)).toString();
        if (!value.isEmpty())
            logging.insert(QLatin1String(text), value);
    }
    if (entry.value(QStringLiteral("goodTake")).toBool())
        logging.insert(QStringLiteral("goodTake"), true);
    const QVariantList markers = entry.value(QStringLiteral("clipMarkers")).toList();
    if (!markers.isEmpty())
        logging.insert(QStringLiteral("clipMarkers"), markers);
    // The beats found in its music: an analysis worth keeping, like a transcript.
    const QVariantMap beatMap = entry.value(QStringLiteral("beatMap")).toMap();
    if (!beatMap.isEmpty())
        logging.insert(QStringLiteral("beatMap"), beatMap);
    const QVariantList transcript = entry.value(QStringLiteral("transcript")).toList();
    if (!transcript.isEmpty()) {
        logging.insert(QStringLiteral("transcript"), transcript);
        const QString language = entry.value(QStringLiteral("transcriptLanguage")).toString();
        if (!language.isEmpty())
            logging.insert(QStringLiteral("transcriptLanguage"), language);
    }
    // A renamed clip keeps its display name; the file's own name comes back
    // from the probe, so only a real alias is worth writing.
    const QString name = entry.value(QStringLiteral("name")).toString();
    if (!name.isEmpty() && name != QFileInfo(entry.value(QStringLiteral("path")).toString()).fileName())
        logging.insert(QStringLiteral("name"), name);
    return logging;
}

QString ProjectSafety::encodeMarkers(const QVariantList &markers) {
    if (markers.isEmpty()) return {};
    // frame[:type[:percent-encoded text]] — a bare frame is both the old
    // format and the encoding for an unlabelled standard marker.
    QStringList encoded;
    for (const QVariant &v : markers) {
        const QVariantMap m = v.toMap();
        const int frame = m.value(QStringLiteral("frame")).toInt();
        const QString type = m.value(QStringLiteral("type")).toString();
        const QString text = m.value(QStringLiteral("text")).toString();
        const QString notes = m.value(QStringLiteral("notes")).toString();
        const QString colour = m.value(QStringLiteral("colour")).toString();
        if (text.isEmpty() && notes.isEmpty() && colour.isEmpty()
            && (type.isEmpty() || type == QStringLiteral("standard")))
            encoded.append(QString::number(frame));
        else
            encoded.append(
                QString::number(frame) + QLatin1Char(':')
                + (type.isEmpty() ? QStringLiteral("standard") : type)
                + QLatin1Char(':')
                + QString::fromLatin1(QUrl::toPercentEncoding(text))
                // notes: a fourth field older builds simply ignore;
                // colour: a fifth (the notes field kept, empty or not)
                + (notes.isEmpty() && colour.isEmpty() ? QString()
                                   : QLatin1Char(':')
                                         + QString::fromLatin1(QUrl::toPercentEncoding(notes)))
                + (colour.isEmpty() ? QString() : QLatin1Char(':') + colour));
    }
    return QStringLiteral("::markers=") + encoded.join(QLatin1Char(','));
}

QStringList ProjectSafety::encodeLibraryState(const QVariantMap &state) {
    if (state.isEmpty()) return {};
    const auto library=state.value("library").toList();
    const auto markers=state.value("markers").toList();
    const auto sequences=state.value("sequences").toList();
    const int activeSequence=state.value("activeSequence").toInt();
    // One line per clip: the path, tab-separated from its marked range when
    // one exists. Range-less lines are plain paths, so old projects load.
    // Markers ride along as one "::markers=" line at the end — the load
    // parser strips it before importing, and old projects simply lack it.
    QStringList paths;
    for (const QVariant &v : std::as_const(library)) {
        const QVariantMap entry = v.toMap();
        QString line = ProjectSafety::encodeLibraryPath(entry.value(QStringLiteral("path")).toString());
        const int in = entry.value(QStringLiteral("rangeIn"), -1).toInt();
        const int out = entry.value(QStringLiteral("rangeOut"), -1).toInt();
        QVariantMap logging = libraryLogging(entry);
        if (entry.value(QStringLiteral("multicam")).toBool()) {
            // A multicam clip is its own line ("multicam:<id>"): the whole
            // description rides the meta field, so it never needs a probe.
            for (const char *key : {"id", "name", "angles", "syncMethod", "duration",
                                    "fps", "width", "height", "activeAngle"})
                logging.insert(QLatin1String(key), entry.value(QLatin1String(key)));
            // (`sound`, `soundAngle`: a clip with a sound source of its own;
            // `recordings`: the recordings lined up with it that are no angle.)
            for (const char *key : {"multicamVersion", "multicamFps", "sound", "soundAngle", "recordings"})
                if (entry.contains(QLatin1String(key)))
                    logging.insert(QLatin1String(key), entry.value(QLatin1String(key)));
            logging.insert(QStringLiteral("multicam"), true);
        }
        if (in >= 0 || out >= 0 || !logging.isEmpty())
            line += QLatin1Char('\t') + QString::number(in)
                    + QLatin1Char('\t') + QString::number(out);
        // Logging rides as a fourth field: percent-encoded compact JSON, so a
        // tab or newline in a note can never break the line format. Builds
        // that predate it read the first three fields and ignore the rest.
        if (!logging.isEmpty())
            line += QLatin1Char('\t') + QStringLiteral("meta=")
                    + QString::fromLatin1(QUrl::toPercentEncoding(
                          QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(logging))
                                                .toJson(QJsonDocument::Compact))));
        paths.append(line);
    }
    if (!markers.isEmpty())
        paths.append(encodeMarkers(markers));
    // Sequences: the active one is the XML itself; the others ride as
    // percent-encoded JSON snapshots (entries + markers + track flags +
    // playhead). Builds that predate it ignore the line and open the
    // active sequence alone.
    if (!sequences.isEmpty()) {
        QVariantList list;
        for (int i = 0; i < sequences.size(); ++i) {
            QVariantMap m = sequences.at(i).toMap();
            if (i == activeSequence)
                m.remove(QStringLiteral("snapshot")); // the XML carries it
            list.append(m);
        }
        QVariantMap doc;
        doc.insert(QStringLiteral("active"), activeSequence);
        doc.insert(QStringLiteral("list"), list);
        paths.append(QStringLiteral("::sequences=")
                     + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(
                         QJsonDocument(QJsonObject::fromVariantMap(doc)).toJson(QJsonDocument::Compact)))));
    }
    paths.append(QStringLiteral("::media=") + QString::fromLatin1(QUrl::toPercentEncoding(
        QString::fromUtf8(QJsonDocument::fromVariant(state.value("media")).toJson(QJsonDocument::Compact)))));
    return paths;
}
