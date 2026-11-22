#include "stills.h"
#include "gradeengine.h"
#include "workingframe.h"
#include "storagepreferences.h"
#include <QImageReader>
#include <QSaveFile>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <QStandardPaths>

StillsGallery::StillsGallery(QObject *parent) : QObject(parent) {
    setDirectory(QString());
}

void StillsGallery::setDirectory(const QString &dir) {
    QString target = dir;
    if (target.isEmpty()) {
        target = StoragePreferences::galleryRoot();
    }
    if (target == m_dir && !m_items.isEmpty())
        return;
    QDir().mkpath(target);
    m_dir = std::move(target);
    emit directoryChanged();
    load();
}

QString StillsGallery::indexPath() const { return m_dir + QStringLiteral("/stills.json"); }

QStringList StillsGallery::albums() const {
    QStringList out;
    for (const QVariant &v : m_items) {
        const QString a = v.toMap().value(QStringLiteral("album")).toString();
        if (!a.isEmpty() && !out.contains(a))
            out << a;
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

// The index names files inside the gallery folder only. It travels with a
// project folder, so a name with a path in it must not lead removal elsewhere.
static bool galleryName(const QString &name) {
    return !name.isEmpty() && name != QLatin1String(".") && name != QLatin1String("..")
           && !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\'));
}

void StillsGallery::load() {
    m_items.clear();
    QFile f(indexPath());
    if (f.open(QIODevice::ReadOnly)) {
        for (const auto &v : QJsonDocument::fromJson(f.readAll()).array()) {
            QVariantMap m = v.toObject().toVariantMap();
            if (m.contains("grade")) m["grade"] = GradeEngine::pinMaskModels(m.value("grade").toMap());
            // Paths are stored relative so a project folder can move.
            const QString file = m.value(QStringLiteral("file")).toString();
            const QString thumbnail = m.value(QStringLiteral("thumbnail")).toString();
            if (!galleryName(file) || (!thumbnail.isEmpty() && !galleryName(thumbnail)))
                continue;
            m.insert(QStringLiteral("path"), m_dir + QLatin1Char('/') + file);
            if (QFileInfo::exists(m.value(QStringLiteral("path")).toString()))
                m_items.append(m);
        }
    }
    emit imagesReady();
    emit itemsChanged();
}

bool StillsGallery::commitItems(const QVariantList &items) {
    QJsonArray arr;
    for (const QVariant &v : items) {
        QVariantMap m = v.toMap();
        m.remove(QStringLiteral("path"));
        arr.append(QJsonObject::fromVariantMap(m));
    }
    QSaveFile f(indexPath());
    const auto bytes = QJsonDocument(arr).toJson(QJsonDocument::Compact);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit()) {
        emit failed(tr("Could not save the stills gallery: %1").arg(f.errorString()));
        return false;
    }
    m_items = items;
    return true;
}

int StillsGallery::indexOf(const QString &id) const {
    for (int i = 0; i < m_items.size(); ++i)
        if (m_items.at(i).toMap().value(QStringLiteral("id")).toString() == id)
            return i;
    return -1;
}

QString StillsGallery::grab(const QImage &frame, const QVariantMap &grade, const QString &name,
                            const QString &album, const QString &clipName, int frame_) {
    if (frame.isNull())
        return QString();
    QString id;
    do {
        id = "s" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    } while (indexOf(id) >= 0 || QFileInfo::exists(m_dir+'/'+id+".png") ||
             QFileInfo::exists(m_dir+'/'+id+".exr") || QFileInfo::exists(m_dir+'/'+id+"-display.png"));
    const bool linear=WorkingFrame::isTagged(frame);
    const QString file = id + (linear ? QStringLiteral(".exr") : QStringLiteral(".png"));
    if(linear) {
        const auto working=WorkingFrame::fromTagged(frame);
        if(working.isNull() || !working.save(m_dir+'/'+file,{{"grade",grade},{"name",name},{"album",album}}))return {};
        const auto thumb=working.display({}).scaledToWidth(320,Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888);
        if(!thumb.save(m_dir+'/'+id+"-display.png")){QFile::remove(m_dir+'/'+file);return {};}
    } else if (!frame.save(m_dir + QLatin1Char('/') + file)) return {};
    QVariantMap m;
    m.insert(QStringLiteral("id"), id);
    m.insert(QStringLiteral("file"), file);
    if(linear)m.insert("thumbnail",id+"-display.png");
    m.insert(QStringLiteral("path"), m_dir + QLatin1Char('/') + file);
    m.insert(QStringLiteral("name"), name.isEmpty() ? QStringLiteral("Still %1").arg(m_items.size() + 1) : name);
    m.insert(QStringLiteral("album"), album);
    m.insert(QStringLiteral("grade"), grade);
    m.insert(QStringLiteral("clipName"), clipName);
    m.insert(QStringLiteral("frame"), frame_);
    m.insert(QStringLiteral("favorite"), false);
    m.insert(QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate));
    m.insert(QStringLiteral("width"), frame.width());
    m.insert(QStringLiteral("height"), frame.height());
    auto items = m_items;
    items.prepend(m);
    if (!commitItems(items)) {
        QFile::remove(m_dir+'/'+file);
        if (linear) QFile::remove(m_dir+'/'+id+"-display.png");
        return {};
    }
    emit imagesReady();
    emit itemsChanged();
    emit grabbed(id);
    return id;
}

