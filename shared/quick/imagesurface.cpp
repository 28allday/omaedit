#include "imagesurface.h"
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGRectangleNode>
#include <QSGTransformNode>
#include <QCoreApplication>
#include <QPointer>
#include <QRunnable>
#include <functional>
namespace SuiteQuick {
namespace {
// An identity transform retains inherited clip/opacity/matrix state when late
// worker results insert image children into an already-rendered software scene.
class SurfaceNode : public QSGTransformNode {
public:
    QSGRectangleNode *surround=nullptr,*frame=nullptr;
    QSGImageNode *images[3]{};
    qint64 keys[3]{};
    quint64 presentation=0;
    explicit SurfaceNode(QQuickWindow *window) {
        surround=window->createRectangleNode();
        if(surround){surround->setColor(Qt::black);appendChildNode(surround);}
        frame=window->createRectangleNode();
        if(frame){frame->setColor(Qt::transparent);appendChildNode(frame);}
    }
    bool set(int index,const QImage &image,const QRectF &rect,QQuickWindow *window) {
        auto *node=images[index];
        if(image.isNull()) {
            if(node){removeChildNode(node);delete node;images[index]=nullptr;}
            keys[index]=0;return true;
        }
        if(!node || keys[index]!=image.cacheKey()) {
            auto *texture=window->createTextureFromImage(image);
            if(!texture){if(node)node->setRect({});return false;}
            // Replace the owning node as a unit: backend implementations differ
            // in how setTexture handles a previously owned texture.
            if(node){removeChildNode(node);delete node;}
            node=window->createImageNode();images[index]=node;
            if(!node){delete texture;return false;}
            node->setTexture(texture);node->setOwnsTexture(true);node->setFiltering(QSGTexture::Linear);
            QSGNode *previous=frame?static_cast<QSGNode*>(frame):surround;for(int i=0;i<index;++i)if(images[i])previous=images[i];
            if(previous)insertChildNodeAfter(node,previous);else prependChildNode(node);
            keys[index]=image.cacheKey();
        }
        node->setSourceRect(QRectF(QPointF(),image.size()));node->setRect(rect);return true;
    }
};
class Rendered : public QRunnable {
    QPointer<ImageSurface> m_item;quint64 m_serial;
public:
    Rendered(ImageSurface *item,quint64 serial):m_item(item),m_serial(serial){}
    void run() override {
        const auto item=m_item;const auto serial=m_serial;
        // Dereference the guarded GUI object only on its own thread.
        QMetaObject::invokeMethod(QCoreApplication::instance(),[item,serial]{if(item)emit item->imageRendered(serial);},Qt::QueuedConnection);
    }
};
}
ImageSurface::ImageSurface(QQuickItem *parent):QQuickItem(parent){setFlag(ItemHasContents);setClip(true);}
void ImageSurface::setImage(const QImage &image,quint64 presentation){if(m_image.cacheKey()==image.cacheKey() && m_presentation==presentation)return;m_image=image;m_presentation=presentation;update();}
void ImageSurface::setImageRect(const QRectF &rect){if(m_rect==rect)return;m_rect=rect;update();}
void ImageSurface::setBackdrop(const QColor &surround,const QColor &frame){if(m_surround==surround && m_frame==frame)return;m_surround=surround;m_frame=frame;update();}
void ImageSurface::setLayers(const QImage &background,const QImage &foreground){m_background=background;m_foreground=foreground;update();}
QSGNode *ImageSurface::updatePaintNode(QSGNode *old,UpdatePaintNodeData *) {
    if(!window()){delete old;return nullptr;}
    auto *node=static_cast<SurfaceNode*>(old);if(!node)node=new SurfaceNode(window());
    if(node->surround){node->surround->setRect(boundingRect());if(node->surround->color()!=m_surround)node->surround->setColor(m_surround);}
    if(node->frame){
        const QRectF frame=m_image.isNull() || m_frame.alpha()==0 ? QRectF() : m_rect;
        node->frame->setRect(frame);if(node->frame->color()!=m_frame)node->frame->setColor(m_frame);
    }
    node->set(0,m_background,boundingRect(),window());
    const bool changed=node->keys[1]!=m_image.cacheKey();
    const bool ready=node->set(1,m_image,m_rect,window());
    if(ready && changed && !m_image.isNull())++m_uploads;
    node->set(2,m_foreground,boundingRect(),window());
    if(ready && !m_image.isNull() && m_presentation && node->presentation!=m_presentation) {
        node->presentation=m_presentation;
        window()->scheduleRenderJob(new Rendered(this,m_presentation),QQuickWindow::AfterRenderingStage);
    }
    return node;
}
}
