#pragma once
#include <QVariantMap>

namespace SuiteHardware {
constexpr int schemaVersion = 1;
// Inventory only: no processing operation is enabled by detecting a device.
QVariantMap vulkanInventory(const QString &loaderOverride = {});
// Call before constructing the app. Returns -1 for ordinary application use.
// Each app embeds its own probe mode, so no companion app/service is required.
int probeMain(int argc, char **argv);
}
