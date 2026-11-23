#pragma once
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// The caption checks: how fast a caption reads, how long its lines are,
// how many it has, how long it stays on screen, the gap to the next and an
// overlap on its track; and the colours speakers are shown in. Plain
// functions of a timeline's captions and its rules, kept with the timeline
// (Timeline::captionSettings "rules": only the keys set).
namespace CaptionChecks {
// Every rule with its default: charsPerSecond 17, lineLetters 42, lines 2,
// shortestSeconds 0.83, longestSeconds 7, gapFrames 2 (a gap of none is
// fine: one caption straight after another), noOverlap true; and for
// Generate Captions twoSpeakers false (each channel of a stereo clip one
// speaker: leftSpeaker, rightSpeaker).
QVariantMap defaultRules();
// The rules as stored, over the defaults.
QVariantMap rules(const QVariantMap &stored);
// `input`'s keys checked and set on `rules`; false with the reason, and
// nothing set, when a key or a value is not one.
bool applyRules(QVariantMap &rules, const QVariantMap &input, QString *why);
// The problems of each caption: rows [{index, track, start, frames, text}]
// (frames at `fps`), by index [{kind, reason}]. Kinds: speed, line, lines,
// short, long, gap, overlap.
QHash<int, QVariantList> problems(const QVariantList &rows, const QVariantMap &rules, double fps);
// One line for each kind found, for the Export page's warnings.
QStringList summary(const QHash<int, QVariantList> &problems, const QVariantMap &rules);
// The colours a speaker can be shown in, and the one a name takes when it
// has none of its own (always the same for a name).
QStringList palette();
QString colourFor(const QString &name);
}
