#pragma once
#include <QCache>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>
#include <QSet>
#include <QVariantMap>

// Pixels are bounded; source records let evicted library posters and gallery
// thumbnails regenerate on Qt's asynchronous image thread.
class ThumbnailProvider : public QQuickImageProvider {
public:
    explicit ThumbnailProvider(int memoryKiB=64*1024);
    QImage requestImage(const QString &id,QSize *size,const QSize &requestedSize) override;
    void insert(const QString &key,const QImage &image);
    // Exact revisions keep asynchronous QML image requests in one scope batch.
    // Three fixed-size batches (about 2.3 MiB) are retained independently of thumbnails.
    void insertScopes(int revision,const QList<QImage> &images);
    void clearScopes();
    void registerSource(const QString &key,const QString &path,int frame=-1,int width=320,
                        const QVariantMap &inputTransform={},const QVariantMap &workingManagement={});
    void remove(const QString &key);
    void retainStrips(const QSet<QString> &keys);
    qint64 memoryBytes() const;
private:
    struct Source {
        QString path;
        int frame=-1,width=320;
        QVariantMap inputTransform,workingManagement;
    };
    mutable QMutex m_mutex;
    QCache<QString,QImage> m_images;
    QCache<QString,Source> m_sources{10000};
    struct ScopeBatch { int revision; QList<QImage> images; };
    QList<ScopeBatch> m_scopeBatches;
};
