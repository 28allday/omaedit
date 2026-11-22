#pragma once

#include <QColorSpace>
#include <QImage>
#include <QVariantMap>
#include "workingframe.h"

// The Color pipeline is shared by preview, timeline and export: every grade
// is corrected here, in floating point on 16-bit pictures (or 32-bit float
// for scene-linear grades).
namespace GradeEngine {
bool required(const QVariantMap &grade);
// A plain grade whose clip is in another space than its output (Rec.601 or
// Rec.2020 primaries): the engine converts before correcting.
bool spaceConversion(const QVariantMap &grade);
// Pin pre-model grades to the selection algorithm they originally rendered with.
QVariantMap pinMaskModels(const QVariantMap &grade);
QVariantList pinVersionMaskModels(const QVariantList &versions);
QString validate(const QVariantMap &grade);
QVariantMap nodeAt(const QVariantMap &grade, const QString &path);
bool replaceNode(QVariantMap *grade, const QString &path, const QVariantMap &node);
QVariantMap freshCopy(const QVariantMap &grade);
QStringList resourceProblems(const QVariantMap &grade);
QVariantMap evaluateNode(const QVariantMap &node, int frame);
// A mask window where its track puts it at `frame` (x, y, w, h, rotation).
QVariantMap trackedWindowAt(const QVariantMap &window, int frame);
QColorSpace space(const QString &gamut, const QString &gamma = QString());
QImage convert(const QImage &image, const QString &from, const QString &fromGamma, const QString &to,
               const QString &toGamma);
QImage mask(const QImage &image, const QVariantMap &node, int frame);
QImage render(const QImage &image, const QVariantMap &grade, int frame, QString *error = nullptr);
// A display-referred grade on a 16-bit (or 8-bit) picture, corrected in place:
// the one-node fast path when the grade is that simple, else the full graph.
// The same result as the clip's grade filter gives the picture.
bool correct(QImage &image, const QVariantMap &grade, int frame, QString *error = nullptr);
// The grade's one node at `frame` when the whole grade is that one node's
// primaries and RGB curves (DisplayGrade does all of it); empty otherwise.
QVariantMap plainNode(const QVariantMap &grade, int frame);
// Corrections only: an already-linear frame must never be input-transformed
// again or output-transformed before composition. Legacy render() is unchanged.
WorkingFrame renderWorking(const WorkingFrame &image, const QVariantMap &grade, int frame,
                           QString *error = nullptr, bool matteView = false);
} // namespace GradeEngine
