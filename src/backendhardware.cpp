#include "backend.h"
#include "hardwareprobe.h"
#include "viewerprocessor.h"
#include "scopeprocessor.h"
#include "interface.h"
#include "startup.h"
#include "exportspec.h"
#include "storagepreferences.h"
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSysInfo>
#include <QThread>
#include <QSettings>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

void Backend::setupHardware() {
    m_hardware = new HardwareSettings(this);
    connect(m_hardware, &HardwareSettings::viewerProcessingPreferenceChanged, this, [this] {
        if (m_viewerProcessor) m_viewerProcessor->setPreference(m_hardware->viewerProcessingPreference());
    });
}
ViewerProcessor *Backend::viewerProcessor() {
    if(!m_viewerProcessor) {
        m_viewerProcessor=new ViewerProcessor(this);m_viewerProcessor->setPreference(m_hardware->viewerProcessingPreference());
        auto *timer=new QTimer(m_viewerProcessor);timer->setSingleShot(true);timer->setInterval(1000);
        connect(timer,&QTimer::timeout,m_hardware,&HardwareSettings::infoChanged);
        connect(m_viewerProcessor,&ViewerProcessor::changed,timer,[timer]{if(!timer->isActive())timer->start();});
    }
    return m_viewerProcessor;
}
QString Backend::licensesFolder() const {
    for (const QString &candidate : {QCoreApplication::applicationDirPath() + QStringLiteral("/../licenses"),
                                     QStringLiteral("/usr/share/licenses/omaedit"), QStringLiteral("/usr/local/share/licenses/omaedit")})
        if (QFileInfo(candidate).isDir()) return QDir(candidate).canonicalPath();
    return QString();
}
QString Backend::systemInformation() const {
    auto info = m_hardware->info();
    info["application"] = "OmaEdit";
    info["build"] = QCoreApplication::applicationVersion().isEmpty() ? "Development build" : QCoreApplication::applicationVersion();
    info["qtVersion"] = qVersion();
    info["os"] = QSysInfo::prettyProductName();
    info["kernel"] = QSysInfo::kernelType() + ' ' + QSysInfo::kernelVersion();
    info["architecture"] = QSysInfo::currentCpuArchitecture();
    info["logicalCpuCount"] = QThread::idealThreadCount();
    info["platform"] = QGuiApplication::platformName();
    QString renderer = "Not initialized";
    for (auto *window : QGuiApplication::allWindows()) {
        auto *quick = qobject_cast<QQuickWindow *>(window);
        if (!quick || !quick->isSceneGraphInitialized()) continue;
        switch (quick->rendererInterface()->graphicsApi()) {
        case QSGRendererInterface::Vulkan: renderer = "Vulkan"; break;
        case QSGRendererInterface::OpenGL: renderer = "OpenGL"; break;
        case QSGRendererInterface::Direct3D11: renderer = "Direct3D 11"; break;
        case QSGRendererInterface::Direct3D12: renderer = "Direct3D 12"; break;
        case QSGRendererInterface::Metal: renderer = "Metal"; break;
        case QSGRendererInterface::Software: renderer = "Software"; break;
        case QSGRendererInterface::Null: renderer = "Null"; break;
        default: renderer = "Unknown"; break;
        }
        break;
    }
    info["interfaceRenderer"] = renderer;
    info["interfaceStartup"] = SuiteQuick::interfaceStartup();
    info["interfaceRenderingPreference"] = m_hardware->interfaceRenderingPreference();
    info["interfaceRestartRequired"] = m_hardware->interfaceRenderingRestartRequired();
    info["viewerSurface"] = "Qt Quick image nodes";
    info["processingBackend"] = "CPU";
    info["cacheProcessingPreference"] = m_backgroundRenderProcessing;
    info["cacheProcessing"] = m_backgroundRender.value("processing").toMap();
    info["viewerProcessingPreference"]=m_hardware->viewerProcessingPreference();
    info["viewerProcessing"] = m_viewerProcessor ? m_viewerProcessor->information() : QVariantMap{{"backend","CPU"},{"workers",0}};
    info["scopeProcessing"] = m_scopeProcessor ? m_scopeProcessor->information() : QVariantMap{{"backend","cpu"},{"workerThreads",0}};
    info["playbackDecoder"] = SuiteVideo::defaultDecodeOptions().backend;
    info["decodeStartup"]=SuiteVideo::decodeStartup();
    info["nativeDecoding"]=SuiteVideo::runtimeInformation();
    info["videoDecoderPreference"]=m_hardware->videoDecoderPreference();info["videoDecoderDevice"]=m_hardware->videoDecoderDevice();
    info["videoDecoderRestartRequired"]=m_hardware->videoDecoderRestartRequired();
    // Preferences ▸ Storage: what this process uses, and whether saved
    // values are waiting for a restart — support asks for exactly this.
    info["storage"] = QVariantMap{{"active", storageActivePaths()}, {"saved", storageSettings()},
        {"session", StoragePreferences::session()}, {"restartRequired", storageRestartRequired()},
        {"usage", storageUsage()}, {"audioBufferFrames", m_audioBufferFrames}};
#ifdef Q_OS_LINUX
    QFile memory("/proc/meminfo");
    if (memory.open(QIODevice::ReadOnly)) {
        QVariantMap values;
        for (const auto &line : memory.readAll().split('\n')) {
            const auto parts = line.simplified().split(' ');
            if (parts.size() < 2) continue;
            if (parts[0] == "MemTotal:" || parts[0] == "MemAvailable:")
                values[QString::fromLatin1(parts[0]).chopped(1)] = parts[1].toULongLong() * 1024ULL;
        }
        info["systemMemoryBytes"] = values;
    }
#endif
    return QString::fromUtf8(QJsonDocument::fromVariant(info).toJson(QJsonDocument::Indented));
}
void Backend::copyText(const QString &text) {
    if (QGuiApplication::clipboard())
        QGuiApplication::clipboard()->setText(text);
}

bool Backend::copySystemInformation() {
    if (!QGuiApplication::clipboard()) return false;
    QGuiApplication::clipboard()->setText(systemInformation());
    return true;
}
