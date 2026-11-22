#pragma once
#include <QVariantMap>

namespace AudioTimingEdit {
// Shared by clip timing fields and the sound cursor. Empty means invalid.
QVariantMap parsePosition(const QString &text, const QString &unit, int sampleRate);
QString formatPosition(const QVariantMap &time, const QString &unit, int sampleRate);
QVariantMap fields(const QVariantMap &timing, const QString &unit, int sampleRate);
// Numeric sound controls use exact rational seconds, including non-grid values.
// Fields contains only edited start/in/duration strings in the displayed unit.
QVariantMap preview(const QVariantMap &entry, int fpsNum, int fpsDen,
                    const QVariantMap &fields, const QString &unit, int sampleRate);
// A pointer delta is quantized once by the view; all clip arithmetic remains
// rational. A snap carries an existing exact boundary, never a rounded label.
QVariantMap gesture(const QVariantMap &entry, int fpsNum, int fpsDen,
                    const QString &action, const QVariantMap &changes, int sampleRate);
}
