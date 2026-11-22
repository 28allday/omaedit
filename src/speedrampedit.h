#pragma once
#include <QVariantMap>

namespace SpeedRampEdit {
// Drafts map the visible cut locally. Applying one splices it into the parent
// map, retaining hidden trim handles on either side.
QVariantMap prepare(const QVariantMap &entry,double sourceLength);
QVariantMap inspect(const QVariantMap &definition);
QVariantMap edit(const QVariantMap &definition,const QString &action,int index,
                 const QVariantMap &options = {});
QVariantMap splice(const QVariantMap &entry,const QVariantMap &parent,
                   const QVariantMap &definition,bool normal = false,double sourceRate = 0.);
// A source-independent preset uses the visible source range as its origin.
// Fitting scales both source distances and output duration; whole-frame
// rounding adjusts rates slightly to keep the exact material and ramp phase.
QVariantMap preset(const QVariantMap &entry);
QVariantMap fitPreset(const QVariantMap &pattern,const QVariantMap &destination);
}
