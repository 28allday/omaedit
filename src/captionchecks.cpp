#include "captionchecks.h"

#include <QCoreApplication>
#include <QMap>
#include <QSet>
#include <algorithm>

namespace {
// A number as the window shows it: no more places than it has.
QString number(double value, int places = 2) {
    QString text = QString::number(value, 'f', places);
    if (text.contains(QLatin1Char('.'))) {
        while (text.endsWith(QLatin1Char('0'))) text.chop(1);
        if (text.endsWith(QLatin1Char('.'))) text.chop(1);
    }
    return text;
}
constexpr double kSlack = 1e-6; // a caption exactly at a limit keeps to it
} // namespace

namespace CaptionChecks {

QVariantMap defaultRules() {
    return {{"charsPerSecond", 17.}, {"lineLetters", 42}, {"lines", 2}, {"shortestSeconds", 0.83},
            {"longestSeconds", 7.}, {"gapFrames", 2}, {"noOverlap", true},
            {"twoSpeakers", false}, {"leftSpeaker", QCoreApplication::translate("CaptionChecks", "Speaker 1")}, {"rightSpeaker", QCoreApplication::translate("CaptionChecks", "Speaker 2")}};
}

QVariantMap rules(const QVariantMap &stored) {
    QVariantMap out = defaultRules();
    for (auto it = stored.begin(); it != stored.end(); ++it)
        if (out.contains(it.key())) out[it.key()] = it.value();
    return out;
}

bool applyRules(QVariantMap &rules, const QVariantMap &input, QString *why) {
    const auto refuse = [why](const QString &reason) { if (why) *why = reason; return false; };
    QVariantMap out = rules;
    const QVariantMap known = defaultRules();
    for (auto it = input.begin(); it != input.end(); ++it) {
        const QString &key = it.key();
        const QVariant &v = it.value();
        if (!known.contains(key)) return refuse(QCoreApplication::translate("CaptionChecks", "%1 is not a caption rule").arg(key));
        bool ok = false;
        const auto ranged = [&](double lo, double hi, bool whole) {
            double d = v.toDouble(&ok);
            ok = ok && v.metaType().id() != QMetaType::Bool && d >= lo && d <= hi;
            return whole ? double(qRound(d)) : d;
        };
        if (key == QLatin1String("noOverlap") || key == QLatin1String("twoSpeakers")) {
            if (v.metaType().id() != QMetaType::Bool) return refuse(QCoreApplication::translate("CaptionChecks", "%1 is true or false").arg(key));
            out[key] = v.toBool();
        } else if (key == QLatin1String("leftSpeaker") || key == QLatin1String("rightSpeaker")) {
            const QString name = v.toString().simplified();
            if (name.isEmpty() || name.size() > 80) return refuse(QCoreApplication::translate("CaptionChecks", "%1 is a name of 1 to 80 letters").arg(key));
            out[key] = name;
        } else if (key == QLatin1String("charsPerSecond")) {
            const double d = ranged(1, 60, false);
            if (!ok) return refuse(QCoreApplication::translate("CaptionChecks", "charsPerSecond is 1 to 60"));
            out[key] = d;
        } else if (key == QLatin1String("lineLetters")) {
            const double d = ranged(10, 120, true);
            if (!ok) return refuse(QCoreApplication::translate("CaptionChecks", "lineLetters is 10 to 120"));
            out[key] = int(d);
        } else if (key == QLatin1String("lines")) {
            const double d = ranged(1, 6, true);
            if (!ok) return refuse(QCoreApplication::translate("CaptionChecks", "lines is 1 to 6"));
            out[key] = int(d);
        } else if (key == QLatin1String("shortestSeconds")) {
            const double d = ranged(0, 10, false);
            if (!ok) return refuse(QCoreApplication::translate("CaptionChecks", "shortestSeconds is 0 to 10"));
            out[key] = d;
        } else if (key == QLatin1String("longestSeconds")) {
            const double d = ranged(0.5, 120, false);
            if (!ok) return refuse(QCoreApplication::translate("CaptionChecks", "longestSeconds is 0.5 to 120"));
            out[key] = d;
        } else if (key == QLatin1String("gapFrames")) {
            const double d = ranged(0, 100, true);
            if (!ok) return refuse(QCoreApplication::translate("CaptionChecks", "gapFrames is 0 to 100"));
            out[key] = int(d);
        }
    }
    if (out.value("longestSeconds").toDouble() < out.value("shortestSeconds").toDouble())
        return refuse(QCoreApplication::translate("CaptionChecks", "longestSeconds is less than shortestSeconds"));
    rules = out;
    return true;
}

QHash<int, QVariantList> problems(const QVariantList &rows, const QVariantMap &stored, double fps) {
    const QVariantMap r = rules(stored);
    const double rate = fps > 0 ? fps : 25.;
    const double cps = r.value("charsPerSecond").toDouble(), shortest = r.value("shortestSeconds").toDouble(),
                 longest = r.value("longestSeconds").toDouble();
    const int letters = r.value("lineLetters").toInt(), most = r.value("lines").toInt(), gapFrames = r.value("gapFrames").toInt();
    QHash<int, QVariantList> out;
    const auto add = [&out](int index, const char *kind, const QString &reason) {
        out[index].append(QVariantMap{{"kind", QString::fromLatin1(kind)}, {"reason", reason}});
    };
    struct Placed { int start, end, index; };
    QMap<int, QList<Placed>> tracks;
    for (const QVariant &v : rows) {
        const QVariantMap row = v.toMap();
        const int index = row.value("index").toInt(), frames = row.value("frames").toInt(), start = row.value("start").toInt();
        tracks[row.value("track").toInt()].append({start, start + frames, index});
        QStringList lines;
        for (const QString &line : row.value("text").toString().split(QLatin1Char('\n')))
            if (!line.trimmed().isEmpty()) lines.append(line.trimmed());
        int characters = 0, longestLine = 0;
        for (const QString &line : std::as_const(lines)) {
            characters += int(line.size());
            longestLine = qMax(longestLine, int(line.size()));
        }
        const double seconds = frames / rate;
        if (seconds > 0 && characters / seconds > cps + kSlack)
            add(index, "speed", QCoreApplication::translate("CaptionChecks", "Reads at %1 characters a second (most %2)").arg(number(characters / seconds, 1), number(cps)));
        if (longestLine > letters)
            add(index, "line", QCoreApplication::translate("CaptionChecks", "A line of %1 letters (most %2)").arg(longestLine).arg(letters));
        if (lines.size() > most)
            add(index, "lines", QCoreApplication::translate("CaptionChecks", "%n lines (most %1)", nullptr, int(lines.size())).arg(most));
        if (seconds < shortest - kSlack)
            add(index, "short", QCoreApplication::translate("CaptionChecks", "On screen %1 s (least %2 s)").arg(number(seconds), number(shortest)));
        if (seconds > longest + kSlack)
            add(index, "long", QCoreApplication::translate("CaptionChecks", "On screen %1 s (most %2 s)").arg(number(seconds), number(longest)));
    }
    for (auto it = tracks.begin(); it != tracks.end(); ++it) {
        QList<Placed> &list = it.value();
        std::sort(list.begin(), list.end(), [](const Placed &a, const Placed &b) { return a.start < b.start || (a.start == b.start && a.index < b.index); });
        for (int i = 0; i + 1 < list.size(); ++i) {
            const int gap = list[i + 1].start - list[i].end;
            if (gap < 0) {
                if (r.value("noOverlap").toBool()) add(list[i].index, "overlap", QCoreApplication::translate("CaptionChecks", "Overlaps the next caption on its track"));
            } else if (gap > 0 && gap < gapFrames) {
                add(list[i].index, "gap", QCoreApplication::translate("CaptionChecks", "%n frame(s) before the next caption (least %1)", nullptr, gap).arg(gapFrames));
            }
        }
    }
    return out;
}

QStringList summary(const QHash<int, QVariantList> &problems, const QVariantMap &stored) {
    const QVariantMap r = rules(stored);
    QMap<QString, int> counts;
    for (const QVariantList &list : problems) {
        QSet<QString> kinds;
        for (const QVariant &p : list) kinds.insert(p.toMap().value("kind").toString());
        for (const QString &kind : std::as_const(kinds)) ++counts[kind];
    }
    QStringList out;
    const auto n = [&counts](const char *kind) { return counts.value(QString::fromLatin1(kind)); };
    if (n("speed"))
        out.append(QCoreApplication::translate("CaptionChecks", "%n caption(s) read faster than %1 characters a second", nullptr, n("speed"))
                       .arg(number(r.value("charsPerSecond").toDouble())));
    if (n("line"))
        out.append(QCoreApplication::translate("CaptionChecks", "%n caption(s) have a line longer than %1 letters", nullptr, n("line"))
                       .arg(r.value("lineLetters").toInt()));
    if (n("lines"))
        out.append(QCoreApplication::translate("CaptionChecks", "%n caption(s) have more than %1 lines", nullptr, n("lines"))
                       .arg(r.value("lines").toInt()));
    if (n("short"))
        out.append(QCoreApplication::translate("CaptionChecks", "%n caption(s) are on screen for less than %1 s", nullptr, n("short"))
                       .arg(number(r.value("shortestSeconds").toDouble())));
    if (n("long"))
        out.append(QCoreApplication::translate("CaptionChecks", "%n caption(s) are on screen for more than %1 s", nullptr, n("long"))
                       .arg(number(r.value("longestSeconds").toDouble())));
    if (n("gap"))
        out.append(QCoreApplication::translate("CaptionChecks", "%n caption(s) leave less than %1 frames before the next", nullptr, n("gap"))
                       .arg(r.value("gapFrames").toInt()));
    if (n("overlap"))
        out.append(QCoreApplication::translate("CaptionChecks", "%n caption(s) overlap the next on their track", nullptr, n("overlap")));
    return out;
}

QStringList palette() {
    return {QStringLiteral("#61afef"), QStringLiteral("#e5c07b"), QStringLiteral("#98c379"), QStringLiteral("#e06c75"),
            QStringLiteral("#c678dd"), QStringLiteral("#56b6c2"), QStringLiteral("#d19a66"), QStringLiteral("#ff79c6")};
}

QString colourFor(const QString &name) {
    // FNV-1a over the name's letters: the same colour on every run.
    quint32 hash = 2166136261u;
    for (const QChar c : name) { hash ^= c.unicode(); hash *= 16777619u; }
    const QStringList colours = palette();
    return colours.at(int(hash % quint32(colours.size())));
}

} // namespace CaptionChecks
