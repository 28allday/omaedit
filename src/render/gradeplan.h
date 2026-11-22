#pragma once

// A colour-managed clip's grade as an engine grade plan (engine/grade.h):
// its node graph (serial, parallel, layer, combiner, splitter and compound
// nodes, keys from qualifiers and windows with their cleanup, key inputs,
// view nodes) compiled for one frame and picture width, as GradeEngine runs
// it on working frames. Pictures are reused once nothing reads them again.

#include "grade.h"

#include <QString>
#include <QVariantMap>
#include <memory>

namespace render {
// Null with `error` when the grade has something the engine does not draw
// yet (an external matte).
std::shared_ptr<const engine::GradePlan> gradePlan(const QVariantMap &grade, int frame, int width, QString *error);
// Whether a grade has anything to do (any enabled node, or a view node).
bool gradeActive(const QVariantMap &grade);
// Whether what these nodes do depends on the clip's frame: keys on a node, or
// a tracked window (a node's own or one of its list), at any depth of
// compound nodes. What gradePlan reads at `frame`; the renderer keeps a plan
// from frame to frame by it, and the playback cache its pictures.
bool gradeAnimated(const QVariantList &nodes);
}
