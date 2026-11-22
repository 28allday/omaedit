#pragma once

#include <QCoreApplication>
#include <QVariantList>
#include <QVariantMap>

// What a clip is for: Dialogue, Music, Effects, Video, Titles and any the
// project adds. A clip's picture and its sound are separate entries, so each
// has its own. An entry with no category, or one since removed from the
// project, reads as the default for what it is.
namespace ClipCategories {
inline QVariantList builtIn() {
    const auto row = [](const char *id, const char *name, const char *kind, const char *color) {
        return QVariant(QVariantMap{{"id", id}, {"name", QCoreApplication::translate("ClipCategories", name)},
                                    {"kind", kind}, {"color", color}, {"builtIn", true}});
    };
    return {row("dialogue", QT_TRANSLATE_NOOP("ClipCategories", "Dialogue"), "audio", "blue"),
            row("music", QT_TRANSLATE_NOOP("ClipCategories", "Music"), "audio", "green"),
            row("effects", QT_TRANSLATE_NOOP("ClipCategories", "Effects"), "audio", "purple"),
            row("video", QT_TRANSLATE_NOOP("ClipCategories", "Video"), "video", "yellow"),
            row("titles", QT_TRANSLATE_NOOP("ClipCategories", "Titles"), "video", "orange")};
}
inline QString defaultFor(const QVariantMap &entry) {
    if (entry.value("audioOnly").toBool())
        return QStringLiteral("dialogue");
    return entry.value("title").toString().isEmpty() ? QStringLiteral("video") : QStringLiteral("titles");
}
inline QString effective(const QVariantMap &entry, const QVariantList &categories) {
    const QString id = entry.value("category").toString();
    const QString kind = entry.value("audioOnly").toBool() ? QStringLiteral("audio") : QStringLiteral("video");
    if (!id.isEmpty())
        for (const QVariant &v : categories)
            if (v.toMap().value("id").toString() == id && v.toMap().value("kind").toString() == kind)
                return id;
    return defaultFor(entry);
}
// Stems by category alone tell a sound with no category from Dialogue: it
// goes to a stem of its own, under this id (no category's: a built-in's is
// its plain word, a project's own is made up by the catalogue). Everywhere
// else it reads as Dialogue, as `effective` says.
inline QString uncategorisedStem() { return QStringLiteral("uncategorised"); }
inline QString stemOf(const QVariantMap &entry, const QVariantList &categories) {
    const QString id = entry.value("category").toString();
    if (!id.isEmpty())
        for (const QVariant &v : categories)
            if (v.toMap().value("id").toString() == id && v.toMap().value("kind").toString() == QLatin1String("audio"))
                return id;
    return uncategorisedStem();
}
} // namespace ClipCategories
