#pragma once
#include <QVariantMap>

namespace SourceRateEdit {
// Pure preparation for an explicit range-preserving source-rate edit. Inputs
// remain untouched on failure. The existing metadata-only mode is separate.
// The result contains valid/error, entries and primary ripple events.
// Primary duration changes move later clips on eligible tracks. Independent
// secondary duration changes ripple their lane; linked sound stays anchored to
// its picture. Overlaps, locked changes, offset linked trims and nested media
// are refused. This does not apply a project transaction or rewrite history.
QVariantMap prepare(const QVariantList &entries, const QVariantMap &previousSources,
                    const QVariantMap &sources, double fps,
                    const QVariantList &tracks = {});
}
