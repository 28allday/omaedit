#pragma once
#include <QVariantMap>

namespace RetimeEdit {
// Marked clips retain a physical source anchor; older entries keep their
// established whole-frame mapping. Dialog and timeline share these bounds.
QVariantMap preview(const QVariantMap &entry, double speed);
QVariantMap forDuration(const QVariantMap &entry, int frames, bool reverse);
}
