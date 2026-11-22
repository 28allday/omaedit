#include "thumbnailprovider.h"
#include "thumbnaildiskcache.h"
#include "workingframe.h"
#include "mediaplayer.h"
#include <QImageReader>
#include <QJsonDocument>

ThumbnailProvider::ThumbnailProvider(int memoryKiB):QQuickImageProvider(Image,ForceAsynchronousImageLoading),m_images(qMax(1,memoryKiB)){}
void ThumbnailProvider::insertScopes(int revision,const QList<QImage> &images){
    if(revision<=0 || images.size()!=4)return;
    for(int i=0;i<4;++i)if(images[i].size()!=(i==3?QSize(200,200):QSize(256,200)) || images[i].format()!=QImage::Format_ARGB32_Premultiplied)return;
    QMutexLocker lock(&m_mutex);
    m_scopeBatches.prepend({revision,images});
    while(m_scopeBatches.size()>3)m_scopeBatches.removeLast();
}
void ThumbnailProvider::clearScopes(){QMutexLocker lock(&m_mutex);m_scopeBatches.clear();}
void ThumbnailProvider::insert(const QString &key,const QImage &image){
    QImage preview=image;
    if(WorkingFrame::isTagged(preview))preview=WorkingFrame::fromTagged(preview).display({}).convertToFormat(QImage::Format_RGBA8888);
    QMutexLocker lock(&m_mutex);
    m_images.insert(key,new QImage(preview),qMax(1,int((preview.sizeInBytes()+1023)/1024)));
}
void ThumbnailProvider::registerSource(const QString &key,const QString &path,int frame,int width,
                                       const QVariantMap &inputTransform,const QVariantMap &workingManagement){
    width=qBound(1,width,1920);
    QMutexLocker lock(&m_mutex);const auto *old=m_sources.object(key);
    if(old && old->path==path && old->frame==frame && old->width==width &&
       old->inputTransform==inputTransform && old->workingManagement==workingManagement)return;
    m_images.remove(key);m_sources.insert(key,new Source{path,frame,width,inputTransform,workingManagement});
}
QImage ThumbnailProvider::requestImage(const QString &id,QSize *size,const QSize &requestedSize){
    Q_UNUSED(requestedSize);
    if(id.startsWith("scope:")) {
        const QStringList kinds{"waveform","parade","histogram","vectorscope"};
        const int index=kinds.indexOf(id.section('?',0,0).mid(6));
        bool valid=false;const int revision=id.section('?',1).toInt(&valid);
        QImage image;
        {QMutexLocker lock(&m_mutex);
            for(const auto &batch:m_scopeBatches)if(index>=0 && (!id.contains('?') || (valid && batch.revision==revision))){image=batch.images[index];break;}
        }
        if(size)*size=image.size();return image; // Never fetch stale scopes from disk.
    }
    const auto key=id.section('?',0,0);QImage image;Source source;
    {QMutexLocker lock(&m_mutex);if(const auto *cached=m_images.object(key))image=*cached;if(const auto *record=m_sources.object(key))source=*record;}
    const QString diskKey=source.path.isEmpty()?key:key+'|'+QString::fromUtf8(QJsonDocument::fromVariant(
        QVariantList{source.path,source.frame,source.width,source.inputTransform,source.workingManagement}).toJson(QJsonDocument::Compact));
    if(image.isNull())image=ThumbnailDiskCache::read(diskKey);
    if(image.isNull() && !source.path.isEmpty()){
        if(source.frame>=0){
            MediaPlayer player;
            if(player.open(source.path)){
                player.setInputTransform(source.inputTransform);
                player.setWorkingManagement(source.workingManagement);
                image=player.frameAt(source.frame);
            }
        }
        else if(source.path.endsWith(".exr",Qt::CaseInsensitive))image=WorkingFrame::load(source.path).display({});
        else {QImageReader reader(source.path);if(reader.size().isValid())reader.setScaledSize(reader.size().scaled(source.width,source.width,Qt::KeepAspectRatio));image=reader.read();}
        if(!image.isNull()){image=image.scaledToWidth(source.width,Qt::SmoothTransformation);ThumbnailDiskCache::write(diskKey,image);}
    }
    if(WorkingFrame::isTagged(image))image=WorkingFrame::fromTagged(image).display({}).convertToFormat(QImage::Format_RGBA8888);
    if(!image.isNull())insert(key,image);
    if(size)*size=image.size();return image;
}
void ThumbnailProvider::remove(const QString &key){QMutexLocker lock(&m_mutex);m_images.remove(key);m_sources.remove(key);}
void ThumbnailProvider::retainStrips(const QSet<QString> &keys){QMutexLocker lock(&m_mutex);for(const auto &key:m_images.keys())if(key.startsWith("strip-")&&!keys.contains(key))m_images.remove(key);}
qint64 ThumbnailProvider::memoryBytes()const{
    QMutexLocker lock(&m_mutex);qint64 bytes=qint64(m_images.totalCost())*1024;
    for(const auto &batch:m_scopeBatches)for(const auto &image:batch.images)bytes+=image.sizeInBytes();
    return bytes;
}
