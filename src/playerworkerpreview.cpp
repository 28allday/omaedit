#include <QElapsedTimer>
#include "playerworker.h"
#include "previewrendercache.h"
#include "renderrequest.h"
#include "clipentry.h"
#include "projectfile.h"
#include <QFileInfo>

void PlayerWorker::cancelFrame() {
    m_frameGeneration.fetch_add(1,std::memory_order_relaxed);
    m_viewerAsked->fetch_add(1,std::memory_order_relaxed); // a filmstrip tile being drawn gives way (makeTiles)
}
std::shared_ptr<RenderRequest> PlayerWorker::frameRequest() {
    const auto generation=m_frameGeneration.load(std::memory_order_relaxed);
    return std::make_shared<RenderRequest>([this,generation] {
        return generation!=m_frameGeneration.load(std::memory_order_relaxed);
    });
}

void PlayerWorker::invalidateBackgroundRender() {
    m_backgroundRenderDelay.stop();
    if (m_backgroundRenderEnabled && !m_timeline.isEmpty()) {
        // The pictures are out of use at once; the render that starts in a
        // moment keeps those the edit left alone.
        m_backgroundRender->invalidate();
        m_backgroundRenderDelay.start();
    } else {
        m_backgroundRender->clear(); // no render follows: nothing to hold frames for
    }
}
void PlayerWorker::picturesMayHaveChanged() {
    // Every announce comes here, and most change no picture: a label, a
    // marker, a track's name, lock or fader, a timeline's name. Those keep
    // the render and its finished frames; only an edit that changes what a
    // frame shows starts it again.
    if (m_mixAnnounce)
        return;
    if (!m_backgroundRenderEnabled || m_timeline.isEmpty()) {
        m_backgroundRenderOf.clear();
        invalidateBackgroundRender();
        return;
    }
    QVariantMap snapshot{{"entries", m_timeline.entries()}, {"tracks", m_timeline.trackFlagsVariant()},
                         {"project", m_timeline.projectSettings()}, {"mediaSources", m_timeline.mediaSources()},
                         {"proxyMode", m_timeline.proxyMode()}, {"frames", m_timeline.frameCount()}};
    // The profile, and each nested timeline's file as it stands: a nest
    // edited since is another picture under the same entry.
    QVariantList shape{m_timeline.width(), m_timeline.height(), m_timeline.fps()};
    for (const QVariant &v : snapshot.value("entries").toList()) {
        const QString path = v.toMap().value(ClipKey::path).toString();
        if (!ProjectFile::isProject(path)) continue;
        const QFileInfo file(path);
        shape << path << file.size() << file.lastModified().toMSecsSinceEpoch();
    }
    snapshot.insert("profile", shape);
    if (!m_backgroundRenderOf.isEmpty() && PreviewRenderCache::samePictures(snapshot, m_backgroundRenderOf)) return;
    m_backgroundRenderOf = snapshot;
    invalidateBackgroundRender();
}
void PlayerWorker::updateBackgroundRenderBusy() {
    m_backgroundRender->setBusy(m_speed != 0 || exportActive() || bool(m_render));
    // Filmstrip tiles wait as well (processStripTile asks for none meanwhile).
    m_tilesHeld->store(m_speed != 0 || exportActive());
}
void PlayerWorker::setBackgroundRenderEnabled(bool enabled) {
    m_backgroundRenderEnabled = enabled;
    invalidateBackgroundRender();
}
void PlayerWorker::setBackgroundRenderLimit(int gib) {
    m_backgroundRenderLimit = qBound(1, gib, 200);
    invalidateBackgroundRender();
}
void PlayerWorker::setBackgroundRenderProcessing(const QString &preference) {
    if ((preference != "auto" && preference != "cpu") || preference == m_backgroundRenderProcessing) return;
    m_backgroundRenderProcessing = preference;
    invalidateBackgroundRender();
}
void PlayerWorker::backgroundRenderAction(const QString &action) {
    if (action == "clear") {
        m_backgroundRenderDelay.stop();
        m_backgroundRender->clear(); // the editor's own word: the frames go
        m_backgroundRenderOf.clear(); // the next announce starts it again, whatever it changed
    } else if (action == "retry") invalidateBackgroundRender();
    else m_backgroundRender->act(action);
}
MediaPlayer::Decoded PlayerWorker::decodePlaybackFrame(int position, bool audio) {
    MediaPlayer::Decoded decoded;
    // Playing, a rendered section's frames are taken from whichever is the
    // quicker there: the kept picture, or the frame drawn again (light work
    // at a reduced playback size is drawn faster than a kept picture is read).
    PlayCost *cost = nullptr;
    const bool draw = drawRatherThanKept(position, &cost);
    const auto mean = [](double &was, double now) { was = was < 0 ? now : was + (now - was) * 0.25; };
    QElapsedTimer timer;
    timer.start();
    if (!draw && cachedPlaybackFrame(position, audio, &decoded)) {
        if (cost) { mean(cost->keptMs, double(timer.nsecsElapsed()) / 1e6); ++cost->kept; }
        return decoded;
    }
    decoded = m_timeline.decodeAt(position, audio, 48000, 2);
    if (cost && draw) {
        const double took = double(timer.nsecsElapsed()) / 1e6;
        if (cost->trying > 0) {
            // A first try: the quickest of three, the first of which opens the clip.
            cost->drawnMs = cost->drawnMs < 0 ? took : std::min(cost->drawnMs, took);
            // Far slower at once: no more of it.
            if (took > cost->keptMs * 4 + 20) cost->trying = 1;
            if (--cost->trying == 0) cost->draw = cost->drawnMs < cost->keptMs * 0.75;
        } else {
            mean(cost->drawnMs, took);
            if (cost->drawnMs > cost->keptMs) {
                // No longer the quicker: the kept pictures again, and one more try later.
                cost->draw = false;
                if (cost->tries < 2) { cost->drawnMs = -1; cost->kept = -60; }
            }
        }
    }
    return decoded;
}

bool PlayerWorker::drawRatherThanKept(int position, PlayCost **cost) {
    *cost = nullptr;
    // Only while playing (a picture made for the viewer), and only where a picture is kept.
    if (!m_backgroundRenderEnabled || !m_timeline.shownAs().isValid() || m_timeline.scopesAs().isValid()) return false;
    QString render;
    int range = 0;
    double made = -1;
    if (!m_backgroundRender->about(position, &render, &range, &made)) return false;
    const QSize size = m_timeline.drawSize();
    const QString of = render + QLatin1Char('/') + QString::number(size.width()) + QLatin1Char('x') + QString::number(size.height());
    if (of != m_playCostsOf) { m_playCosts.clear(); m_playCostsOf = of; }
    PlayCost &c = m_playCosts[range];
    *cost = &c;
    // Frames the background render took much longer over than those drawing
    // was tried on are heavier work: never drawn while playing.
    // (A frame whose making was not timed goes as its neighbours do.)
    const bool alike = made < 0 || (c.madeMs >= 0 && made <= c.madeMs * 2.5 + 10);
    if (c.trying > 0 || c.draw) {
        if (alike) return true;
        c.trying = 0;
        c.draw = false;
        return false;
    }
    // Not tried yet: once the kept pictures have been timed, and only where
    // the background render made the frame quickly (heavy work is what the
    // kept pictures are for, and trying it would stall playback).
    if (c.drawnMs < 0 && c.kept >= 3 && made >= 0 && made <= 150) {
        c.madeMs = made;
        c.trying = 3;
        ++c.tries;
        return true;
    }
    // Tried and slower: the kept pictures, until the render, the size or the display changes.
    return false;
}
