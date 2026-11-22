#include "gradeengine.h"
#include "clipentry.h"
#include "playerworker.h"
#include <QHash>
#include <QFileInfo>
#include <QUuid>

namespace {
QVariantMap linkOf(const QVariantMap &clip) { return clip.value(ClipKey::grade).toMap().value("sharing").toMap(); }
QVariantMap withLink(QVariantMap clip, const QVariantMap &link) {
    QVariantMap grade = clip.value(ClipKey::grade).toMap();
    if (grade.isEmpty())
        grade = Timeline::defaultGrade();
    grade["sharing"] = link;
    clip[ClipKey::grade] = grade;
    QVariantList versions = clip.value(ClipKey::gradeVersions).toList();
    for (QVariant &v : versions) {
        auto version = v.toMap();
        auto g = version.value("grade").toMap();
        g["sharing"] = link;
        version["grade"] = g;
        v = version;
    }
    clip[ClipKey::gradeVersions] = versions;
    return clip;
}
void copyLook(QVariantMap *destination, const QVariantMap &source) {
    for (const char *key : {"grade", "gradeVersions", "gradeVersion"}) {
        if (source.contains(key))
            (*destination)[key] = source.value(key);
        else
            destination->remove(key);
    }
}
bool video(const QVariantMap &e) {
    return e.contains(ClipKey::path) && !e.value(ClipKey::gap).toBool() && !e.value(ClipKey::audioOnly).toBool() &&
           e.value(ClipKey::title).toString().isEmpty();
}
} // namespace

QVariantMap PlayerWorker::inactiveColorSnapshots() const {
    QVariantMap snapshots;
    for (int i = 0; i < m_sequences.size(); ++i)
        if (i != m_activeSequence)
            snapshots[m_sequences[i].id] = m_sequences[i].snapshot;
    return snapshots;
}
QVariantList PlayerWorker::colorUndoSnapshot() {
    QVariantList snapshot = snapshotNow();
    auto sentinel = snapshot.last().toMap();
    sentinel["omavid:colorSequences"] = inactiveColorSnapshots();
    snapshot.last() = sentinel;
    return snapshot;
}
void PlayerWorker::restoreColorSnapshots(const QVariantMap &snapshots) {
    for (int i = 0; i < m_sequences.size(); ++i)
        if (i != m_activeSequence && snapshots.contains(m_sequences[i].id)) {
            m_sequences[i].snapshot = snapshots.value(m_sequences[i].id).toList();
            if (QFileInfo::exists(nestPath(m_sequences[i].id)))
                writeNest(m_sequences[i]);
        }
    announceSequences();
}

