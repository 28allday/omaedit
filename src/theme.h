#pragma once

#include <QColor>
#include <QHash>
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>

class QDBusVariant;

// Everything the app needs to look like it belongs on this desktop: the current
// Omarchy palette, the desktop's dark/light preference, and its text scale.
// All of it is optional at runtime — on a machine without Omarchy themes or a
// portal, the built-in fallbacks keep the app looking sane.
//
// ⚠️ Of the colours here, only the ACCENT reaches the interface. The rest of
// the palette is FIXED in Ui/Theme.qml on purpose: a picture-editing surface
// has to be colour-stable, because a surround that changes hue changes how you
// read the image and the grade in front of it — which is why every editor
// worth the name pins neutral greys. The desktop gets to colour the chrome,
// never the viewer or the panels around it. See the note atop
// src/qml/OmaEdit/Ui/Theme.qml.
//
// Three documented Omarchy traps this has to survive:
//   · a theme change REPLACES the `current` symlink, which leaves the old
//     target's inode untouched — a watcher armed only on the target goes deaf,
//     so the DIRECTORY HOLDING THE LINK is watched too and all paths are
//     re-armed on every change;
//   · none of these paths exist on plain Arch or in CI, where the app must
//     still look finished — hence the fallback to the fixed Oma accent;
//   · the value can be junk, so it is validated before it is believed.
class OmarchyTheme : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool darkMode READ darkMode NOTIFY darkModeChanged)
    Q_PROPERTY(qreal textScale READ textScale NOTIFY textScaleChanged)
    Q_PROPERTY(QString background READ background NOTIFY colorsChanged)
    Q_PROPERTY(QString foreground READ foreground NOTIFY colorsChanged)
    Q_PROPERTY(QString accent READ accent NOTIFY colorsChanged)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY colorsChanged)
    Q_PROPERTY(QString selection READ selection NOTIFY colorsChanged)
    // Whether the accent above came from a real Omarchy theme, or is the
    // fallback this app wears where there is no Omarchy to follow.
    Q_PROPERTY(bool following READ following NOTIFY colorsChanged)

public:
    explicit OmarchyTheme(QObject *parent = nullptr);

    // The one instance the whole process shares: the app's engine, every
    // extra window's engine and each QML test all read the same watcher
    // rather than each opening their own. Made on first use, which is after
    // QGuiApplication exists, so the watcher has an event loop to fire on.
    static OmarchyTheme *instance();

    bool darkMode() const { return m_darkMode; }
    qreal textScale() const { return m_textScale; }
    QString background() const { return m_background; }
    QString foreground() const { return m_foreground; }
    QString accent() const { return m_accent; }
    QString selection() const { return m_selection; }
    QString accentForeground() const { return foregroundFor(m_accent); }
    bool following() const { return m_following; }

    // The accent the app wears when there is no Omarchy theme to follow: the
    // Oma accent the interface was drawn in, so it still looks finished on
    // plain Arch, in a container or in CI.
    static QString fixedAccent();
    // Where the accent is read from, under the given home directory.
    static QString themeFile(const QString &home);

    // key = value pairs from an omarchy colors.toml; empty when there is none.
    static QHash<QString, QString> colorsFromFile(const QString &path);
    // "black" or "white", whichever stays legible on the given colour.
    static QString foregroundFor(const QString &color);
    // Whether a colour reads as dark, for deciding a theme's mode.
    static bool isDark(const QString &color, bool *known);

public slots:
    void reload();

private slots:
    void handlePortalSettingChanged(const QString &nameSpace, const QString &key,
                                    const QDBusVariant &value);

private:
    static QString colorsPath();
    void watchTheme();
    bool detectDarkMode() const;
    qreal detectTextScale() const;

    bool m_darkMode = true;
    bool m_following = false;
    qreal m_textScale = 1.0;
    QString m_background;
    QString m_foreground;
    QString m_accent;
    QString m_selection;
    QFileSystemWatcher m_watcher;

signals:
    void darkModeChanged();
    void textScaleChanged();
    void colorsChanged();
};
