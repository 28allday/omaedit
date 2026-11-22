#include "keyboardnavigation.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlEngine>
#include <QStyleHints>
#include <QTimer>
#include <algorithm>
#include <functional>

void KeyboardNavigation::setWindow(QQuickWindow *window) {
    if (m_window == window) return;
    if (m_window) {
        m_window->removeEventFilter(this);
        disconnect(m_window, nullptr, this, nullptr);
    }
    m_window = window;
    QGuiApplication::styleHints()->setTabFocusBehavior(Qt::TabFocusAllControls);
    if (window) {
        window->installEventFilter(this);
        connect(window, &QQuickWindow::activeFocusItemChanged, this, [this] {
            emit focusChanged();
            updateFocusRect();
            if (m_keyboardMode) QTimer::singleShot(0, this, &KeyboardNavigation::revealFocus);
        });
        connect(window, &QQuickWindow::afterAnimating, this, &KeyboardNavigation::updateFocusRect);
    }
    updateFocusRect();
    emit windowChanged();
    emit focusChanged();
}

bool KeyboardNavigation::textFocused() const {
    const auto *item = focusItem();
    return item && (item->flags().testFlag(QQuickItem::ItemAcceptsInputMethod)
                    || item->property("cursorPosition").isValid());
}

bool KeyboardNavigation::controlFocused() const {
    const auto *item = focusItem();
    // A containing SplitView/ScrollView is also a Control: walking ancestors
    // would incorrectly take playback keys away from the timeline canvas.
    return item && (item->inherits("QQuickControl")
                    || item->property("keyboardConsumesKeys").toBool() || textFocused());
}

void KeyboardNavigation::setKeyboardMode(bool enabled) {
    if (m_keyboardMode == enabled) return;
    m_keyboardMode = enabled;
    emit keyboardModeChanged();
    updateFocusRect();
}

bool KeyboardNavigation::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_window) return false;
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TouchBegin)
        setKeyboardMode(false);
    // A press away from a text field in the window ends the typing: the keys go
    // back to the editor. In a pop-up the field stays with its dialog's own flow.
    if (event->type() == QEvent::MouseButtonPress && textFocused()) {
        QQuickItem *field = focusItem();
        const QPointF at = field->mapFromScene(static_cast<QMouseEvent *>(event)->scenePosition());
        bool inPopup = false;
        for (const QQuickItem *up = field; up && !inPopup; up = up->parentItem()) inPopup = up->inherits("QQuickOverlay");
        if (!inPopup && !field->contains(at)) field->setFocus(false);
    }
    if (event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        if (key == Qt::Key_Tab || key == Qt::Key_Backtab) setKeyboardMode(true);
    }
    if (event->type() == QEvent::ShortcutOverride && controlFocused()) {
        auto *key = static_cast<QKeyEvent *>(event);
        // Plain keys belong to focused controls (Space clicks, arrows adjust,
        // Delete removes a chip). Keep modified and function-key app commands.
        const bool fieldEscape = focusItem() && focusItem()->property("keyboardConsumesEscape").toBool();
        // Reached by keyboard, a control keeps the keys: Space presses a button.
        // Reached any other way (a click, or the panel round a field that has just
        // let go) it keeps its arrows and nothing else: a click on a slider must
        // not take Space, J, K, L from the editor. Keys with no command behind them
        // still reach the control as usual.
        const int reason = focusItem()->property("focusReason").toInt();
        const bool byKeyboard = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
        const bool keepsAll = m_keyboardMode || byKeyboard || textFocused() || focusItem()->property("keyboardConsumesKeys").toBool();
        const bool arrow = key->key() == Qt::Key_Left || key->key() == Qt::Key_Right || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down
                           || key->key() == Qt::Key_PageUp || key->key() == Qt::Key_PageDown || key->key() == Qt::Key_Home || key->key() == Qt::Key_End;
        if ((keepsAll || arrow) && (key->key() != Qt::Key_Escape || fieldEscape)
            && !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
            && !(key->key() >= Qt::Key_F1 && key->key() <= Qt::Key_F35)) {
            key->accept();
            return true;
        }
    }
    return false;
}

