#pragma once

#include <QImage>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// The Color page's stills gallery: a grabbed frame (FLOAT EXR when linear,
// otherwise PNG) with the grade it
// carried, filed under an album, starred, searchable, applied back to any
// clip. Lives in <project dir>/Stills (AppData/stills when the project is
// unsaved) with a JSON index beside the images. Thumbnails are served by
// the "thumbs" image provider under still:<id>.
//
// Item shape: {id, name, album, path, grade, clipName, frame, favorite,
// created, width, height}.
class StillsGallery : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(QStringList albums READ albums NOTIFY itemsChanged)
    Q_PROPERTY(QString directory READ directory NOTIFY directoryChanged)

public:
    explicit StillsGallery(QObject *parent = nullptr);

    QVariantList items() const { return m_items; }
    QStringList albums() const;
    QString directory() const { return m_dir; }
    // Point the gallery at a project (its Stills/ folder) or the default.
    void setDirectory(const QString &dir);

    // Grab: writes the image, a separate display thumbnail, and the index.
    QString grab(const QImage &frame, const QVariantMap &grade, const QString &name,
                 const QString &album, const QString &clipName, int frame_);
    Q_INVOKABLE QVariantMap item(const QString &id) const;
    Q_INVOKABLE QImage image(const QString &id) const;
    Q_INVOKABLE bool remove(const QString &id);
    Q_INVOKABLE bool rename(const QString &id, const QString &name);
    Q_INVOKABLE bool setAlbum(const QString &id, const QString &album);
    Q_INVOKABLE bool setFavorite(const QString &id, bool on);
    Q_INVOKABLE bool setGrade(const QString &id, const QVariantMap &grade);
    Q_INVOKABLE QString indexPath() const;
    bool exportStill(const QString &id, const QString &path, QString *error = nullptr) const;
    QString importStill(const QString &path, QString *error = nullptr);

signals:
    void failed(const QString &message);
    // Populate image providers before notifying QML about newly visible rows.
    void imagesReady();
    void itemsChanged();
    void directoryChanged();
    void grabbed(const QString &id);

private:
    void load();
    bool commitItems(const QVariantList &items);
    int indexOf(const QString &id) const;

    QString m_dir;
    QVariantList m_items;
};
