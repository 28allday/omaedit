#pragma once
#include <QImage>
#include <QVariantMap>
namespace LinearGrade {
// Straight scene-linear float correction. Coverage never enters colour math.
QImage correct(const QImage &image, const QVariantMap &node, QString *error);
// General mathematical path retained for numerical verification/benchmarks.
QImage correctReference(const QImage &image, const QVariantMap &node, QString *error);
// Reports whether this node's primary/curve stage is an RGB affine transform.
// Spatial detail and LUT stages, if any, are still applied afterwards.
bool usesAffine(const QVariantMap &node);
} // namespace LinearGrade
