#pragma once

#include <QImage>
#include <QVariantMap>
#include "imagesurface.h"
#include <QPointer>
#include <QTimer>

class Backend;
class ViewerProcessor;

// The viewer surface: paints whatever frame the backend last decoded,
// aspect-fit on the chosen canvas colour, with black inside the picture's
// own frame so its edges always show. `channel` picks which monitor this
// surface is: Auto follows the active view (single mode-switching viewer),
// Program always shows the timeline, Source always shows the previewed clip,
// Before shows the comparison view's frame with every attribute off.
class QPainter;
class VideoView : public SuiteQuick::ImageSurface {
    Q_OBJECT

public:
    enum Channel { Auto, Program, Source, Before, TwoUpOutgoing, TwoUpIncoming, ColorMatte, Still, EffectPreview };
    Q_ENUM(Channel)

private:
    Q_PROPERTY(QObject *source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(Channel channel READ channel WRITE setChannel NOTIFY channelChanged)
    Q_PROPERTY(QString stillId READ stillId WRITE setStillId NOTIFY stillIdChanged)
    Q_PROPERTY(QVariantMap colorOptions READ colorOptions WRITE setColorOptions NOTIFY colorOptionsChanged)
    Q_PROPERTY(QRectF contentRect READ contentRect NOTIFY viewGeometryChanged)
    Q_PROPERTY(bool viewTransformEnabled READ viewTransformEnabled WRITE setViewTransformEnabled NOTIFY viewGeometryChanged)
    Q_PROPERTY(QVariantMap overlayPalette MEMBER m_overlayPalette NOTIFY viewGeometryChanged)
    Q_PROPERTY(bool overlaysEnabled MEMBER m_overlaysEnabled NOTIFY viewGeometryChanged)
    Q_PROPERTY(bool processing READ processing NOTIFY processingChanged)
    Q_PROPERTY(QString colorError READ colorError NOTIFY colorErrorChanged)

public:
    explicit VideoView(QQuickItem *parent = nullptr);
    ~VideoView() override;
    bool processing() const { return m_processing; }
    bool hasPicture() const { return !m_frame.isNull(); } // something is on screen
    QSize shownSize() const { return m_frame.size(); }    // the pixels it is made of

    // CPU reference for pixel tests; normal display uses the inherited image nodes.
    void paint(QPainter *painter);
    QRectF contentRect() const;
    QColor canvasColor() const;
    bool viewTransformEnabled() const { return m_viewTransformEnabled; }
    void setViewTransformEnabled(bool on) { if(m_viewTransformEnabled!=on) { m_viewTransformEnabled=on; emit viewGeometryChanged(); update(); } }

    QObject *source() const;
    void setSource(QObject *source);
    Channel channel() const { return m_channel; }
    void setChannel(Channel channel);
    QString stillId() const { return m_stillId; }
    void setStillId(const QString &id);
    QVariantMap colorOptions() const { return m_colorOptions; }
    void setColorOptions(const QVariantMap &options);
    QString colorError() const { return m_colorError; }

signals:
    void viewGeometryChanged();
    void processingChanged();
    void framePrepared();
    void stillIdChanged();
    void sourceChanged();
    void channelChanged();
    void colorOptionsChanged();
    void colorErrorChanged();

protected:
    void itemChange(ItemChange change, const ItemChangeData &data) override;

private:
    QString m_colorError;
    void refresh();
    void invalidate();
    void setProcessing(bool value);
    void syncSurface();
    void paintChecker(QPainter *painter) const;
    void paintGuides(QPainter *painter) const;
    QByteArray m_layersKey;

    QPointer<Backend> m_backend;
    QPointer<ViewerProcessor> m_processor;
    QTimer m_retry;
    quint64 m_client=0,m_epoch=0,m_request=0,m_accepted=0,m_stillRevision=0;
    bool m_processing=false;
    QByteArray m_requestKey,m_contextKey;
    Channel m_channel = Auto;
    QString m_stillId;
    QImage m_frame;
    QSize m_nativeSize;
    double m_pixelAspect = 1.;
    bool m_overlaysEnabled = true;
    bool m_viewTransformEnabled = true, m_acknowledgeFrame = false;
    quint64 m_serial = 0;
    QVariantMap m_overlayPalette;
    QVariantMap m_viewSettings;
    QVariantMap m_colorOptions;
};
