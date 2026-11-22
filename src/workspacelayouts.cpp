#include "workspacelayouts.h"
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QJSValue>
#include <QScreen>
#include <QSettings>

namespace {
QVariant plain(const QVariant &v) {
    if (v.metaType().id() == qMetaTypeId<QJSValue>())
        return plain(v.value<QJSValue>().toVariant());
    if (v.metaType().id() == QMetaType::QVariantMap) {
        auto m = v.toMap();
        for (auto i = m.begin(); i != m.end(); ++i)
            i.value() = plain(i.value());
        return m;
    }
    if (v.metaType().id() == QMetaType::QVariantList) {
        auto list = v.toList();
        for (auto &i : list)
            i = plain(i);
        return list;
    }
    return v;
}
QString key(const QString &workspace) { return "workspaceLayouts/v1/" + workspace; }
QString presetKey(const QString &workspace, const QString &name) {
    return key(workspace) + "/presets/" +
           QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex());
}
} // namespace
WorkspaceLayouts::WorkspaceLayouts(QObject *parent) : QObject(parent) {
    auto watch = [this](QScreen *s) {
        connect(s, &QScreen::availableGeometryChanged, this, &WorkspaceLayouts::screensChanged);
    };
    for (auto *s : QGuiApplication::screens())
        watch(s);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this, watch](QScreen *s) {
        watch(s);
        emit screensChanged();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { emit screensChanged(); });
}
bool WorkspaceLayouts::validWorkspace(const QString &workspace) const {
    return QStringList{"Media", "Edit", "Color", "Audio", "Deliver"}.contains(workspace);
}
QVariantMap WorkspaceLayouts::read(const QString &workspace) const {
    if (!validWorkspace(workspace))
        return {};
    const auto state = QSettings().value(key(workspace) + "/current").toMap();
    return state.value("version").toInt() == 1 ? state : QVariantMap();
}
void WorkspaceLayouts::write(const QString &workspace, const QVariantMap &state) {
    if (!validWorkspace(workspace) || state.value("version").toInt() != 1)
        return;
    QSettings settings;
    settings.setValue(key(workspace) + "/current", plain(state));
    settings.sync();
}
QStringList WorkspaceLayouts::presetNames(const QString &workspace) const {
    if (!validWorkspace(workspace))
        return {};
    QSettings settings;
    settings.beginGroup(key(workspace) + "/presets");
    QStringList names;
    for (const auto &id : settings.childKeys()) {
        const auto m = settings.value(id).toMap();
        if (!m.value("name").toString().isEmpty())
            names.append(m.value("name").toString());
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}
bool WorkspaceLayouts::savePreset(const QString &workspace, const QString &name, const QVariantMap &state,
                                  bool replace) {
    const auto clean = name.trimmed();
    if (!validWorkspace(workspace) || clean.isEmpty() || clean.size() > 80 ||
        state.value("version").toInt() != 1)
        return false;
    QSettings settings;
    const auto path = presetKey(workspace, clean);
    if (settings.contains(path) && !replace)
        return false;
    settings.setValue(path, QVariantMap{{"name", clean}, {"state", plain(state)}});
    settings.sync();
    ++m_revision;
    emit changed();
    return true;
}
QVariantMap WorkspaceLayouts::preset(const QString &workspace, const QString &name) const {
    if (!validWorkspace(workspace))
        return {};
    return QSettings().value(presetKey(workspace, name)).toMap().value("state").toMap();
}
void WorkspaceLayouts::removePreset(const QString &workspace, const QString &name) {
    if (!validWorkspace(workspace))
        return;
    QSettings settings;
    settings.remove(presetKey(workspace, name));
    settings.sync();
    ++m_revision;
    emit changed();
}
QVariantList WorkspaceLayouts::screens() const {
    QVariantList list;
    for (auto *s : QGuiApplication::screens()) {
        const auto r = s->availableGeometry();
        list.append(QVariantMap{
            {"name", s->name()}, {"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}});
    }
    return list;
}
QRect WorkspaceLayouts::fitGeometry(const QRect &requested, const QRect &available) {
    const QSize size(qBound(1, requested.width(), qMax(1, available.width())),
                     qBound(1, requested.height(), qMax(1, available.height())));
    return QRect(QPoint(qBound(available.left(), requested.x(), available.right() - size.width() + 1),
                        qBound(available.top(), requested.y(), available.bottom() - size.height() + 1)),
                 size);
}
QVariantMap WorkspaceLayouts::windowGeometry(QWindow *window) const {
    if (!window)
        return {};
    const auto r = window->geometry();
    return {{"x", r.x()},
            {"y", r.y()},
            {"width", r.width()},
            {"height", r.height()},
            {"screen", window->screen() ? window->screen()->name() : QString()}};
}
void WorkspaceLayouts::placeWindow(QWindow *window, const QVariantMap &geometry, const QString &screenName) {
    if (!window)
        return;
    auto *screen = QGuiApplication::primaryScreen();
    const auto name = screenName.isEmpty() ? geometry.value("screen").toString() : screenName;
    bool matched = false;
    for (auto *s : QGuiApplication::screens())
        if (s->name() == name) {
            screen = s;
            matched = true;
            break;
        }
    if (!screen)
        return;
    const auto available = screen->availableGeometry();
    QRect r(geometry.value("x", available.x() + 40).toInt(), geometry.value("y", available.y() + 40).toInt(),
            qMax(260, geometry.value("width", 800).toInt()),
            qMax(180, geometry.value("height", 500).toInt()));
    if (!matched || !screenName.isEmpty())
        r.moveCenter(available.center());
    window->setScreen(screen);
    window->create();
    // Saved geometry describes the client area. Keep the title bar and borders
    // on the display too, including when a large panel returns to a smaller one.
    const auto frame = window->frameMargins();
    const auto clientArea = available.adjusted(frame.left(), frame.top(), -frame.right(), -frame.bottom());
    window->setGeometry(fitGeometry(r, clientArea.isValid() ? clientArea : available));
}
