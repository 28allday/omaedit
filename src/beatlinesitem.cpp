#include "beatlinesitem.h"

#include <QHash>
#include <QQuickWindow>
#include <QSGNode>
#include <QSGRectangleNode>
#include <QtQml/qqml.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace {
void registerBeatLinesItem() { qmlRegisterType<BeatLinesItem>("omaedit", 1, 0, "BeatLinesItem"); }
Q_COREAPP_STARTUP_FUNCTION(registerBeatLinesItem)
}

BeatLinesItem::BeatLinesItem(QQuickItem *parent) : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    // Looked at, never pressed: a click on a line is a click on what lies under it.
    setAcceptedMouseButtons(Qt::NoButton);
    setAcceptHoverEvents(false);
    setAcceptTouchEvents(false);
}

void BeatLinesItem::changed() {
    m_stale = true;
    polish();
    update();
}

void BeatLinesItem::setLines(const QVariantList &lines) {
    m_lines = lines;
    // By clip: each is thinned by the spacing of its own lines.
    m_clips.clear();
    QHash<QPair<int, int>, int> place;
    for (const QVariant &value : lines) {
        const QVariantMap line = value.toMap();
        const int track = line.value(QStringLiteral("track")).toInt();
        const QPair<int, int> clip{track, line.value(QStringLiteral("index")).toInt()};
        auto at = place.constFind(clip);
        if (at == place.constEnd()) {
            at = place.insert(clip, int(m_clips.size()));
            m_clips.append(Clip{});
            m_clips.last().track = track;
        }
        m_clips[*at].lines.append({line.value(QStringLiteral("frame")).toInt(),
                                   std::clamp(line.value(QStringLiteral("level")).toInt(), 0, kLevels - 1),
                                   line.value(QStringLiteral("bar")).toBool()});
    }
    for (Clip &clip : m_clips) {
        std::sort(clip.lines.begin(), clip.lines.end(), [](const Line &a, const Line &b) { return a.frame < b.frame; });
        std::array<int, kLevels> last;
        last.fill(std::numeric_limits<int>::min());
        clip.gap.fill(std::numeric_limits<double>::infinity());
        for (const Line &line : std::as_const(clip.lines)) {
            clip.top = std::max(clip.top, line.level);
            for (int level = 0; level <= line.level; ++level) {
                if (clip.held[size_t(level)]++ > 0)
                    clip.gap[size_t(level)] = std::min(clip.gap[size_t(level)], double(line.frame) - last[size_t(level)]);
                last[size_t(level)] = line.frame;
            }
        }
    }
    emit linesChanged();
    changed();
}

void BeatLinesItem::setRows(const QVariantList &rows) {
    m_rows = rows;
    m_rowList.clear();
    for (const QVariant &value : rows) {
        const QVariantMap row = value.toMap();
        m_rowList.append({row.value(QStringLiteral("track")).toInt(), row.value(QStringLiteral("y")).toDouble(),
                          row.value(QStringLiteral("height")).toDouble()});
    }
    emit rowsChanged();
    update();
}

void BeatLinesItem::setFirstFrame(double frame) {
    if (qFuzzyCompare(m_firstFrame + 1, frame + 1)) return;
    m_firstFrame = frame;
    emit viewChanged();
    changed();
}

void BeatLinesItem::setPxPerFrame(double px) {
    if (qFuzzyCompare(m_pxPerFrame, px)) return;
    m_pxPerFrame = px;
    emit viewChanged();
    changed();
}

void BeatLinesItem::setSpanTop(double y) {
    if (m_spanTop == y) return;
    m_spanTop = y;
    emit viewChanged();
    update();
}

void BeatLinesItem::setSpanBottom(double y) {
    if (m_spanBottom == y) return;
    m_spanBottom = y;
    emit viewChanged();
    update();
}

void BeatLinesItem::setColor(const QColor &color) {
    if (m_color == color) return;
    m_color = color;
    emit lookChanged();
    update();
}

void BeatLinesItem::setLineWidth(double width) {
    if (m_lineWidth == width) return;
    m_lineWidth = width;
    emit lookChanged();
    update();
}

void BeatLinesItem::setBarWidth(double width) {
    if (m_barWidth == width) return;
    m_barWidth = width;
    emit lookChanged();
    update();
}

