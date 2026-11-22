#pragma once
#include <QVariantMap>

namespace TimelineRate {
double sourceRate(const QVariantMap &entry);
// originSeconds is the beginning of the copied selection in its source
// timeline. Shared rounding keeps adjacent cuts and linked tracks together.
QVariantMap convert(const QVariantMap &entry, double destination, double originSeconds,
                    QString *error = nullptr, double destinationInterpretation = 0.);
// Keep the selected recorded material while changing its playback clock.
// Timeline duration and local animation change; the user's speed stays intact.
QVariantMap reinterpretSource(const QVariantMap &entry, const QVariantMap &metadata,
                              double timelineFps, QString *error = nullptr);
}
