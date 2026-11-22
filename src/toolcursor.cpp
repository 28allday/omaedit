#include "toolcursor.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCursor>
#include <QImage>
#include <QImageReader>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QSGTexture>

// The drawn pointer: one small picture on top of the window's content.
class ToolCursorGlyph : public QQuickItem {
public:
    explicit ToolCursorGlyph(QQuickItem *parent) : QQuickItem(parent) {
        setFlag(ItemHasContents);
        setAcceptedMouseButtons(Qt::NoButton);
        setAcceptHoverEvents(false);
        setEnabled(false);
        setZ(1e9);
        setVisible(false);
    }
    void setImage(const QImage &image) {
        m_image = image;
        m_dirty = true;
        update();
    }
protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override {
        auto *node = static_cast<QSGImageNode *>(old);
        if (m_image.isNull()) {
            delete node;
            return nullptr;
        }
        if (!node) {
            node = window()->createImageNode();
            node->setOwnsTexture(true);
            node->setFiltering(QSGTexture::Linear);
            m_dirty = true;
        }
        if (m_dirty) {
            node->setTexture(window()->createTextureFromImage(m_image));
            m_dirty = false;
        }
        node->setRect(boundingRect());
        return node;
    }
private:
    QImage m_image;
    bool m_dirty = true;
};

static void registerToolCursor() { qmlRegisterType<ToolCursor>("omaedit", 1, 0, "ToolCursor"); }
Q_COREAPP_STARTUP_FUNCTION(registerToolCursor)

ToolCursor::ToolCursor(QQuickItem *parent) : QQuickItem(parent) {
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
}

// The glyph belongs to the window's content: it goes later, or with the
// window, never while the window is walking its items.
ToolCursor::~ToolCursor() {
    if (m_window)
        m_window->removeEventFilter(this);
    if (m_glyph)
        m_glyph->deleteLater();
}

void ToolCursor::setShape(const QString &shape) {
    if (shape == m_shape) return;
    m_shape = shape;
    rebuild();
    emit shapeChanged();
}

void ToolCursor::setHotX(qreal x) {
    if (qFuzzyCompare(x, m_hotX)) return;
    m_hotX = x;
    rebuild();
    emit shapeChanged();
}

void ToolCursor::setHotY(qreal y) {
    if (qFuzzyCompare(y, m_hotY)) return;
    m_hotY = y;
    rebuild();
    emit shapeChanged();
}

void ToolCursor::setSize(int size) {
    if (size == m_size || size <= 0) return;
    m_size = size;
    rebuild();
    emit shapeChanged();
}

void ToolCursor::itemChange(ItemChange change, const ItemChangeData &value) {
    QQuickItem::itemChange(change, value);
    if (change == ItemSceneChange)
        watch(value.window);
    else if (change == ItemDevicePixelRatioHasChanged)
        rebuild();
    else if (change == ItemVisibleHasChanged) {
        if (!value.boolValue && m_glyph)
            m_glyph->setVisible(false);
        polish();
    }
}

void ToolCursor::watch(QQuickWindow *window) {
    if (m_window == window)
        return;
    if (m_window)
        m_window->removeEventFilter(this);
    if (m_glyph) {
        m_glyph->setVisible(false);
        m_glyph->deleteLater();
    }
    m_window = window;
    m_inWindow = false;
    if (m_window) {
        m_window->installEventFilter(this);
        m_glyph = new ToolCursorGlyph(m_window->contentItem());
    }
    rebuild();
}

// Every pointer move reaches the window first, drags included, so the
// drawn pointer follows wherever the press went.
bool ToolCursor::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::MouseMove:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::Enter:
        m_scenePos = static_cast<QSinglePointEvent *>(event)->scenePosition();
        m_inWindow = true;
        polish();
        break;
    case QEvent::Wheel: // the view may scroll under a still pointer
        polish();
        break;
    case QEvent::Leave:
        m_inWindow = false;
        polish();
        break;
    default:
        break;
    }
    return QQuickItem::eventFilter(watched, event);
}

// After the window has chosen its cursor for this move: the glyph shows
// only where the window shows this item's blank cursor, so a menu or a
// handle with its own pointer above the item keeps its pointer.
void ToolCursor::updatePolish() {
    const bool show = m_window && m_glyph && m_inWindow && isVisible() && !m_shape.isEmpty()
                      && contains(mapFromScene(m_scenePos))
                      && m_window->cursor().shape() == Qt::BlankCursor;
    if (m_glyph) {
        if (show) {
            const qreal dpr = m_window->effectiveDevicePixelRatio();
            const QPointF corner = m_scenePos - QPointF(m_hotX * m_size, m_hotY * m_size);
            // On whole device pixels, so the picture is shown pixel for pixel.
            m_glyph->setPosition(QPointF(qRound(corner.x() * dpr) / dpr, qRound(corner.y() * dpr) / dpr));
            m_glyph->setSize(QSizeF(m_size, m_size));
        }
        m_glyph->setVisible(show);
    }
    if (show != m_shown || show) {
        m_shown = show;
        emit pointerChanged();
    }
}

QImage ToolCursor::render(const QString &shape, int size, qreal dpr) {
    const int pixels = qMax(1, qRound(size * dpr));
    auto layer = [&](const char *stroke, qreal width) {
        const QByteArray svg = QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-2 -2 28 28\" fill=\"none\" stroke=\"")
                               + stroke + "\" stroke-width=\"" + QByteArray::number(width)
                               + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\">" + shape.toUtf8() + "</svg>";
        // Drawn at the pixels it will show at: enlarging a small image blurs
        // thin strokes apart on a scaled display.
        QBuffer source;
        source.setData(svg);
        source.open(QIODevice::ReadOnly);
        QImageReader reader(&source, "svg");
        reader.setScaledSize(QSize(pixels, pixels));
        return reader.read();
    };
    const QImage outline = layer("#000000", 4.75), fill = layer("#ffffff", 2.25);
    QImage out(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    if (!outline.isNull() && !fill.isNull()) {
        QPainter painter(&out);
        painter.drawImage(0, 0, outline);
        painter.drawImage(0, 0, fill);
    }
    out.setDevicePixelRatio(dpr);
    return out;
}

void ToolCursor::rebuild() {
    if (m_shape.isEmpty()) {
        unsetCursor();
        if (m_glyph)
            m_glyph->setImage(QImage());
        polish();
        return;
    }
    // A blank cursor hides the system pointer over the item; the glyph
    // stands in for it.
    setCursor(Qt::BlankCursor);
    if (m_glyph)
        m_glyph->setImage(render(m_shape, m_size, m_window ? m_window->effectiveDevicePixelRatio() : 1.0));
    polish();
}