void BeatLinesItem::setSpacing(double spacing) {
    if (m_spacing == spacing) return;
    m_spacing = spacing;
    emit lookChanged();
    changed();
}

void BeatLinesItem::setBeatOpacity(double opacity) {
    if (m_beatOpacity == opacity) return;
    m_beatOpacity = opacity;
    emit lookChanged();
    update();
}

void BeatLinesItem::setBarOpacity(double opacity) {
    if (m_barOpacity == opacity) return;
    m_barOpacity = opacity;
    emit lookChanged();
    update();
}

void BeatLinesItem::setCap(const QSizeF &cap) {
    if (m_cap == cap) return;
    m_cap = cap;
    emit lookChanged();
    update();
}

void BeatLinesItem::geometryChange(const QRectF &now, const QRectF &before) {
    QQuickItem::geometryChange(now, before);
    if (now.size() != before.size()) changed();
}

int BeatLinesItem::count() { lay(); return int(m_shown.size()); }
int BeatLinesItem::level() { lay(); return m_level; }

QVariantList BeatLinesItem::shown() {
    lay();
    QVariantList out;
    out.reserve(m_shown.size());
    for (const Shown &line : std::as_const(m_shown)) {
        QVariantList tracks;
        for (const int track : line.tracks) tracks.append(track);
        out.append(QVariantMap{{QStringLiteral("x"), line.x}, {QStringLiteral("frame"), line.frame}, {QStringLiteral("level"), line.level},
                               {QStringLiteral("bar"), line.bar}, {QStringLiteral("strong"), line.strong}, {QStringLiteral("tracks"), tracks}});
    }
    return out;
}

void BeatLinesItem::updatePolish() { lay(); }

// Which lines are drawn, and where. Only the lines in view are looked at
// (each clip's are in order, so the first is found by halving), so a long
// piece of music costs what is on screen and no more.
void BeatLinesItem::lay() {
    if (!m_stale) return;
    m_stale = false;
    const int before = int(m_shown.size()), levelBefore = m_level;
    QVector<Shown> found;
    if (m_pxPerFrame > 0 && width() > 0) {
        const double reach = std::max(m_lineWidth, m_barWidth) / m_pxPerFrame;
        const double first = m_firstFrame - reach, last = m_firstFrame + width() / m_pxPerFrame + reach;
        const auto x = [&](int frame) { return std::round((frame - m_firstFrame) * m_pxPerFrame); };
        for (const Clip &clip : std::as_const(m_clips)) {
            if (clip.lines.isEmpty() || clip.lines.last().frame < first || clip.lines.first().frame > last) continue;
            // The finest level whose lines stand `spacing` apart.
            int level = -1;
            for (int l = 0; l <= clip.top && level < 0; ++l)
                if (clip.held[size_t(l)] == 1 || clip.gap[size_t(l)] * m_pxPerFrame >= m_spacing) level = l;
            const auto from = std::lower_bound(clip.lines.cbegin(), clip.lines.cend(), first, [](const Line &line, double frame) { return line.frame < frame; });
            if (level < 0) {
                // Even its greatest lines would run together (a sliver of a
                // clip): one of them, the first in view if any is.
                const Line *one = nullptr;
                for (auto line = from; line != clip.lines.cend() && line->frame <= last && !one; ++line)
                    if (line->level == clip.top) one = &*line;
                for (auto line = clip.lines.cbegin(); line != clip.lines.cend() && !one; ++line)
                    if (line->level == clip.top) one = &*line;
                if (one && one->frame >= first && one->frame <= last)
                    found.append({x(one->frame), one->frame, one->level, one->bar, one->bar, {clip.track}});
                continue;
            }
            for (auto line = from; line != clip.lines.cend() && line->frame <= last; ++line)
                if (line->level >= level) found.append({x(line->frame), line->frame, line->level, line->bar, line->bar && line->level > level, {clip.track}});
        }
    }
    // Lines of different clips (two pieces of music, the pieces of one cut
    // up) keep the same distance: the greater line stays, and a line on the
    // very place of another gives it its clip.
    std::stable_sort(found.begin(), found.end(), [](const Shown &a, const Shown &b) { return a.level != b.level ? a.level > b.level : a.x < b.x; });
    std::map<double, int> kept; // x -> its place in m_shown
    m_shown.clear();
    m_level = -1;
    for (const Shown &line : std::as_const(found)) {
        const auto after = kept.lower_bound(line.x);
        if (after != kept.end() && after->first == line.x) {
            Shown &same = m_shown[after->second];
            same.strong = same.strong || line.strong;
            for (const int track : line.tracks)
                if (!same.tracks.contains(track)) same.tracks.append(track);
            continue;
        }
        if (after != kept.end() && after->first - line.x < m_spacing) continue;
        if (after != kept.begin() && line.x - std::prev(after)->first < m_spacing) continue;
        kept.insert(after, {line.x, int(m_shown.size())});
        m_shown.append(line);
        m_level = m_level < 0 ? line.level : std::min(m_level, line.level);
    }
    std::sort(m_shown.begin(), m_shown.end(), [](const Shown &a, const Shown &b) { return a.x < b.x; });
    if (before != int(m_shown.size()) || levelBefore != m_level) emit shownChanged();
}

