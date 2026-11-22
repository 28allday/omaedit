#include "interface.h"
#include "offscreen.h"
#include "imagesurface.h"
#include <QGuiApplication>
#include <QQuickWindow>
#include <QSettings>
#include <QProcess>
#include <QProcessEnvironment>
#include <QJsonDocument>
#include <QFile>
#include <QEvent>
#include <cstdio>
namespace SuiteQuick {
namespace {
class WindowGuard : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *object,QEvent *event) override {
        if(event->type()==QEvent::Show || event->type()==QEvent::PlatformSurface) {
            if(auto *window=qobject_cast<QQuickWindow*>(object);window && !window->property("omaedit.interfaceGuard").toBool()) {
                window->setProperty("omaedit.interfaceGuard",true);
                QObject::connect(window,&QQuickWindow::sceneGraphError,this,[](QQuickWindow::SceneGraphError,const QString &reason){recordInterfaceFailure(reason);qCritical().noquote()<<"Qt Quick interface initialization failed; next launch will use software:"<<reason;});
            }
        }
        return false;
    }
};
}
QString normalizedInterfacePreference(const QString &value){return value=="software"?value:value=="vulkan"?value:QString("auto");}
QVariantMap interfaceStartup(){return qApp->property("omaedit.interfaceStartup").toMap();}
int interfaceProbeMain(int argc,char **argv) {
    if(argc<3 || QByteArray(argv[1])!="--omaedit-ui-probe")return -1;
    const QByteArray choice(argv[2]);if(choice!="vulkan" && choice!="software")return 2;
    QGuiApplication app(argc,argv);
    QQuickWindow::setGraphicsApi(choice=="vulkan"?QSGRendererInterface::Vulkan:QSGRendererInterface::Software);
    Offscreen scene(QSize(32,16));QString error;
    if(!scene.initialize(&error)){fprintf(stderr,"%s\n",qPrintable(error));return 2;}
    ImageSurface surface(scene.window()->contentItem());surface.setSize(QSizeF(32,16));surface.setImageRect(QRectF(0,0,32,16));
    QImage pixels(32,16,QImage::Format_ARGB32_Premultiplied);pixels.fill(QColor(20,170,80));surface.setImage(pixels);
    const auto painted=scene.render(&error);const auto center=painted.isNull()?QColor():painted.pixelColor(16,8);
    auto report=scene.information();report["valid"]=!painted.isNull() && error.isEmpty() && qAbs(center.red()-20)<=1 && qAbs(center.green()-170)<=1 && qAbs(center.blue()-80)<=1;
    report["error"]=error;
    QFile output;if(!output.open(stdout,QIODevice::WriteOnly))return 2;output.write(QJsonDocument::fromVariant(report).toJson(QJsonDocument::Compact));output.write("\n");output.flush();
    return report.value("valid").toBool()?0:2;
}
QVariantMap configureInterface() {
    auto *guard=new WindowGuard(qApp);qApp->installEventFilter(guard);
    const auto saved=normalizedInterfacePreference(QSettings().value("hardware/interfaceRendering","auto").toString());
    const auto requested=normalizedInterfacePreference(qEnvironmentVariable("OMAEDIT_INTERFACE_RENDERING",saved));
    QVariantMap report{{"preference",requested},{"savedPreference",saved},{"selected","software"},{"schemaVersion",1}};
    const auto finish=[&](const QString &selected,const QString &reason){
        report["selected"]=selected;report["reason"]=reason;
        QQuickWindow::setGraphicsApi(selected=="vulkan"?QSGRendererInterface::Vulkan:QSGRendererInterface::Software);
        qApp->setProperty("omaedit.interfaceStartup",report);return report;
    };
    // Explicit Qt renderer variables remain authoritative, including developer
    // capture overrides. Report them instead of claiming our preflight chose it.
    if(qEnvironmentVariableIsSet("QT_QUICK_BACKEND") || qEnvironmentVariableIsSet("QSG_RHI_BACKEND")) {
        report["selected"]="environment";report["reason"]="Qt renderer environment override.";
        report["qtQuickBackend"]=qEnvironmentVariable("QT_QUICK_BACKEND");report["rhiBackend"]=qEnvironmentVariable("QSG_RHI_BACKEND");
        qApp->setProperty("omaedit.interfaceStartup",report);return report;
    }
    if(requested=="software")return finish("software","Software interface preference.");
    const auto failure=QSettings().value("hardware/interfaceFailure").toString();
    if(!failure.isEmpty())return finish("software","A previous interface initialization failed; software retained. "+failure);
    if(QGuiApplication::platformName()=="offscreen" || QGuiApplication::platformName()=="minimal")return finish("software","Headless platform uses the software interface; Vulkan remains available to processing.");
    // RenderControl never exposes a native window. Keep the desktop platform:
    // Qt's offscreen platform has no QVulkanInstance implementation.
    QProcess probe;auto env=QProcessEnvironment::systemEnvironment();env.insert("MANGOHUD","0");env.insert("DISABLE_MANGOHUD","1");env.insert("QT_FORCE_STDERR_LOGGING","1");
    probe.setProcessEnvironment(env);probe.setProgram(QCoreApplication::applicationFilePath());probe.setArguments({"--omaedit-ui-probe","vulkan"});probe.start();
    if(!probe.waitForStarted(1000) || !probe.waitForFinished(8000)) {probe.kill();probe.waitForFinished(1000);return finish("software","Qt Quick Vulkan startup probe timed out or could not start.");}
    const auto bytes=probe.readAllStandardOutput();const auto output=QJsonDocument::fromJson(bytes.size()<=65536?bytes:QByteArray{}).toVariant().toMap();
    report["probe"]=output;
    if(probe.exitStatus()!=QProcess::NormalExit || probe.exitCode()!=0 || !output.value("valid").toBool() || output.value("renderer").toString()!="Vulkan")return finish("software","Qt Quick Vulkan startup probe failed; software retained.");
    return finish("vulkan","Qt Quick Vulkan passed an isolated render/readback check.");
}
void recordInterfaceFailure(const QString &reason){QSettings().setValue("hardware/interfaceFailure",reason.left(1024));auto info=interfaceStartup();info["initializationError"]=reason.left(1024);qApp->setProperty("omaedit.interfaceStartup",info);}
}
