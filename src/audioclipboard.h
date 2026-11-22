#pragma once
#include "audiotimeline.h"
#include <QMap>
#include <QHash>

namespace AudioClipboard {
using Time = AudioTimeline::Time;
struct Span { Time start, duration; };
struct Placement {
    QMap<int,QVariantList> tracks;
    QVariantList added;
    QHash<QString,QString> links;
    Time duration;
};
// Prepare immutable sound-track replacements. The caller validates track locks,
// prepares every render graph and commits them together. Insert affects exactly
// the supplied destination tracks; global picture/linked ripple is a caller's
// transaction. A shared group span lets mixed-media callers retain one origin.
std::optional<Placement> prepare(const QVariantList &copies,
    const QMap<int,QVariantList> &destinations, int sourceNum, int sourceDen,
    int destinationNum, int destinationDen, Time position, bool insert,
    QString *error = nullptr, std::optional<Span> group = {});
}
