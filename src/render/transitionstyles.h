#pragma once

// The transitions a cut can carry, by the name a clip stores in "mixStyle"
// ("" and "dissolve" are the cross dissolve), each as the engine draws it.
// One list for the renderer, the timeline's check and the pickers.
//
// A style may carry settings after its name, "name:key=value,key=value":
// "soft" (a wipe's edge, 0..100, how soft) and "colour" (Dip to Colour's,
// six hex digits). They ride in the one string so that every place a style
// already travels (projects, undo, copies, interchange) carries them too.

#include "engine.h"

#include <QColor>
#include <QCoreApplication>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <cmath>
#include <optional>
#include <vector>

namespace render {

// How a transition is drawn: by the engine's own transition, or by the
// renderer from the engine's other pieces. Blur: both pictures blurred, most
// half way, as they dissolve. Zoom: the first grows and fades over the next.
enum class TransitionMade { Engine, Blur, Zoom };

struct NamedTransition {
    const char *key;
    const char *name;  // as people read it (translated in the "Transitions" context)
    engine::TransitionStyle style;
    TransitionMade made = TransitionMade::Engine;
};

inline const std::vector<NamedTransition> &namedTransitions() {
    using T = engine::Transition;
    using M = engine::WipeMask;
    using S = engine::Side;
    // A wipe's edge travels the way its name says: Wipe Right brings the
    // next clip in from the left. Hard edges, as the first wipes had.
    const auto wipe = [](M mask) {
        engine::TransitionStyle s;
        s.type = T::Wipe;
        s.mask = mask;
        s.softness = 0.f;
        return s;
    };
    // Push: both pictures travel. Slide: the next one comes in over the old.
    const auto move = [](T type, S from) {
        engine::TransitionStyle s;
        s.type = type;
        s.side = from;
        return s;
    };
    const auto dip = [](float level) {
        engine::TransitionStyle s;
        s.type = T::FadeColour;
        s.colour = {level, level, level, 1.f};
        return s;
    };
    static const std::vector<NamedTransition> all{
        {"wipe-right", QT_TRANSLATE_NOOP("Transitions", "Wipe Right"), wipe(M::Left)},
        {"wipe-left", QT_TRANSLATE_NOOP("Transitions", "Wipe Left"), wipe(M::Right)},
        {"wipe-down", QT_TRANSLATE_NOOP("Transitions", "Wipe Down"), wipe(M::Top)},
        {"wipe-up", QT_TRANSLATE_NOOP("Transitions", "Wipe Up"), wipe(M::Bottom)},
        {"wipe-down-right", QT_TRANSLATE_NOOP("Transitions", "Diagonal Wipe Down Right"), wipe(M::TopLeft)},
        {"wipe-down-left", QT_TRANSLATE_NOOP("Transitions", "Diagonal Wipe Down Left"), wipe(M::TopRight)},
        {"wipe-up-right", QT_TRANSLATE_NOOP("Transitions", "Diagonal Wipe Up Right"), wipe(M::BottomLeft)},
        {"wipe-up-left", QT_TRANSLATE_NOOP("Transitions", "Diagonal Wipe Up Left"), wipe(M::BottomRight)},
        {"barn-vertical", QT_TRANSLATE_NOOP("Transitions", "Barn Doors Vertical"), wipe(M::BarnVertical)},
        {"barn-horizontal", QT_TRANSLATE_NOOP("Transitions", "Barn Doors Horizontal"), wipe(M::BarnHorizontal)},
        {"barn-diagonal", QT_TRANSLATE_NOOP("Transitions", "Barn Doors Diagonal"), wipe(M::BarnDiagonal)},
        {"iris", QT_TRANSLATE_NOOP("Transitions", "Iris"), wipe(M::Iris)},
        {"box", QT_TRANSLATE_NOOP("Transitions", "Box"), wipe(M::Box)},
        {"diamond", QT_TRANSLATE_NOOP("Transitions", "Diamond"), wipe(M::Diamond)},
        {"clock", QT_TRANSLATE_NOOP("Transitions", "Clock"), wipe(M::Clock)},
        {"blinds-horizontal", QT_TRANSLATE_NOOP("Transitions", "Blinds Horizontal"), wipe(M::BlindsHorizontal)},
        {"blinds-vertical", QT_TRANSLATE_NOOP("Transitions", "Blinds Vertical"), wipe(M::BlindsVertical)},
        {"checkerboard", QT_TRANSLATE_NOOP("Transitions", "Checkerboard"), wipe(M::Checkerboard)},
        {"push-left", QT_TRANSLATE_NOOP("Transitions", "Push Left"), move(T::Slide, S::Right)},
        {"push-right", QT_TRANSLATE_NOOP("Transitions", "Push Right"), move(T::Slide, S::Left)},
        {"push-up", QT_TRANSLATE_NOOP("Transitions", "Push Up"), move(T::Slide, S::Bottom)},
        {"push-down", QT_TRANSLATE_NOOP("Transitions", "Push Down"), move(T::Slide, S::Top)},
        {"slide-left", QT_TRANSLATE_NOOP("Transitions", "Slide Left"), move(T::Cover, S::Right)},
        {"slide-right", QT_TRANSLATE_NOOP("Transitions", "Slide Right"), move(T::Cover, S::Left)},
        {"dip-black", QT_TRANSLATE_NOOP("Transitions", "Dip to Black"), dip(0.f)},
        {"dip-white", QT_TRANSLATE_NOOP("Transitions", "Dip to White"), dip(1.f)},
        {"dip-colour", QT_TRANSLATE_NOOP("Transitions", "Dip to Colour"), dip(0.2158f)},  // mid grey until a colour is chosen
        {"blur-dissolve", QT_TRANSLATE_NOOP("Transitions", "Blur Dissolve"), engine::TransitionStyle{}, TransitionMade::Blur},
        {"zoom", QT_TRANSLATE_NOOP("Transitions", "Zoom"), engine::TransitionStyle{}, TransitionMade::Zoom},
    };
    return all;
}

// A style's name without its settings.
inline QString transitionBase(const QString &style) { return style.section(QLatin1Char(':'), 0, 0); }

// How the style of this name is drawn.
inline TransitionMade transitionMade(const QString &style) {
    const QString base = transitionBase(style);
    for (const NamedTransition &t : namedTransitions())
        if (base == QLatin1String(t.key)) return t.made;
    return TransitionMade::Engine;
}

// A style's settings as written after its name: {soft: 0..100, colour: "#rrggbb"}, each only if given.
inline QVariantMap transitionSettings(const QString &style) {
    QVariantMap out;
    const QString rest = style.section(QLatin1Char(':'), 1);
    for (const QString &part : rest.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString key = part.section(QLatin1Char('='), 0, 0), value = part.section(QLatin1Char('='), 1);
        if (key == QLatin1String("soft")) out.insert(key, value.toInt());
        else if (key == QLatin1String("colour")) out.insert(key, QStringLiteral("#") + value.toLower());
        else out.insert(key, value);
    }
    return out;
}

// The style for a clip's "mixStyle"; nothing for a name that is not known,
// or for settings the style does not take or cannot read.
inline std::optional<engine::TransitionStyle> transitionStyle(const QString &name) {
    if (name.isEmpty() || name == QLatin1String("dissolve")) return engine::TransitionStyle{};
    const QString base = transitionBase(name);
    for (const NamedTransition &t : namedTransitions()) {
        if (base != QLatin1String(t.key)) continue;
        engine::TransitionStyle style = t.style;
        if (!name.contains(QLatin1Char(':'))) return style;
        const QString rest = name.section(QLatin1Char(':'), 1);
        if (rest.isEmpty()) return std::nullopt;
        for (const QString &part : rest.split(QLatin1Char(','))) {
            const QString key = part.section(QLatin1Char('='), 0, 0), value = part.section(QLatin1Char('='), 1);
            bool ok = false;
            if (key == QLatin1String("soft") && style.type == engine::Transition::Wipe) {
                const int soft = value.toInt(&ok);
                if (!ok || soft < 0 || soft > 100) return std::nullopt;
                style.softness = float(soft) / 200.f;  // up to half the wipe's travel
            } else if (key == QLatin1String("colour") && base == QLatin1String("dip-colour")) {
                const QColor c(QStringLiteral("#") + value);
                if (value.size() != 6 || !c.isValid()) return std::nullopt;
                const auto light = [](double v) { return float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4)); };
                style.colour = {light(c.redF()), light(c.greenF()), light(c.blueF()), 1.f};
            } else return std::nullopt;
        }
        return style;
    }
    return std::nullopt;
}

// [{key, name, soft, colour}] for a picker, the cross dissolve first (its
// key is ""); soft and colour say which settings the style takes.
inline QVariantList transitionChoices() {
    QVariantList out{QVariantMap{{"key", QString()}, {"name", QCoreApplication::translate("Transitions", "Cross Dissolve")}, {"soft", false}, {"colour", false}}};
    for (const NamedTransition &t : namedTransitions())
        out << QVariantMap{{"key", QString::fromLatin1(t.key)}, {"name", QCoreApplication::translate("Transitions", t.name)},
                           {"soft", t.style.type == engine::Transition::Wipe}, {"colour", QLatin1String(t.key) == QLatin1String("dip-colour")}};
    return out;
}

}  // namespace render
