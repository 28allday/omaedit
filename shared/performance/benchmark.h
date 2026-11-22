#pragma once
#include <QVariantMap>
#include <QVector>
#include <functional>
namespace SuitePerformance {
constexpr int schemaVersion=1;
struct Options { int warmups=2; int iterations=7; };
// Samples are elapsed milliseconds. Invalid/empty input returns an error.
QVariantMap statistics(const QVector<double> &milliseconds);
// The operation must finish all work (including GPU fences/readback, if any)
// before returning. Preparation and result verification belong outside it.
QVariantMap measure(const std::function<bool(QString*)> &operation, const Options &options={});
}
