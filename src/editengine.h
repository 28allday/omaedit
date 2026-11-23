#pragma once
#include <QImage>
#include <QVariantMap>
namespace edit {
class Profile;
class Producer;
} // namespace edit

// Clip-local animation shared by preview, nested sequences and export.
namespace EditEngine {
QStringList parameters(const QString &group);
double valueAt(const QVariantList &keys, int frame, double fallback, const char *valueName = "value");
bool normalizeOpacityKeys(const QVariantList &keys, QVariantList *result);
QVariantList shiftedCurve(const QVariantList &keys, int offset);
QVariantList scaledCurve(const QVariantList &keys, double ratio);
QVariantMap shiftedKeys(const QVariantMap &keys, int offset, int frames);
QVariantMap scaledKeys(const QVariantMap &keys, double ratio, int frames);
// A clip's video effects with their controls' keys moved or stretched the
// same way (each effect's "keys", in the clip's frames like the rest).
QVariantList shiftedEffects(const QVariantList &stack, int offset);
QVariantList scaledEffects(const QVariantList &stack, double ratio);
QVariantMap motionPreset(const QVariantMap &entry, const QString &preset,
                         const QVariantMap &options, double fps, QString *error);
QImage render(const QImage &source, const QVariantMap &config, int frame);
// The title's fuller look beyond the first keys, kept together on a title
// (ClipEntry::titleLook, a producer's typography): a drop shadow (shadow
// on/off, shadowColor #rrggbb or #aarrggbb, shadowOffset and shadowSoftness
// in pixels), the outline's outlineColor and outlineWidth (pixels; 2 when
// unset, the edge titles always had), and the box's boxPadding and
// boxRadius (pixels). Pixels are the timeline's frame's, scaled with it.
const QStringList &titleLookKeys();
// A title entry's whole style as the "title" group draws it: its title*
// keys, and its titleLook keys alongside.
QVariantMap titleStyleOfEntry(const QVariantMap &entry);
void attach(edit::Profile &profile, edit::Producer &cut, const QVariantMap &entry, int in, int out,
            bool includeStatic = false);
} // namespace EditEngine
