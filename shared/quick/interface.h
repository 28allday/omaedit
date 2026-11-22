#pragma once
#include <QVariantMap>
namespace SuiteQuick {
QString normalizedInterfacePreference(const QString &value);
// Probe entry point must run before the application creates its QGuiApplication.
int interfaceProbeMain(int argc,char **argv);
// Called on the GUI thread before constructing any QQuickWindow.
QVariantMap configureInterface();
QVariantMap interfaceStartup();
void recordInterfaceFailure(const QString &reason);
}
