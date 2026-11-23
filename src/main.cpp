#include "engine.h"
#include "cachecodec.h"
#include "sink.h"
#include "vulkaninventory.h"
#include "interface.h"
#include "decoder.h"
#include "startup.h"
#include "engines.h"
// omaedit — an Omarchy app skeleton. Qt Quick (QML) UI, C++ backend.

#include <QGuiApplication>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include "backend.h"
#include "agentcli.h"
#include "applicationtranslations.h"
#include "storagepreferences.h"
#include "theme.h"
#include "thumbnailprovider.h"
#include "videoview.h"

int main(int argc, char *argv[]) {
    SuiteVideo::enableDriverVideoQueues();
    if (const int probe = SuiteVideo::decodeProbeMain(argc, argv); probe >= 0) return probe;
    if (const int probe = SuiteQuick::interfaceProbeMain(argc, argv); probe >= 0) return probe;
    if (const int probe = SuiteHardware::probeMain(argc, argv); probe >= 0) return probe;
    if (const int probe = engine::probeMain(argc, argv); probe >= 0) return probe;
    if (const int probe = render::cachecodec::probeMain(argc, argv); probe >= 0) return probe;
    if (const int probe = deliver::probeMain(argc, argv); probe >= 0) return probe;
    if (const int cli = agentCliMain(argc, argv); cli >= 0) return cli;
    QGuiApplication app(argc, argv);

    // The identity trio. setDesktopFileName is what becomes the Wayland app_id,
    // which is what Hyprland rules, the taskbar and the icon lookup key off.
    app.setOrganizationName(QStringLiteral("omaedit")); // QML Settings shares Backend's QSettings file
    app.setApplicationName(QStringLiteral("omaedit"));
    app.setApplicationVersion(QStringLiteral(OMAEDIT_VERSION));
    app.setDesktopFileName(QStringLiteral("omaedit"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("omaedit")));

    ApplicationTranslations translations(app);

    // The same Qt Controls family Quickshell — the Omarchy shell — builds on.
    QQuickStyle::setStyle(QStringLiteral("Material"));
    SuiteQuick::configureInterface();
    // (The GPU that draws is the first one tried for decoding.)
    {
      const int drawing = render::engineGpu();
      SuiteVideo::configureDecoding(drawing >= 0 ? QString::number(drawing) : QString());
    }

    // The process-wide instance — the same object QML reads as
    // OmaEdit.Desktop.OmarchyTheme, so the window and the chrome follow one
    // watcher rather than two.
    OmarchyTheme &theme = *OmarchyTheme::instance();
    Backend backend;
    backend.startClean();

    // Carry the desktop's text size into the default font so every control
    // grows with `omarchy display text size`, without a restart.
    // Text that names no face of its own takes the app's one too (the same
    // order as Theme.fontFamily).
    {
        const QStringList families = QFontDatabase::families();
        for (const char *wanted : {"JetBrains Mono", "JetBrainsMono Nerd Font", "JetBrainsMono NF"}) {
            if (families.contains(QLatin1String(wanted))) {
                QFont font = app.font();
                font.setFamily(QLatin1String(wanted));
                app.setFont(font);
                break;
            }
        }
    }
    const qreal basePointSize = app.font().pointSizeF();
    const auto scaleFont = [&app, basePointSize](qreal scale) {
        QFont font = app.font();
        font.setPointSizeF(basePointSize * scale);
        app.setFont(font);
    };
    scaleFont(theme.textScale());
    QObject::connect(&theme, &OmarchyTheme::textScaleChanged, &app, [&theme, scaleFont] {
        scaleFont(theme.textScale());
    });

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [](const QList<QQmlError> &warnings) {
        for (const QQmlError &warning : warnings)
            qWarning().noquote() << warning.toString();
    });
    // The OmaEdit.Ui module (Theme singleton + shared primitives) lives in
    // the binary's resources, so it resolves with no on-disk module dir.
    engine.addImportPath(QStringLiteral(":/qml"));
    qmlRegisterType<VideoView>("omaedit", 1, 0, "VideoView");
    auto *thumbs = new ThumbnailProvider(StoragePreferences::session().value(QStringLiteral("thumbnailMemoryMiB")).toInt() * 1024); // engine takes ownership
    engine.addImageProvider(QStringLiteral("thumbs"), thumbs);
    backend.setThumbnailProvider(thumbs);
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    engine.rootContext()->setContextProperty(QStringLiteral("theme"), &theme);

    // Every window opens on the Media page, where a session starts with its footage.
    engine.setInitialProperties({{QStringLiteral("workspace"), QStringLiteral("Media")}});
    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        qCritical() << "Could not load the omaedit interface.";
        return -1;
    }

    // Dev hook: OMAEDIT_GRAB=/path.png renders the window once it has settled
    // and quits. Used with QT_QPA_PLATFORM=offscreen for before/after checks
    // of the interface without ever putting a window on a live screen.
    if (const QByteArray grab = qgetenv("OMAEDIT_GRAB"); !grab.isEmpty()) {
        if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
            // OMAEDIT_GRAB_PROPS="selectedTimelineIndex=0;workspace=Edit" sets
            // window properties just before the grab, to picture a state that
            // needs a click (a selection, a workspace) without a pointer.
            QTimer::singleShot(1500, window, [window] {
                const QString props = QString::fromLocal8Bit(qgetenv("OMAEDIT_GRAB_PROPS"));
                for (const QString &pair : props.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                    const int eq = pair.indexOf(QLatin1Char('='));
                    if (eq > 0)
                        window->setProperty(pair.left(eq).trimmed().toUtf8().constData(),
                                            pair.mid(eq + 1).trimmed());
                }
            });
            // OMAEDIT_GRAB_OPEN="projectSettings:openFor" calls a method on a
            // named child first — a dialog that only a key or menu opens.
            // A third part is passed as the method's one argument
            // ("preferences:openAt:storage"), or split at '|' as two
            // ("workspace:Deliver:edit:subtitles|sidecar"); calls are
            // separated by ';'. OMAEDIT_GRAB_OPEN_MS waits longer (a page
            // that loads when shown).
            QTimer::singleShot(qBound(1600, qEnvironmentVariableIntValue("OMAEDIT_GRAB_OPEN_MS"), 60000), window, [window] {
                const QString all = QString::fromLocal8Bit(qgetenv("OMAEDIT_GRAB_OPEN"));
                for (const QString &spec : all.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
                    // The child's name may itself hold a ':' ("workspace:Deliver").
                    QStringList parts = spec.split(QLatin1Char(':'));
                    QObject *target = nullptr;
                    for (int k = 1; k < parts.size() && !target; ++k)
                        if ((target = window->findChild<QObject *>(parts.mid(0, k).join(QLatin1Char(':')))))
                            parts = QStringList{parts.mid(0, k).join(QLatin1Char(':'))} + parts.mid(k);
                    if (parts.size() < 2 || !target) {
                        qWarning().noquote() << "grab: nothing named in" << spec;
                        continue;
                    }
                    const QByteArray method = parts.at(1).toUtf8();
                    const QString argument = parts.mid(2).join(QLatin1Char(':'));
                    const QStringList two = argument.split(QLatin1Char('|'));
                    const bool called = parts.size() > 2 && two.size() == 2
                        ? QMetaObject::invokeMethod(target, method.constData(), Q_ARG(QVariant, two.at(0)), Q_ARG(QVariant, two.at(1)))
                        : parts.size() > 2 ? QMetaObject::invokeMethod(target, method.constData(), Q_ARG(QVariant, argument))
                                           : QMetaObject::invokeMethod(target, method.constData());
                    if (!called)
                        qWarning().noquote() << "grab: could not call" << spec;
                }
            });
            const int captureDelay = qBound(2600, qEnvironmentVariableIntValue("OMAEDIT_GRAB_DELAY_MS"), 60000);
            QTimer::singleShot(captureDelay, window, [window, grab, &backend] {
                bool ok = window->grabWindow().save(QString::fromLocal8Bit(grab));
                const auto diagnostics=qEnvironmentVariable("OMAEDIT_GRAB_DIAGNOSTICS");
                if(!diagnostics.isEmpty()) {
                    QSaveFile output(diagnostics);const auto bytes=backend.systemInformation().toUtf8();
                    ok=output.open(QIODevice::WriteOnly) && output.write(bytes)==bytes.size() && output.commit() && ok;
                }
                qInfo().noquote() << (ok ? "grabbed" : "grab FAILED") << grab;
                QCoreApplication::exit(ok ? 0 : 2);
            });
        }
    }

    // "Open with", and `omaedit somefile` from a terminal: import to the
    // library and drop it straight onto the timeline.
    const QStringList args = app.arguments();
    if (args.size() > 1)
        backend.open(QUrl::fromLocalFile(args.at(1)), true);

    return app.exec();
}
