#pragma once
#include "audiosequence.h"
#include <QVariantMap>
#include <optional>

// Logical audio cuts. Their identity and rational time are independent of the
// covering video frames used by the existing timeline presentation.
namespace AudioTimeline {
using Time = AudioSequence::Time;
struct Cut { QVariantMap entry; Time start, in, duration; };
bool containsPreciseCuts(const QVariant &value);
bool uniqueIds(const QVariantList &entries);
QVariantMap entryWithId(const QVariantList &entries, const QString &id);
QString editState(QVariantMap entry);
bool sameEditContext(const QVariantMap &captured, const QVariantMap &current, bool legacy);
bool readTime(const QVariant &value, Time *time);
bool readSpan(const QVariantMap &entry, Time *start, Time *duration);
bool contains(const QVariantMap &entry, Time position);
bool startsAt(const QVariantMap &entry, Time position);
QVariantMap writeTime(Time time);
std::optional<Time> add(Time a, Time b);
std::optional<QList<Cut>> prepare(const QVariantList &entries, int numerator,
                                 int denominator, QString *error = nullptr);
QVariantMap timed(QVariantMap entry, Time start, Time in, Time duration,
                  int numerator, int denominator);
// Convert an ordinary frame-based sound cut without changing its selected span.
QVariantMap fromFrames(const QVariantMap &entry, int numerator, int denominator);
QVariantMap shifted(const QVariantMap &entry, Time delta, int numerator, int denominator);
QVariantList split(const QVariantMap &entry, Time position, int numerator, int denominator);
}