void KeyboardNavigation::updateFocusRect() {
    QRectF rect;
    auto *focus = focusItem();
    if (m_keyboardMode && focus && focus->isVisible() && focus->opacity() > 0) {
        auto *surface = m_window->contentItem();
        rect = focus->mapRectToItem(surface, focus->boundingRect());
        // Keep the outline in a separate window overlay: adding visual children
        // to a Menu changes its content model and breaks reopened menu actions.
        for (auto *parent = focus->parentItem(); parent; parent = parent->parentItem())
            if (parent->clip()) rect = rect.intersected(parent->mapRectToItem(surface, parent->boundingRect()));
        rect = rect.intersected(surface->boundingRect());
    }
    if (rect == m_focusRect) return;
    m_focusRect = rect;
    emit focusRectChanged();
}

void KeyboardNavigation::revealFocus() {
    auto *focus = focusItem();
    if (!focus || !focus->isVisible()) return;
    // Scroll the innermost viewport first. Public Flickable properties also
    // cover ListView/GridView and avoid depending on Qt's private headers.
    for (auto *item = focus->parentItem(); item; item = item->parentItem()) {
        if (!item->inherits("QQuickFlickable")) continue;
        auto *content = qvariant_cast<QQuickItem *>(item->property("contentItem"));
        if (!content) continue;
        const QRectF rect = focus->mapRectToItem(content, focus->boundingRect());
        const auto scroll = [item](const char *position, const char *origin, const char *extent,
                                   qreal start, qreal end, qreal viewport) {
            const qreal old = item->property(position).toReal();
            const qreal minimum = item->property(origin).toReal();
            const qreal maximum = minimum + qMax(qreal(0), item->property(extent).toReal() - viewport);
            qreal next = old;
            if (end > old + viewport) next = end - viewport;
            if (start < next) next = start;
            item->setProperty(position, qBound(minimum, next, maximum));
        };
        scroll("contentX", "originX", "contentWidth", rect.left(), rect.right(), item->width());
        scroll("contentY", "originY", "contentHeight", rect.top(), rect.bottom(), item->height());
    }
    updateFocusRect();
}

bool KeyboardNavigation::cyclePanel(bool backwards) {
    if (!m_window) return false;
    QList<QQuickItem *> panels;
    std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
        if (!item->isVisible() || !item->isEnabled()) return;
        if (!item->property("keyboardRegion").toString().isEmpty()
            && item->width() > 0 && item->height() > 0) {
            panels.append(item);
            return;
        }
        for (auto *child : item->childItems()) collect(child);
    };
    collect(m_window->contentItem());
    if (panels.isEmpty()) return false;
    std::stable_sort(panels.begin(), panels.end(), [](QQuickItem *a, QQuickItem *b) {
        const auto ap = a->mapToScene(QPointF()), bp = b->mapToScene(QPointF());
        return ap.y() != bp.y() ? ap.y() < bp.y() : ap.x() < bp.x();
    });
    int current = -1;
    for (auto *item = focusItem(); item; item = item->parentItem()) {
        current = panels.indexOf(item);
        if (current >= 0) break;
    }
    const int next = current < 0 ? (backwards ? panels.size() - 1 : 0)
        : (current + (backwards ? -1 : 1) + panels.size()) % panels.size();
    setKeyboardMode(true);
    panels[next]->forceActiveFocus(Qt::TabFocusReason);
    QTimer::singleShot(0, this, &KeyboardNavigation::revealFocus);
    return true;
}

static void registerKeyboardNavigation() {
    qmlRegisterType<KeyboardNavigation>("OmaEdit.Input", 1, 0, "KeyboardNavigation");
}
Q_COREAPP_STARTUP_FUNCTION(registerKeyboardNavigation)
