#pragma once
#include <QImage>
#include <QPointer>
#include <QQuickItem>

class ToolCursorGlyph;

// A pointer drawn from a Lucide icon, for the tools Qt has no stock cursor
// for (blade, slip, slide, trim, track select). The item takes no input of
// its own: clicks and drags reach the timeline underneath. `shape` is the
// icon's path data (Icons.shapes[name]); the hot spot is a fraction of the
// cursor's size.
//
// The app draws the pointer itself rather than handing the picture to the
// compositor: some compositor and driver pairs scramble a client cursor
// picture. Over the item the system pointer is hidden (a blank cursor, which
// every compositor shows as nothing) and a glyph follows the mouse on top of
// the window, drawn at the window's own pixel ratio so it stays sharp.
class ToolCursor : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QString shape READ shape WRITE setShape NOTIFY shapeChanged)
    Q_PROPERTY(qreal hotX READ hotX WRITE setHotX NOTIFY shapeChanged)
    Q_PROPERTY(qreal hotY READ hotY WRITE setHotY NOTIFY shapeChanged)
    Q_PROPERTY(int size READ size WRITE setSize NOTIFY shapeChanged)
    // True while the drawn pointer shows; where its hot spot is, in the scene.
    Q_PROPERTY(bool pointerShown READ pointerShown NOTIFY pointerChanged)
    Q_PROPERTY(QPointF pointerScenePosition READ pointerScenePosition NOTIFY pointerChanged)
public:
    explicit ToolCursor(QQuickItem *parent = nullptr);
    ~ToolCursor() override;
    QString shape() const { return m_shape; }
    void setShape(const QString &shape);
    qreal hotX() const { return m_hotX; }
    void setHotX(qreal x);
    qreal hotY() const { return m_hotY; }
    void setHotY(qreal y);
    int size() const { return m_size; }
    void setSize(int size);
    bool pointerShown() const { return m_shown; }
    QPointF pointerScenePosition() const { return m_scenePos; }
    // The pointer drawn at this device pixel ratio: a white icon ringed in
    // black so it reads on light and dark clips alike.
    static QImage render(const QString &shape, int size, qreal dpr);
signals:
    void shapeChanged();
    void pointerChanged();
protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void updatePolish() override;
private:
    void rebuild();
    void watch(QQuickWindow *window);
    QString m_shape;
    qreal m_hotX = 0.5;
    qreal m_hotY = 0.5;
    int m_size = 24;
    QPointer<QQuickWindow> m_window;
    QPointer<ToolCursorGlyph> m_glyph;
    QPointF m_scenePos;
    bool m_inWindow = false;
    bool m_shown = false;
};
