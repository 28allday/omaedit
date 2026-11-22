#pragma once

// A clip's video effects (its "videoEffects" stack, VideoEffects' catalogue)
// as engine steps. Each catalogue effect the engine draws becomes one step:
// an engine effect with the controls as the clip stores them, or one of the
// engine's own steps where it already does the job (blur, sharpen, shadow,
// keys). Bypassed effects are left out.

#include "engine.h"

#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <optional>
#include <vector>

namespace render {
// The stack's key, when it comes before every other active effect: such a key
// works on the clip's picture as filmed and at its own size (see clipLayer).
std::optional<QVariantMap> keyFirst(const QVariantList &stack);
// Empty when every active effect can be drawn; otherwise the first that cannot.
QString effectsUnsupported(const QVariantList &stack);
// The steps for the active effects in stack order, at the clip's frame.
// `frame`: the clip's frame (an effect's own clock adds to it, kept across
// cuts; a control with keys takes its value at this frame); `scale`: the picture drawn against the one it stands for.
std::vector<engine::VideoFilter> effectSteps(const QVariantList &stack, int frame, double scale = 1.0);
bool hasActiveEffects(const QVariantList &stack);
// Whether any effect's control has keys: the clip's look then depends on how
// far into the clip a frame is.
bool effectsAnimated(const QVariantList &stack);
}