QVariantMap StillsGallery::item(const QString &id) const {
    const int i = indexOf(id);
    return i < 0 ? QVariantMap() : m_items.at(i).toMap();
}

QImage StillsGallery::image(const QString &id) const {
    const int i = indexOf(id);
    if(i<0)return {};
    const auto path=m_items.at(i).toMap().value("path").toString();
    if(path.endsWith(".exr",Qt::CaseInsensitive))return WorkingFrame::load(path).taggedImage();
    return QImage(path);
}

bool StillsGallery::remove(const QString &id) {
    const int i = indexOf(id);
    if (i < 0)
        return false;
    const auto removed = m_items.at(i).toMap();
    auto items = m_items;
    items.removeAt(i);
    if (!commitItems(items)) return false;
    // Older galleries may contain duplicate IDs/paths. Commit the index first
    // and never delete an image still used by another row.
    const auto path = removed.value("path").toString();
    const auto thumbnail = removed.value("thumbnail").toString();
    bool sharedImage = false, sharedThumbnail = false;
    for (const auto &item : m_items) {
        sharedImage |= item.toMap().value("path").toString() == path;
        sharedThumbnail |= item.toMap().value("thumbnail").toString() == thumbnail;
    }
    if (!sharedImage && galleryName(removed.value("file").toString())) QFile::remove(path);
    if (!thumbnail.isEmpty() && !sharedThumbnail && galleryName(thumbnail)) QFile::remove(m_dir+'/'+thumbnail);
    emit itemsChanged();
    return true;
}

bool StillsGallery::rename(const QString &id, const QString &name) {
    const int i = indexOf(id);
    if (i < 0 || name.trimmed().isEmpty())
        return false;
    QVariantMap m = m_items.at(i).toMap();
    m.insert(QStringLiteral("name"), name.trimmed());
    auto items = m_items;
    items[i] = m;
    if (!commitItems(items)) return false;
    emit itemsChanged();
    return true;
}

bool StillsGallery::setAlbum(const QString &id, const QString &album) {
    const int i = indexOf(id);
    if (i < 0)
        return false;
    QVariantMap m = m_items.at(i).toMap();
    m.insert(QStringLiteral("album"), album.trimmed());
    auto items = m_items;
    items[i] = m;
    if (!commitItems(items)) return false;
    emit itemsChanged();
    return true;
}

bool StillsGallery::setFavorite(const QString &id, bool on) {
    const int i = indexOf(id);
    if (i < 0)
        return false;
    QVariantMap m = m_items.at(i).toMap();
    m.insert(QStringLiteral("favorite"), on);
    auto items = m_items;
    items[i] = m;
    if (!commitItems(items)) return false;
    emit itemsChanged();
    return true;
}

