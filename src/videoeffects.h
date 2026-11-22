#pragma once

#include <QVariantList>
#include <QVariantMap>

namespace edit { class Producer; class Profile; }

// Curated, bounded controls for real video filters. The same schema drives the
// shelf, Inspector, project validation and command-line parameter validation.
namespace VideoEffects {
QVariantList catalogue();
QVariantMap definition(const QString &id);
QVariantMap defaults(const QString &id);
QString validate(const QString &id, const QVariantMap &params);
// An effect's keys, {control: [{frame, value, interpolation}]} in the clip's
// frames: only its number controls take them, values within the control's
// range. `tidy` gets them in frame order, one per frame, empty curves left out.
QString validateKeys(const QString &id, const QVariantMap &keys, QVariantMap *tidy = nullptr);
// An effect's mask: {shape: "rectangle" | "ellipse" | "polygon", x, y, w, h
// (centre and size, fractions of the frame), rotation (degrees), points
// ([[x, y], ...], a polygon's 3 to 8 corners), feather (0..0.5 of the frame's
// shorter side), invert}. The effect shows inside it only. Keys and light
// wrap work on the clip's edges and take none. `tidy`: with every field set.
QString validateMask(const QString &id, const QVariantMap &mask, QVariantMap *tidy = nullptr);
bool takesMask(const QString &id);
// A mask's keys, {field: [{frame, value, interpolation}]} in the clip's
// frames: x, y, w, h and rotation of a rectangle or ellipse, and any shape's
// feather, each within the field's own range. `tidy` as validateKeys'.
QString validateMaskKeys(const QString &id, const QVariantMap &mask, const QVariantMap &keys, QVariantMap *tidy = nullptr);
QVariantList read(edit::Producer *cut);
// Prepare every filter before replacing anything. Restoring retains unavailable
// effects in neutral carriers so their settings can be recovered on another host.
bool apply(edit::Profile &profile, edit::Producer *cut, const QVariantList &stack,
           bool restoring, QString *error = nullptr);
QStringList problems(const QVariantList &stack);
// The names of the stack's active effects that work at 8 bits with their
// settings: run on a 16-bit picture, they hand back 8-bit values.
QStringList eightBit(const QVariantList &stack);
}