// Called by the common edit transaction, so LUTs, tracking, stills and version
// actions have the same sharing semantics and a single cross-sequence undo.
bool PlayerWorker::synchronizeColorLinks(const QVariantList &before) {
    const QVariantList after = m_timeline.entries();
    // The clips as they were, by where they sat. This runs after every edit
    // and looks each clip up; scanning the old list for every clip of the
    // new one took time with the square of the timeline's length. The
    // comparisons below are the ones the scan made, on the few clips that
    // share a place.
    QHash<quint64, QList<QVariantMap>> placed;
    const auto place = [](const QVariantMap &e) {
        return (quint64(quint32(e.value(ClipKey::track).toInt())) << 32) | quint32(e.value(ClipKey::index).toInt());
    };
    for (const QVariant &b : before) {
        const QVariantMap e = b.toMap();
        placed[place(e)].append(e);
    }
    QMap<QString, QVariantMap> updates;
    for (const QVariant &v : after) {
        QVariantMap current = v.toMap();
        if (!video(current))
            continue;
        QVariantMap previous;
        for (const QVariantMap &e : placed.value(place(current))) {
            if (e.value(ClipKey::track) == current.value(ClipKey::track) && e.value(ClipKey::index) == current.value(ClipKey::index) &&
                e.value(ClipKey::path) == current.value(ClipKey::path)) {
                previous = e;
                break;
            }
        }
        if (previous.isEmpty())
            continue;
        if (current.value(ClipKey::grade) == previous.value(ClipKey::grade) &&
            current.value(ClipKey::gradeVersions) == previous.value(ClipKey::gradeVersions) &&
            current.value(ClipKey::gradeVersion) == previous.value(ClipKey::gradeVersion))
            continue;
        auto grade = current.value(ClipKey::grade).toMap();
        auto link = grade.contains("sharing") ? linkOf(current) : linkOf(previous);
        if (link.value("id").toString().isEmpty())
            continue;
        current = withLink(current, link);
        updates[link.value("id").toString()] = current;
    }
    if (updates.isEmpty())
        return false;
    for (const QVariant &v : after) {
        auto target = v.toMap();
        const QString id = linkOf(target).value("id").toString();
        // A paste of an independent grade retains the destination membership.
        QString match = id;
        if (match.isEmpty() && !target.value(ClipKey::grade).toMap().contains("sharing"))
            for (const QVariantMap &e : placed.value(place(target))) {
                if (e.value(ClipKey::track) == target.value(ClipKey::track) && e.value(ClipKey::index) == target.value(ClipKey::index))
                    match = linkOf(e).value("id").toString();
            }
        if (!updates.contains(match))
            continue;
        const auto source = updates.value(match);
        // Members on locked tracks follow too; the lock guards membership.
        m_timeline.setSharedLookAt(target.value(ClipKey::track).toInt(), target.value(ClipKey::index).toInt(),
                                   source.value(ClipKey::gradeVersions).toList(), source.value(ClipKey::gradeVersion).toInt(),
                                   source.value(ClipKey::grade).toMap());
    }
    for (int i = 0; i < m_sequences.size(); ++i)
        if (i != m_activeSequence) {
            bool changed = false;
            auto snapshot = m_sequences[i].snapshot;
            for (QVariant &v : snapshot) {
                auto e = v.toMap();
                const QString id = linkOf(e).value("id").toString();
                if (!updates.contains(id))
                    continue;
                copyLook(&e, updates.value(id));
                v = e;
                changed = true;
            }
            if (changed) {
                m_sequences[i].snapshot = snapshot;
                if (QFileInfo::exists(nestPath(m_sequences[i].id)))
                    writeNest(m_sequences[i]);
            }
        }
    return true;
}