bool StillsGallery::setGrade(const QString &id, const QVariantMap &grade) {
    const int i = indexOf(id);
    if (i < 0)
        return false;
    QVariantMap m = m_items.at(i).toMap();
    m.insert(QStringLiteral("grade"), grade);
    auto items = m_items;
    items[i] = m;
    if (!commitItems(items)) return false;
    emit itemsChanged();
    return true;
}

bool StillsGallery::exportStill(const QString &id, const QString &path, QString *error) const {
    if (error)
        error->clear();
    const QVariantMap still = item(id);
    QImage picture = image(id);
    if (still.isEmpty() || picture.isNull()) {
        if (error)
            *error = tr("Select a still to export.");
        return false;
    }
    if(WorkingFrame::isTagged(picture)) {
        const auto working=WorkingFrame::fromTagged(picture,error);
        if(working.isNull())return false;
        if(path.endsWith(".exr",Qt::CaseInsensitive))return working.save(path,{{"grade",still.value("grade")},{"name",still.value("name")},{"album",still.value("album")}},error);
        // An explicitly requested PNG is an sRGB output, independent of view.
        picture=working.output("srgb/srgb",error).convertToFormat(QImage::Format_RGBA64);
        if(picture.isNull())return false;
    }
    picture.setText(QStringLiteral("omaedit.grade.v1"),
                    QString::fromUtf8(QJsonDocument::fromVariant(still.value("grade")).toJson(QJsonDocument::Compact)));
    picture.setText(QStringLiteral("omaedit.name"), still.value("name").toString());
    picture.setText(QStringLiteral("omaedit.album"), still.value("album").toString());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !picture.save(&file, "PNG") || !file.commit()) {
        if (error)
            *error = tr("Could not write the still and grade metadata.");
        return false;
    }
    return true;
}
QString StillsGallery::importStill(const QString &path, QString *error) {
    if (error)
        error->clear();
    if(path.endsWith(".exr",Qt::CaseInsensitive)) {
        QVariantMap meta;const auto working=WorkingFrame::load(path,&meta,error);
        if(working.isNull())return {};
        const auto grade=meta.value("grade").toMap();const auto problem=GradeEngine::validate(grade);
        if(!problem.isEmpty() || grade.isEmpty()){if(error)*error=problem.isEmpty()?tr("This EXR does not contain an OmaEdit grade."):problem;return {};}
        const auto id=grab(working.taggedImage(),GradeEngine::freshCopy(grade),meta.value("name").toString(),meta.value("album").toString(),{},0);
        if(id.isEmpty() && error)*error=tr("Could not store the imported linear still.");
        return id;
    }
    QImageReader reader(path);
    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > 32000000 ||
        QFileInfo(path).size() > 128 * 1024 * 1024) {
        if (error)
            *error = tr("Choose a readable still up to 32 megapixels and 128 MiB.");
        return {};
    }
    const QImage picture = reader.read();
    const QString metadata = picture.text(QStringLiteral("omaedit.grade.v1"));
    if (picture.isNull() || metadata.isEmpty() || metadata.size() > 4 * 1024 * 1024) {
        if (error)
            *error = tr("This image does not contain an OmaEdit grade.");
        return {};
    }
    QJsonParseError parse;
    const QVariantMap grade = QJsonDocument::fromJson(metadata.toUtf8(), &parse).toVariant().toMap();
    const QString problem = GradeEngine::validate(grade);
    if (parse.error != QJsonParseError::NoError || !problem.isEmpty()) {
        if (error)
            *error = problem.isEmpty() ? tr("Invalid grade metadata.") : problem;
        return {};
    }
    const QString name = picture.text(QStringLiteral("omaedit.name"));
    const QString id =
        grab(picture, GradeEngine::freshCopy(grade), name.isEmpty() ? QFileInfo(path).completeBaseName() : name,
             picture.text(QStringLiteral("omaedit.album")), QString(), 0);
    if (id.isEmpty() && error)
        *error = tr("Could not store the imported still.");
    return id;
}