// A few plain rectangles (they draw the same on the software renderer and on
// the GPU): each line down the tracks, the stretch of it on its own clip's
// row, and a bar's cap. They are made afresh each time, under a new node: the
// software renderer places a node by where its parent was when it was added,
// and one added later to a node already drawn is placed at the window's corner.
QSGNode *BeatLinesItem::updatePaintNode(QSGNode *old, UpdatePaintNodeData *) {
    lay();
    delete old;
    if (m_shown.isEmpty()) return nullptr;
    QSGNode *node = new QSGNode;
    const auto rect = [&](QRectF area, const QColor &colour) {
        area &= QRectF(0, 0, width(), height());
        if (area.width() <= 0 || area.height() <= 0) return;
        QSGRectangleNode *made = window()->createRectangleNode();
        made->setRect(area);
        made->setColor(colour);
        made->setFlag(QSGNode::OwnedByParent);
        node->appendChildNode(made);
    };
    const double top = std::max(0., m_spanTop), bottom = m_spanBottom < 0 ? height() : std::min(height(), m_spanBottom);
    const auto toned = [&](double opacity) {
        QColor colour = m_color;
        colour.setAlphaF(float(std::clamp(m_color.alphaF() * opacity, 0., 1.)));
        return colour;
    };
    const QColor beat = toned(m_beatOpacity), bar = toned(m_barOpacity), solid = toned(1);
    // Each line on whole device pixels: at a fractional scale (1.6) a width
    // or a place of whole pixels of the item is not one of the screen's, and
    // the line would be drawn blurred, half into the pixels beside it. Its
    // width is made the nearest whole number of the screen's pixels (one at
    // least) and its left edge the screen pixel nearest where it falls.
    const double scale = window() ? window()->effectiveDevicePixelRatio() : 1.;
    const QPointF origin = mapToScene(QPointF(0, 0));
    const auto across = [&](double middle, double width) {
        const double device = std::max(1., std::round(width * scale));
        const double left = std::round((origin.x() + middle) * scale) - std::floor(device / 2);
        return std::pair<double, double>{left / scale - origin.x(), device / scale};
    };
    const auto down = [&](double y) { return std::round((origin.y() + y) * scale) / scale - origin.y(); };
    for (const Shown &line : std::as_const(m_shown)) {
        const auto [left, wide] = across(line.x, line.strong ? m_barWidth : m_lineWidth);
        rect(QRectF(left, top, wide, bottom - top), line.strong ? bar : beat);
        for (const int track : line.tracks)
            for (const Row &row : std::as_const(m_rowList)) {
                if (row.track != track) continue;
                const double from = std::max(top, row.y), to = std::min(bottom, row.y + row.height);
                if (to <= from) break;
                rect(QRectF(left, from, wide, to - from), solid);
                if (line.bar && row.y >= top) {
                    const auto [capLeft, capWide] = across(line.x, m_cap.width());
                    const double capTop = down(row.y);
                    rect(QRectF(capLeft, capTop, capWide, down(row.y + std::min(m_cap.height(), to - from)) - capTop), solid);
                }
                break;
            }
    }
    return node;
}