void PlayerWorker::shareColor(int track, int index, const QString &action, const QString &name,
                              const QVariantList &addresses) {
    if (exportActive() || m_timeline.trackLocked(track))
        return;
    const QVariantList before = colorUndoSnapshot();
    QVariantMap source;
    for (const QVariant &v : m_timeline.entries()) {
        auto e = v.toMap();
        if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::index).toInt() == index)
            source = e;
    }
    if (!video(source)) {
        emit gradeVersionFinished(QStringLiteral("Select a video clip to share its grade."));
        return;
    }
    if (action == "detach") {
        source = withLink(source, {});
        m_timeline.setGradeVersionsAt(track, index, source.value(ClipKey::gradeVersions).toList(),
                                      source.value(ClipKey::gradeVersion).toInt());
        m_timeline.setGradeAt(track, index, source.value(ClipKey::grade).toMap());
        recordEdit(before, false, tr("Share colour"));
        announceTimeline();
        announceSequences();
        refreshFrame();
        emit gradeVersionFinished(QStringLiteral("This clip now has independent local grades"));
        return;
    }
    if (action != "group" && action != "remote") {
        emit gradeVersionFinished(QStringLiteral("Unknown grade sharing action."));
        return;
    }
    const QString displayName =
        action == "remote" ? QFileInfo(source.value(ClipKey::path).toString()).fileName() : name.trimmed();
    if (displayName.isEmpty()) {
        emit gradeVersionFinished(QStringLiteral("Name the shared grade group."));
        return;
    }
    QVariantMap existing;
    auto find = [&](const QVariantList &entries) {
        for (const QVariant &v : entries) {
            const auto e = v.toMap(), link = linkOf(e);
            if (link.value("kind") == action &&
                (action == "group" ? link.value("name").toString().compare(displayName, Qt::CaseInsensitive) == 0
                                   : link.value("source") == source.value(ClipKey::path)))
                existing = e;
        }
    };
    for (int i = 0; i < m_sequences.size(); ++i)
        find(i == m_activeSequence ? m_timeline.entries() : m_sequences[i].snapshot);
    const QVariantMap link = existing.isEmpty()
                                 ? QVariantMap{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                               {"kind", action},
                                               {"name", displayName},
                                               {"source", action == "remote" ? source.value(ClipKey::path) : QVariant()}}
                                 : linkOf(existing);
    const QVariantMap look = withLink(existing.isEmpty() ? source : existing, link);
    auto selected = [&](const QVariantMap &e, bool active) {
        if (!video(e))
            return false;
        if (action == "remote")
            return e.value(ClipKey::path) == source.value(ClipKey::path);
        if (!active)
            return false;
        if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::index).toInt() == index)
            return true;
        for (const QVariant &a : addresses) {
            auto p = a.toMap();
            if (p.value("track") == e.value(ClipKey::track) && p.value("index") == e.value(ClipKey::index))
                return true;
        }
        return false;
    };
    // Membership changes respect track locks. Existing linked looks continue
    // to update every member, as stated in the sharing dialog.
    for (const QVariant &v : m_timeline.entries()) {
        auto e = v.toMap();
        if (selected(e, true) && m_timeline.trackLocked(e.value(ClipKey::track).toInt())) {
            emit gradeVersionFinished(QStringLiteral("Unlock the selected tracks before sharing grades."));
            return;
        }
    }
    for (int i = 0; i < m_sequences.size(); ++i)
        if (i != m_activeSequence) {
            const auto snapshot = m_sequences[i].snapshot;
            const auto flags = Timeline::trackFlagsFromVariant(
                snapshot.isEmpty() ? QVariantList() : snapshot.last().toMap().value("omavid:tracks").toList());
            for (const QVariant &v : snapshot) {
                auto e = v.toMap();
                int t = e.value(ClipKey::track).toInt();
                if (selected(e, false) && t >= 0 && t < int(flags.size()) && flags[t].locked) {
                    emit gradeVersionFinished(
                        QStringLiteral("Unlock matching source tracks in the other sequences first."));
                    return;
                }
            }
        }
    for (const QVariant &v : m_timeline.entries()) {
        auto e = v.toMap();
        if (!selected(e, true))
            continue;
        int t = e.value(ClipKey::track).toInt(), n = e.value(ClipKey::index).toInt();
        m_timeline.setGradeVersionsAt(t, n, look.value(ClipKey::gradeVersions).toList(), look.value(ClipKey::gradeVersion).toInt());
        m_timeline.setGradeAt(t, n, look.value(ClipKey::grade).toMap());
    }
    for (int i = 0; i < m_sequences.size(); ++i)
        if (i != m_activeSequence) {
            auto snapshot = m_sequences[i].snapshot;
            bool changed = false;
            for (QVariant &v : snapshot) {
                auto e = v.toMap();
                if (selected(e, false)) {
                    copyLook(&e, look);
                    v = e;
                    changed = true;
                }
            }
            if (changed) {
                m_sequences[i].snapshot = snapshot;
                if (QFileInfo::exists(nestPath(m_sequences[i].id)))
                    writeNest(m_sequences[i]);
            }
        }
    recordEdit(before, false, tr("Share colour"));
    announceTimeline();
    announceSequences();
    refreshFrame();
    emit gradeVersionFinished(
        action == "remote"
            ? QStringLiteral("Matching source clips now share grades and named versions across sequences")
            : QStringLiteral("Selected clips joined the shared grade group"));
}

