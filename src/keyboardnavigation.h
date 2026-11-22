#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QQuickWindow>

// Window-local focus policy, shared by docked and detached workspaces.
class KeyboardNavigation : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickWindow *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(QQuickItem *focusItem READ focusItem NOTIFY focusChanged)
    Q_PROPERTY(QQuickItem *overlayParent READ overlayParent NOTIFY windowChanged)
    Q_PROPERTY(QRectF focusRect READ focusRect NOTIFY focusRectChanged)
    Q_PROPERTY(bool textFocused READ textFocused NOTIFY focusChanged)
    Q_PROPERTY(bool keyboardMode READ keyboardMode NOTIFY keyboardModeChanged)
public:
    explicit KeyboardNavigation(QObject *parent = nullptr) : QObject(parent) {}
    QQuickWindow *window() const { return m_window; }
    void setWindow(QQuickWindow *window);
    QQuickItem *focusItem() const { return m_window ? m_window->activeFocusItem() : nullptr; }
    QQuickItem *overlayParent() const { return m_window ? m_window->contentItem() : nullptr; }
    QRectF focusRect() const { return m_focusRect; }
    bool textFocused() const;
    bool keyboardMode() const { return m_keyboardMode; }
    Q_INVOKABLE bool cyclePanel(bool backwards = false);
signals:
    void windowChanged();
    void focusChanged();
    void focusRectChanged();
    void keyboardModeChanged();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    bool controlFocused() const;
    void setKeyboardMode(bool enabled);
    void revealFocus();
    void updateFocusRect();
    QPointer<QQuickWindow> m_window;
    bool m_keyboardMode = false;
    QRectF m_focusRect;
};
