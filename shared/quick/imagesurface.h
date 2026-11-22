#pragma once
#include <QQuickItem>
#include <QColor>
#include <QImage>
#include <atomic>
namespace SuiteQuick {
// App-local Qt Quick image surface. Texture ownership stays on the render thread.
class ImageSurface : public QQuickItem {
    Q_OBJECT
public:
    explicit ImageSurface(QQuickItem *parent=nullptr);
    void setImage(const QImage &image, quint64 presentation=0);
    void setImageRect(const QRectF &rect);
    void setLayers(const QImage &background, const QImage &foreground);
    // `surround` fills the whole item; `frame` fills the image rect beneath
    // the image (transparent for none), so see-through pixels never show
    // the surround and the picture's edges stay visible.
    void setBackdrop(const QColor &surround, const QColor &frame);
    quint64 imageUploads() const { return m_uploads.load(); }
signals:
    void imageRendered(quint64 presentation);
protected:
    QSGNode *updatePaintNode(QSGNode *, UpdatePaintNodeData *) override;
private:
    QImage m_image,m_background,m_foreground;
    QRectF m_rect;
    QColor m_surround=Qt::black,m_frame=Qt::transparent;
    quint64 m_presentation=0;
    std::atomic<quint64> m_uploads{0};
};
}