void PlayerWorker::prepareColorMask(int request, int track, int index, const QString &nodePath, int frame) {
    QVariantMap clip;
    for (const QVariant &v : m_timeline.entries()) {
        auto e = v.toMap();
        if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::index).toInt() == index)
            clip = e;
    }
    auto grade = clip.value(ClipKey::grade).toMap();
    if (grade.value("nodes").toList().isEmpty() && nodePath == "n0") {
        auto n = Timeline::defaultGradeNode();
        n["id"] = "n0";
        grade = Timeline::defaultGrade();
        grade["nodes"] = QVariantList{n};
    }
    const auto node = GradeEngine::nodeAt(grade, nodePath);
    if (exportActive() || !video(clip) || clip.value(ClipKey::offline).toBool() || m_timeline.trackLocked(track) ||
        node.isEmpty() || frame < 0 || frame >= clip.value(ClipKey::frames).toInt()) {
        emit colorMaskFrameReady(request, {}, {},
                                 QStringLiteral("Select an online, unlocked clip and a correction node."));
        return;
    }
    QVariantMap target{{"sequence", m_sequences[m_activeSequence].id},
                       {"track", track},
                       {"index", index},
                       {"nodePath", nodePath},
                       {"node", node},
                       {"clip", clip}};
    for (const char *key :
         {"grade", "gradeVersions", "gradeVersion", "mixIn", "mixOut", "mixAlign", "mixExtA", "mixExtB"})
        clip.remove(key);
    clip[ClipKey::track] = 0;
    clip[ClipKey::lane] = 0;
    clip[ClipKey::index] = 0;
    clip[ClipKey::start] = 0;
    clip[ClipKey::gapBefore] = 0;
    Timeline scratch;
    auto settings = m_timeline.projectSettings();
    settings["custom"] = true;
    const double scale = std::min(1., 1280. / std::max(1, m_timeline.width()));
    settings["width"] = std::max(16, int(m_timeline.width() * scale) / 2 * 2);
    settings["height"] = std::max(16, int(m_timeline.height() * scale) / 2 * 2);
    scratch.setProjectSettings(settings);
    if (!scratch.restore({clip})) {
        emit colorMaskFrameReady(request, {}, {}, QStringLiteral("Could not prepare the clip's mask frame."));
        return;
    }
    const QImage image = WorkingFrame::shown(scratch.decodeAt(frame, false).image);
    emit colorMaskFrameReady(request, image, target,
                             image.isNull() ? QStringLiteral("Could not decode the mask frame.") : QString());
}
void PlayerWorker::applyColorMask(const QVariantMap &target, const QString &path) {
    const int track = target.value("track").toInt(), index = target.value("index").toInt();
    QVariantMap clip;
    for (const QVariant &v : m_timeline.entries()) {
        auto e = v.toMap();
        if (e.value(ClipKey::track).toInt() == track && e.value(ClipKey::index).toInt() == index)
            clip = e;
    }
    QVariantMap grade = clip.value(ClipKey::grade).toMap();
    const QString nodePath = target.value("nodePath").toString();
    if (grade.value("nodes").toList().isEmpty() && nodePath == "n0") {
        grade = Timeline::defaultGrade();
        grade["nodes"] = QVariantList{target.value("node")};
    }
    auto node = GradeEngine::nodeAt(grade, nodePath);
    bool valid = !exportActive() && !m_timeline.trackLocked(track) &&
                 target.value("sequence") == m_sequences[m_activeSequence].id && !node.isEmpty() &&
                 node == target.value("node").toMap();
    for (const char *key : {"path", "in", "out", "speed"})
        valid &= clip.value(key) == target.value("clip").toMap().value(key);
    if (!valid) {
        emit gradeVersionFinished(QStringLiteral(
            "The clip or correction changed during mask generation. Generate again for the current selection."));
        return;
    }
    node["externalMatte"] = path;
    node["matteChannel"] = "luma";
    GradeEngine::replaceNode(&grade, nodePath, node);
    const QStringList problems = GradeEngine::resourceProblems(grade);
    if (!problems.isEmpty()) {
        emit gradeVersionFinished(problems.join('\n'));
        return;
    }
    const auto before = m_timeline.entries();
    if (!m_timeline.setGradeAt(track, index, grade))
        return;
    recordEdit(before, true, tr("Apply colour mask"));
    announceTimeline();
    refreshFrame();
    emit gradeVersionFinished(QStringLiteral("Generated matte attached to the selected node"));
}
