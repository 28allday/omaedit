#pragma once
#include <QVariantMap>
#include <QVector>
namespace SuitePerformance {
// Select Vulkan only when every observed GPU sample beats the fastest CPU
// sample by 20%, with at least 0.5 ms saved and verified pixel agreement.
QVariantMap selectBackend(const QVector<double> &cpu,const QVector<double> &gpu,bool agreement);
}
