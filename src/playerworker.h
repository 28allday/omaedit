#pragma once

#include <limits>
#include <QElapsedTimer>
#include "windowtracker.h"

#include <QHash>
#include <QCache>
#include <QImage>
#include <QLockFile>
#include "wavepeaks.h"
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QThreadPool>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <functional>
#include <atomic>
#include <thread>

#include "loudness.h"
#include "timeline.h"
#include "playbackaudio.h"

class QTemporaryDir;
class PreviewRenderCache;
#include "renderrequest.h"

// Lives on Backend's worker thread and owns all media work: probing imports,
// grabbing thumbnails, editing the timeline playlist and decoding playback —
// so a slow frame can never stall the GUI. Playback decodes ahead and shows
// each picture on a steady frame clock, locked to the audio sink's clock at
// normal speed (see pump()). A source-preview mode (openPreview) routes the shared transport at a
// standalone clip instead of the timeline, for three-point editing.
namespace render { class MediaSource; class MotionAnalysis; }

// What an edit from the bin or the source viewer takes of its clip.
namespace EditTakes { enum { Both = 0, Picture = 1, Sound = 2 }; }

class PlayerWorker : public QObject {
    Q_OBJECT

public:
    explicit PlayerWorker(QObject *parent = nullptr, std::unique_ptr<PlaybackAudioOutput> output = {});
    static qint64 steadyNs(); // the clock frameScheduled's due times are on
    // After a stop: the viewer shows `position`, so that is the playhead.
    void settleAt(int position);
    ~PlayerWorker() override;

public:
    // Per-channel peak levels in dBFS for one frame's interleaved s16
    // buffer; empty input reads as silence. Static so tests can pin the
    // maths without a running pump.
    static QList<float> levelsFor(const QByteArray &audio, int channels);
    // Thread-safe posting only; consecutive queued grades for the same clip
    // coalesce on the worker, with every other command acting as a boundary.
    void queueGrade(int track, int index, const QVariantMap &grade, quint64 gesture = 0);
    void queueEditProperties(int track, int index, const QVariantMap &patch, quint64 gesture);
    // May be called from the GUI while a seek is rendering. Only the atomic
    // request generation is touched; the worker retains ownership of the timeline.
    void cancelFrame();
    // Worker-thread only: deferred first placements replay in one event, with
    // each clipboard edit receiving the state left by preceding placements.
    QVariantMap timelinePlacementTarget() const;
    void replayTimelinePlacements(const QVariantMap &expected,
        const QList<std::function<void(const QVariantMap &)>> &placements);
    void placeSourceSnapshot(const QVariantMap &target, const QVariantMap &snapshot,
                             const QVariantMap &action);

public slots:
    void setBackgroundRenderEnabled(bool enabled);
    void setBackgroundRenderLimit(int gib);
    void backgroundRenderAction(const QString &action);
    void setBackgroundRenderProcessing(const QString &preference);
    void refreshSequences() { announceSequences(); }
    void importClip(const QString &path);
    void queueMediaDecor(const QString &path, int frames, double fps, bool audio, bool picture = true);
    void setMediaSources(const QVariantMap &sources, const QString &mode);
    QString mediaRateChangeProblem(const QVariantMap &sources);
    // Project-wide source attributes with one history step. The explicit
    // preservation mode adjusts edits to keep their selected recorded footage.
    QString applyClipAttributes(const QVariantMap &sources, bool preserveSelectedFootage);
    void relinkMedia(const QVariantMap &mapping, const QString &token);
    void previewMedia(const QString &path, int frame, bool audio, const QString &purpose, const QString &token);
    void setMediaGang(bool on, const QVariantMap &anchors = {});
    void setMonitorVolume(double volume);
    void setMonitorOptions(bool muted, bool dim, bool mono);
    void setMonitorMapping(const QString &mode); // Preferences ▸ Audio ▸ Monitor output
    void openPreview(const QString &path);
    void closePreview();
    void cancelCacheTask(const QString &id);
    // `takes` (EditTakes): the picture and its sound, the picture alone (no
    // sound follows it, and sound already under it stays), or the sound alone
    // (on an audio track; the pictures are left as they are).
    void appendClip(const QString &path, int in = -1, int out = -1, int takes = 0);
    void insertClip(const QString &path, int position, int in = -1, int out = -1, int takes = 0);
    void overwriteClip(const QString &path, int position, int in = -1, int out = -1, int takes = 0);
    void removeClip(int index);
    void liftClip(int index); // Shift+Del — leave an equal gap behind
    void removeRange(int start, int end); // range delete, one undo step
    void insertGap(int position, int frames); // Alt+W spacer
    void closeGap(int track, int position); // a selected gap, rippled shut
    void deleteGaps(); // every stretch empty on all tracks
    void rollEdit(int index, int delta); // precision editor's centre line
    void rollEditOn(int track, int index, int delta); // any track
    // Ripple trim at an address (the precision editor's outgoing/incoming
    // strips); V1 → trim(), lanes → trimOn(ripple).
    void trimClipOn(int track, int index, int in, int out);
    void trimToFrame(int track, int index, int frame, bool end, bool ripple);
    void makeCompound(int start, int end); // Alt+G — group span into one clip
    void breakApartClip(int index);        // Alt+Shift+G — ungroup it
    void setClipDisabled(int index, bool disabled); // V toggle
    // Track header flags: "locked" / "disabled" / "muted" / "solo" /
    // "autoSelect". Not an edit — no undo step (edit-page convention).
    void setTrackFlag(int track, const QString &flag, bool value);
    // Audio page: per-track mix state (role/colour/fader/pan/fx…), the
    // master and the ADR cue list. Project data, not edits — no undo step.
    void prepareAdrRecording(int cueIndex);
    void promoteAdrTake(int cueIndex, int takeIndex, int in = -1, int out = -1);
    void setAudioFadeCurve(int track, int index, const QString &curve);
    void endAudioGesture(int track, const QString &key);
    void beginAudioGesture(int track, const QString &key);
    void setTrackMix(int track, const QString &key, const QVariant &value);
    void setTrackMixMap(int track, const QVariantMap &mix);
    void setMasterMix(const QString &key, const QVariant &value);
    void setMasterMixMap(const QVariantMap &mix);
    void setAdrCues(const QVariantList &cues);
    // File ▸ Timeline Settings…: not an edit (no undo), but the timeline is
    // rebuilt against the new profile shape when it changes. The frame
    // rate is refused while the active timeline holds clips (a fixed-rate rule) — the
    // stored rate stays and projectSettingsChanged says so.
    void setProjectSettings(const QVariantMap &settings);
    // Preferences ▸ Playback: the output device by QAudioDevice id ("" =
    // the system default). The sink is rebuilt on the next audible frame.
    void setAudioOutput(const QString &deviceId, bool rebuild = false);
    // Preferences ▸ Playback: frames of audio queued ahead of the picture
    // (2–30). A live change rebuilds the sink before the next audible frame.
    void setAudioBufferFrames(int frames);
    int audioBufferFrames() const { return m_audioBufferFrames; }
    // Loop playback over [in, out] of the timeline (-1 = whole); a source
    // preview uses its independent source In/Out range while `on`.
    void setLoop(bool on, int in, int out);
    void setSourceLoopRange(int in, int out) { m_sourceLoopIn=in; m_sourceLoopOut=out; }
    // Seek and hear that frame's audio once (audio scrub while parked).
    void scrubTo(int frame);
    // A timeline picture for skimming: drawn and handed over
    // (skimFrameReady) without moving the playhead.
    void skimTo(int frame);
    // The pointer has left the timeline: no whole picture is owed for a skim.
    void endSkim();
    void resetLoudness();
    // Offline R128 pass over the whole programme on a scratch timeline;
    // chunked so playback stays responsive → loudnessAnalyzed.
    void analyzeLoudness(int in = -1, int out = -1);
    void cancelLoudnessAnalysis();
    // `peakCeiling` (dBTP), when given, is the target's true-peak limit the
    // result is reported against; normalizing never limits peaks itself.
    void normalizeLoudness(double target, double peakCeiling = qQNaN());
    // Color page: a clip's grade (an edit), its versions, and a window
    // tracker pass (opencv.tracker over the clip's source range, the stab
    // pattern) that writes the node's window.track keyframes.
    void setGrade(int track, int index, const QVariantMap &grade);
    void setGradeVersions(int track, int index, const QVariantList &versions, int active);
    void applyColourSource(int track, int index, const QString &scope, const QVariantMap &fields);
    void editGradeVersion(int track, int index, const QString &action, int version, const QString &name);
    void pasteColor(int track, int index, const QString &kind, const QVariantMap &payload, int afterNode, const QString &parentPath = QString());
    void autoBalance(int track, int index, int clipFrame);
    // Modify ▸ Match Colour onto one clip from the reference frame.
    void matchColor(int track, int index, int clipFrame, int refTrack, int refIndex, int refFrame);
    void prepareColorMask(int request,int track,int index,const QString &nodePath,int frame);
    void applyColorMask(const QVariantMap &target,const QString &path);
    void shareColor(int track, int index, const QString &action, const QString &name, const QVariantList &addresses);
    void colorMatte(int request, int track, int index, const QString &nodeId, int frame);
    void effectPreview(int request, int track, int index, int frame, bool matte, const QSize &size);
    void setNodeLut(int track, int index, int node, const QString &nodeId, const QString &path, double intensity);
    void previewNodeLut(int request, int track, int index, int node, const QString &nodeId, int frame, const QString &path, double intensity);
    void trackWindow(int track, int index, int node, double x, double y, double w, double h);
    void trackWindowRange(int track, int index, int node, int window, int frame, int direction, bool singleFrame, const QString &nodePath = QString());
    void stopWindowTracking(bool keep);
    void addTrack(bool audio);   // not an edit
    // An edit (the tracks above it are renumbered); refused unless empty.
    void removeTrack(int track);
    // Swap two user tracks — an edit: undo puts the order (and the headers
    // that travelled with it) back.
    void swapTracks(int a, int b);
    // E/W/D onto a destination track: mode 0 append (at the timeline's end,
    // no ripple), 1 insert (ripples auto-selected tracks), 2 overwrite. Audio tracks take the clip's sound only.
    // audioHint decides the kind only when `track` does not exist yet.
    // Fit to Fill (Shift+F11): the source range retimed to cover `frames`
    // at `position` on `track` (an overwrite; one undo step).
    void fitToFill(int track, const QString &path, int position, int frames, int in, int out,
                   bool audioHint = false);
    void placeOnTrack(int track, int mode, const QString &path, int position,
                      int in, int out, bool audioHint = false, int takes = 0);
    // ── Multicam ─────────────────────────────────────────────────────────
    // The NEXT placement (append/insert/overwrite/placeOnTrack/connect) is
    // a multicam clip: tag it with {mcam, angle} in the same edit. Queued
    // before the placement request, so it lands first.
    void tagNextPlacement(const QVariantMap &tag);
    void switchAngle(int track, int index, int angle);
    // Blade at `frame` and switch the tail; keepAudio = detach the clip's
    // sound first (audio-follow-video off), all one undo step.
    void cutToAngle(int track, int index, int frame, int angle, bool keepAudio);
    // The sound source of the placed multicam clip that clip (track, index)
    // is a piece of, chosen after the clip was made (Timeline::setSoundSourceOn):
    // every piece of it, one undo step. Says what was done (editNoted) or why
    // nothing was (editRefused).
    void setMulticamSound(int track, int index, int source);
    // Collapse Multicam on the timeline with this id, open or not
    // (Timeline::multicamCollapsed): one undo step. The open timeline's is an
    // ordinary edit; another timeline's stored clips are changed where they
    // are kept, with the history Change Clip Attributes has for timelines
    // that are not open. Says what was done (editNoted) or why nothing was
    // (editRefused).
    void collapseMulticam(const QString &sequenceId);
    void detachAudioOn(int track, int index);
    void reattachAudio(int track, int index);
    void setFlip(int track, int index, bool h, bool v);
    void setAudioMap(int track, int index, const QString &mode);
    void setNormalize(int track, int index, bool on);
    // Freeze Frame: a `length`-frame still of `frame` inserted there.
    void freezeFrame(int track, int index, int frame, int length);
    // Sync by waveform: per-frame peak envelopes, cross-correlated against
    // the first path. Offsets are virtual-timeline frames (min = 0).
    void syncByAudio(const QStringList &paths, double syncFps = 0., const QVariantMap &sources = {});
    // One frame per angle for the multicam viewer (frame < 0 = no picture).
    void decodeAngles(const QStringList &paths, const QList<int> &frames, int seq);
    // Drag between tracks (newTrack == track count creates one).
    void moveClipTo(int track, int index, int newTrack, int newStart);
    // Batch C edit ops: paste an entries() map (overwrite, or insert for
    // duplicate), nudge by frames, colour label, header rename.
    void pasteClip(int track, int position, const QVariantMap &entry, bool insert);
    void nudgeClip(int track, int index, int delta);
    void setLabel(int track, int index, const QString &label);
    // Bypass one applied attribute (an edit, undoable) — see
    // Timeline::setBypassAt for the keys.
    void setBypass(int track, int index, const QString &key, bool on);
    // Clip markers on one clip (the whole list; an edit, undoable).
    void setClipMarkers(int track, int index, const QVariantList &markers);
    // One marker set (by its frame) or removed, against the worker's own
    // markers, so quick successive requests build on each other.
    void editClipMarker(int track, int index, const QVariantMap &marker, bool remove);
    void setAudioTiming(int track, int index, const QVariantMap &timing);
    void splitAudio(int track, int index, const QVariantMap &position);
    void applyAudioEdit(int request, const QVariantMap &target, const QVariantMap &values, bool split);
    void applyAudioGesture(int request, const QVariantMap &target, const QString &action,
                           const QVariantMap &changes, int sampleRate);
    void pasteAudioClipboard(int request, const QVariantMap &target,
                             const QVariantMap &clipboard, const QVariantMap &placement);
    void applyMarkerEdit(int request, const QVariantMap &target, int frame, const QString &type,
                         const QString &text, const QString &notes, bool remove, const QString &colour = QString());
    void setAudioFollowsVideo(bool on) { m_timeline.setAudioFollowsVideo(on); }
    // Render & Replace: the clip alone (its attributes baked in) rendered
    // to a kept file (StoragePreferences::keptMediaRoot) in the background,
    // then swapped in over the same span; the original entry rides the new
    // clip for Restore Original.
    void renderAndReplace(int track, int index);
    void restoreOriginal(int track, int index);
    // Kept media (keptmedia.h). `adoptKeptMedia` runs once as the worker
    // starts, before any project is opened: what earlier builds left in the
    // cache folder, or a kept-media folder the preference has since left,
    // moves to today's folder. `sweepKeptMedia` finds the kept files nothing
    // the editor can check still names (the open project with its timelines
    // and history, the files in `documents`, every project copy under the
    // application's data folder, the strings in `alsoUsed`) and, when
    // `remove` is set, deletes them; `keptMediaSwept` reports {folder, files,
    // bytes, removed, failed}.
    void adoptKeptMedia(int settledSeconds = -1); // -1: KeptMedia::settlingSeconds()
    void sweepKeptMedia(const QStringList &documents, const QStringList &alsoUsed, bool remove);
    // Nested sequence: sequence `index` (not the active one) saved as a
    // timeline file and inserted on V1 at `position`. Its file follows the
    // sequence: every stash rewrites it.
    void nestSequence(int index, int position);
    void nestSequenceOnTrack(int index, int track, int position, bool audioHint);
    // Captions: every cue [{start, frames, text}] lands as a caption-tagged
    // title on the track named "Captions" (made if absent), one undo step.
    // With a `language` {code, name, trackName} (the file's): on the caption
    // track of that language, or a new one with it, named trackName.
    void importCaptions(const QVariantList &cues, const QVariantMap &style, const QVariantMap &language = {});
    // Captions onto caption tracks ("Captions", "Captions 2", …), each on the
    // first one with room; `remove` (flat connected indices) go first. One
    // undo step named `label`.
    void placeCaptions(const QVariantList &cues, const QVariantMap &style,
                       const QVariantList &remove, const QString &label, const QVariantMap &language = {});
    // Edit ▸ Captions ▸ Caption Style: `style` becomes the look of the
    // timeline's captions, kept on every caption track (a "Captions" track is
    // made if there is none). The captions that follow the old look (and have
    // not been dragged) take the new one; with everyCaption, all of them do,
    // dragged ones put back in place. One undo step.
    // `styles`: the new style of each caption track restyled, by its index
    // as text ("-1": a "Captions" track made for it when there is none).
    void restyleCaptions(const QVariantMap &styles, bool everyCaption);
    // One caption: its whole look (`style`, as connectTitleAt takes it) and,
    // when `place` is [x, y], its offset from where the style sets it
    // ([x, y, w, h]: its size as well). One undo step named `label`.
    void restyleCaption(int index, const QVariantMap &style, const QVariantList &place, const QString &label);
    // The speaker of the captions at these flat connected indices ("" =
    // none), with `settings` the timeline's caption settings after (a name
    // added to its speakers; empty: left as they are). One undo step.
    void setCaptionSpeaker(const QVariantList &indices, const QString &speaker, const QVariantMap &settings);
    // The caption list (Edit page). `settings` becomes the timeline's
    // caption settings (Timeline::captionSettings). One undo step `label`.
    void setCaptionSettings(const QVariantMap &settings, const QString &label);
    // New words for captions, [{index, text}] by flat connected index; a
    // caption keeps its look and speaker. One undo step `label`.
    void setCaptionTexts(const QVariantList &changes, const QString &label);
    // New times for captions, [{index, start, frames}], each on its own
    // track. A caption never covers or passes another: one with something
    // in its way stays where it was. One undo step `label`; captionsRetimed
    // says how many moved and how many stayed.
    void retimeCaptions(const QVariantList &changes, const QString &label);
    // Every caption whose speaker is `from` gets `to` ("" = none), and
    // `settings` become the caption settings (the speakers renamed). One undo step.
    void renameCaptionSpeaker(const QString &from, const QString &to, const QVariantMap &settings);
    // A new caption track holding a copy of every caption of `track` and its
    // style, in `language` {code, name}, named `name`, not burnt in. One
    // undo step.
    void duplicateCaptionTrack(int track, const QVariantMap &language, const QString &name);
    // Looks: blend mode, lens correction (edits, undoable).
    void setMixAlign(int track, int index, const QString &align); // dissolve alignment, undoable
    // A transition across the cut between clip `index` of `track` and the
    // next: centred, made of the clips' handles, in `style`. One undo step.
    void addTransition(int track, int index, int length, const QString &style);
    // Its frames before and after the cut (a grip on either edge); the cut
    // and the other side stay. One undo step.
    void setTransitionSides(int track, int index, int before, int after);
    // Sequences: several timelines in one project. The active one IS
    // m_timeline; the others wait as snapshots (entries + markers + track
    // flags + playhead). Switching keeps project history; edit snapshots own
    // stable timeline IDs. sequencesChanged carries names AND snapshots so the backend
    // can write them into the project file (the active one is the XML).
    void createSequence(int request, const QString &anchorId, const QString &name, const QVariantMap &settings = {});
    void addSequence(const QString &name);
    // Import Timeline…: a new sequence built from restore()-ready entries.
    // A new timeline of already lined-up clips, with what is being edited left
    // alone. items: [{path, audioOnly, offset}], pictures first.
    void addSyncedClip(const QString &name, const QVariantList &items, bool silenceCameras,
                       const QVariantMap &format);
    void importSequence(const QString &name, const QVariantList &snapshot, const QStringList &trackNames, const QVariantList &markers, const QVariantMap &format, const QStringList &problems);
    void switchSequence(int index);
    void renameSequence(int index, const QString &name);
    void moveSequenceToBin(int index, const QString &binId);
    void duplicateSequence(int index);
    void closeSequence(int index);
    void setSequences(const QVariantList &sequences, int active); // project load
    void setBlend(int track, int index, int mode);
    void setScaling(int track, int index, const QString &scaling);
    void setLens(int track, int index, double k1, double k2, double cx, double cy);
    void editVideoEffect(int track, int index, const QString &action,
                         const QString &uid, const QVariantMap &values);
    void findKeyBackdrop(int track, int index, const QString &uid, int clipFrame, double x = -1, double y = -1);
    // Stabilise: a background analysis pass over the clip's source range
    // (one frame per event-loop pass, stabProgress along the way), then
    // the vidstab filter is applied as an undoable edit. A cached analysis
    // (same source/range/shakiness) applies at once. Empty results remove.
    void stabilize(int track, int index, int shakiness, int smoothing);
    void removeStab(int track, int index);
    // Comparison view: every frame also goes out as its "before" (all
    // attribute filters off) through beforeFrameReady. Two decodes a
    // frame while it is on.
    void setCompare(bool on);
    // Two-up trim view around timeline frame `edge` (−1 = off). Re-peeked
    // on every refresh while set.
    void setTwoUpEdge(int edge);
    // Group edits over a selection, one undo step each. `addrs` is a list
    // of {track, index}; each op walks it in per-track descending order so
    // indices stay valid as clips leave. Nudge on a group holding a V1 clip
    // ripples from the earliest of them, carrying following clips along;
    // a lane-only group nudges every clip. Moves are
    // {track, index, newTrack, newStart} for lane clips, applied in the
    // direction of travel so a moving clip never lands on a still-unmoved
    // one; newTrack == trackCount() makes ONE new track for the batch.
    void removeClips(const QVariantList &addrs, bool ripple);
    void nudgeClips(const QVariantList &addrs, int delta);
    void moveClips(const QVariantList &moves);
    void setClipsDisabled(const QVariantList &addrs, bool disabled);
    // Link: a picture and sound from its own file become one pair again,
    // the sound put back under its picture. Unlink: every pair a clip in
    // `addrs` belongs to comes apart; each part then moves and trims alone.
    void setClipsLinked(const QVariantList &addrs, bool link);
    void setLabels(const QVariantList &addrs, const QString &label);
    void setCategories(const QVariantList &addrs, const QString &category);
    // Paste Attributes onto every address (one undo step): the chosen
    // groups of `source` replace theirs. Groups as Timeline::attributeGroups.
    void pasteAttributes(const QVariantList &addrs, const QVariantMap &source,
                         const QStringList &groups);
    // Paste several entries keeping their layout: the earliest lands at
    // `position`, the rest keep their offsets and tracks (V1 → V1). Link
    // ids are remapped per paste so the copies pair with each other.
    void pasteClips(const QVariantList &entries, int position);
    void pasteClipboardSnapshot(int request, const QVariantMap &target,
                                const QVariantMap &clipboard, const QVariantMap &placement);
    void setTrackName(int track, const QString &name); // not an edit
    void slideClip(int track, int index, int delta);
    void joinClips(int track, int index);
    // The connected lanes (B-roll tracks above the storyline).
    void connectClip(const QString &path, int position, int in = -1, int out = -1,
                     bool audioOnly = false);
    void connectTitle(const QString &text, int position, int length,
                      const QVariantMap &style = QVariantMap());
    void updateTitle(int index, const QString &text,
                     const QVariantMap &style = QVariantMap());
    void removeConnectedClip(int index);
    void moveConnectedClip(int index, int newStart);
    void trimConnectedClip(int index, int in, int out, int newStart,
                           bool ripple = false);
    void setConnectedRect(int index, double x, double y, double w, double h);
    void splitAt(int position);
    // Cuts one track's clips at each position, as one undo step `label`.
    void splitMany(int track, const QVariantList &positions, const QString &label);
    void splitAllAt(int position);
    void splitOn(int track, int position);
    // The overlap form (the pair's own frames shorten), as Alignment ▸
    // Overlap gives; the window's own new transitions are addTransition's.
    void addDissolve(int index, int length); // between clip and the next
    // The same four on a track clip, by flat connected index.
    void addConnectedDissolve(int index, int length);
    void removeConnectedDissolve(int index);
    void resizeConnectedDissolve(int index, int length);
    void restyleConnectedDissolve(int index, const QString &style);
    void setConnectedFades(int index, int fadeIn, int fadeOut);
    // One fade edge on or off against the worker's own fades (V1 index, or a
    // connected index), so quick toggles before the echo build on each other.
    void toggleFadeEdge(bool connected, int index, bool outEdge, int length);
    void removeDissolve(int index);
    void resizeDissolve(int index, int length); // new overlap; <1 removes
    void restyleDissolve(int index, const QString &style); // wipe pattern
    void setClipFades(int index, int fadeIn, int fadeOut);
    void setClipVolume(int index, double db); // 0 resets, -60 = silence
    void setConnectedVolume(int index, double db); // same, lane clips
    // Volume keyframes ([{frame, db}], clip-relative): one undo step each.
    void setClipVolumeKf(int index, const QVariantList &kf);
    void setConnectedVolumeKf(int index, const QVariantList &kf);
    // Lane-clip opacity (Video Animation): 0..1; kf = [{frame, alpha}].
    void setConnectedOpacity(int index, double alpha);
    void setConnectedOpacityKf(int index, const QVariantList &kf);
    void setClipPan(int index, double pan);        // -1 left … +1 right
    void setConnectedPan(int index, double pan);
    void setClipEq(int index, const QString &preset);      // "" = flat
    void setConnectedEq(int index, const QString &preset);
    // Detach Audio — video-only storyline clip + connected audio clip,
    // one undo step.
    void detachAudio(int index);
    void applyRetime(int request, const QVariantMap &target, double speed);
    void prepareSpeedRamp(int request, const QVariantMap &target);
    void applySpeedRamp(int request, const QVariantMap &target, const QVariantMap &definition, bool normal = false);
    void setClipSpeed(int index, double speed); // retime; 1.0 = normal
    void setConnectedSpeed(int index, double speed); // track clips, flat index
    void setClipCrop(int index, double l, double r, double t, double b,
                     double feather = 0.0, bool invert = false);
    void setConnectedCrop(int index, double l, double r, double t, double b,
                          double feather = 0.0, bool invert = false);
    void setConnectedRotation(int index, double degrees);
    void setConnectedAnchor(int index, double anchorX, double anchorY);
    void setEditProperties(int track, int index, const QVariantMap &patch);
    void applyMotionPreset(int track, int index, const QString &preset, const QVariantMap &options);
    void setFrameBlend(int track, int index, int mode);
    // Generators / adjustment clips on a lane (see Timeline::setAdjustAt).
    void connectGenerator(const QString &resource, int position, int length, bool adjust);
    void setGeneratorColor(int track, int index, const QString &color);
    void setClipColor(int index, double brightness, double contrast,
                      double saturation); // all-neutral resets
    void setConnectedColor(int index, double brightness, double contrast,
                           double saturation); // track clips, flat index
    // ripple=false (the edit-page default): the edge moves, space opens or
    // closes; ripple=true: downstream rides the delta (Trim mode, T).
    void trimClip(int index, int in, int out, bool ripple = true);
    void moveClip(int from, int to);
    // Markers live here so the worker-side undo stacks cover them (each
    // snapshot carries the marker list in a sentinel entry). A marker is a
    // map {frame, text, type} — type standard/chapter/todo/done.
    void toggleMarker(int frame); // records an undo step
    void removeMarkersIn(int first, int last); // one undo step
    void setMarkerInfo(int frame, const QString &text,
                       const QString &type, const QString &notes = QString()); // Shift+M — records an edit
    void setMarkers(const QVariantList &markers); // project load — no undo step
    void undo();
    // The window has the clip list an undo, a redo or a timeline switch left
    // (the "replay" number that came with it in timelineChanged's context):
    // edits that name a clip by its place are taken again. Until then they
    // are refused, for they were made against the list from before. The
    // first call (the window sends 0 when it starts) says a window is
    // answering at all; without one nothing is held back.
    void acknowledgeReplay(int serial);
    void redo();
    void seek(int frame);
    void seekAudioPosition(quint64 request, const QVariantMap &context,
                           const QVariantMap &position, bool audition = false);
    void setSpeed(double speed); // 0 pauses, 1 plays with audio, ±2…±8 shuttle
    // The device pixels the program viewer shows a picture at: playback
    // draws the timeline at that size (when smaller), paused frames whole.
    void setPlaybackDrawSize(const QSize &size);
    // An image sequence delivery: its finished files, and their move into the folder it is delivered as.
    static int sequenceFilesIn(const QString &folder);
    bool publishSequence(const QString &part, QString *error);
    // The display playback's pictures are made for (Timeline::setShownAs).
    void setPlaybackDisplay(const QVariant &display);
    // The scopes are open: pictures made for the display have their scopes
    // counted on the engine as they are drawn (Timeline::setScopesAs; the
    // options as ScopeProcessing::measured gives them). Null: closed.
    void setPlaybackScopes(const QVariant &options);
    // The timeline's picture at `position`, whole (not drawn small).
    QImage wholeFrame(int position);
    // The same three for the source viewer: the size and the display it shows
    // its clip at, so a played or scrubbed source picture is drawn for it on
    // the engine (MediaPlayer::setViewerDraw), and the clip's whole working
    // frame at `position`. A picture drawn that way is followed by its working
    // frame once the playhead rests or the clip stops (settleSource).
    void setSourceDrawSize(const QSize &size);
    void setSourceDisplay(const QVariant &display);
    QImage wholeSourceFrame(int position);
    // Filmstrip tiles and waveform peaks, counted (diagnostics and tests):
    // batches of tiles asked of the tile thread, tiles drawn there, tiles
    // found kept from an earlier run instead, sounds whose peaks were made by
    // reading them through (peaksRead) or found kept from an earlier run
    // (peaksKept), and how often the tile thread rebuilt its copy of the timeline.
    int tileBatches() const { return m_tileBatches; }
    int tilesDrawn() const { return m_tilesDrawn; }
    int tilesKept() const { return m_tilesKept; }
    int tileRebuilds() const { return m_tileRebuilds; }
    int peaksRead() const { return m_peaksRead; }
    int peaksKept() const { return m_peaksKept; }
    // Whether filmstrip jobs are waiting or being made (tests).
    bool stripWorkPending() const { return !m_stripJobs.isEmpty(); }
    void requestSpeed(double speed, quint64 request);
    // The pre-Deliver presets: "h264-high" (default), "h264-medium",
    // "prores", "audio".
    void exportTimeline(const QString &path,
                        const QString &preset = QString());
    // A Deliver render: see ExportSpec for the keys.
    void exportTimeline(const QString &path, const QVariantMap &spec);
    void prepareRenderSnapshot(int jobId);
    void pauseExport();
    void resumeExport();
    void cancelExport();
    bool exportActive() const;
    // Frames in a rendered file's video stream (ffprobe) — resume math.
    static int countVideoFrames(const QString &file);
    int timelineFrameCount() const { return m_timeline.frameCount(); }
    QVariantList timelineTracks() const { return m_timeline.tracks(); }
    void saveProject(const QString &path, const QStringList &library);
    void saveRecovery(const QString &path, const QString &origin, const QStringList &library, const QString &token);
    void prepareProjectArchive(const QStringList &library, const QString &token);
    void loadProject(const QString &path);
    void createProject(int request, const QString &anchorId, const QVariantMap &settings);
    void setDecorationWindow(double first, double last, int pixels);
    void newTimeline(); // clear everything for a fresh project

signals:
    void clipAttributesRestored(const QVariantMap &attributes);
    void frameError(const QString &message);
    void frameDiscarded(bool source);
    void adrRecordReady(const QString &cueId, int track, int start, int stop, const QString &prefix);
    void editOperationFinished(const QString &message);
    void editRefused(const QString &reason); // an edit changed nothing, and why
    void editNoted(const QString &note);     // an edit was made, and what the editor should know of it
    void multicamPlaced();                   // a multicam clip was put on the timeline (not cut, moved or put back)
    void audioOperationFinished(const QString &message);
    void gradeVersionFinished(const QString &message);
    void colorPasteFinished(int track, int index, int node, const QString &message);
    void autoBalanceFinished(int track, int index, int node, const QString &message);
    void matchColorFinished(int track, int index, int node, const QString &message);
    void nodeLutFinished(bool changed, const QString &message);
    void colorMaskFrameReady(int request,const QImage &image,const QVariantMap &target,const QString &error);
    void colorMatteReady(int request, const QImage &image, const QString &error);
    void effectPreviewReady(int request, const QImage &image, const QString &error);
    void lutPreviewReady(int request, const QImage &before, const QImage &after, const QString &error);
    // Empty error means success; index is the resulting V1 clip.
    void compoundEditFinished(bool breakingApart, int index, const QString &error);
    void markerEditFinished(int request, bool changed, const QString &error);
    void retimeFinished(int request, bool changed, const QString &error);
    void audioEditFinished(int request, bool changed, const QString &error);
    void audioGestureFinished(int request, bool changed, const QString &error, const QVariantMap &selected);
    void audioClipboardFinished(int request, bool changed, const QString &error, const QVariantList &added);
    void speedRampPrepared(int request, const QVariantMap &state);
    void markersChanged(const QVariantList &markers);
    // Track header state (see Timeline::tracks()), after every edit and
    // every flag change.
    void tracksChanged(const QVariantList &tracks);
    // meta: {name, path, duration, fps, width, height}
    void clipImported(bool ok, const QString &path, const QVariantMap &meta,
                      const QImage &thumbnail);
    void historyChanged(const QString &undoText, const QString &redoText);
    void audioPositionReady(quint64 request, const QVariantMap &context,
                            const QVariantMap &position, int pictureFrame, const QString &error,
                            const QImage &image = {}, const QImage &sourceImage = {},
                            int sourceFrame = -1, const QString &sourcePath = {});
    void audioCursorRestored(const QVariantMap &context, const QVariantMap &position, int pictureFrame);
    void timelineChanged(const QVariantList &entries, int duration, double fps,
                         int width, int height, bool canUndo, bool canRedo, bool contentEdit, const QVariantMap &format,
                         const QVariantMap &audioEditContext);
    void frameReady(const QImage &image, int position);
    // Timeline playback hands pictures over ahead of time: show `image` when
    // the steady clock (PlayerWorker::steadyNs) reaches dueNs, so a slow
    // decode here can't hold back a picture that is already made.
    void frameScheduled(const QImage &image, int position, qint64 dueNs);
    // Drop every scheduled picture not yet shown; the one for `settled`, if
    // held, is where playback stopped and is shown first.
    void scheduleCleared(int settled);
    void framesSkipped(int count); // pictures playback never made, to keep time
    void cacheTaskChanged(const QVariantMap &task);
    void backgroundRenderChanged(const QVariantMap &status);
    void backgroundFrameUsed(int frame); // diagnostics and playback-path tests
    void linearPlaybackChanged(bool enabled);
    void beforeFrameReady(const QImage &image);
    // Two-up trim view: the frame before the edit point and the frame at
    // it (either null when the edge has no clip on that side).
    void twoUpReady(const QImage &outgoing, const QImage &incoming);
    void renderProgress(int track, int index, int percent); // 100 = replaced, -1 = failed
    void stabProgress(int track, int index, int percent); // 100 = applied, -1 = failed
    // After stabProgress 100: the zoom (percent) the stabilised clip takes on
    // every frame, and how much of its shake that steadies (1, all of it).
    void stabApplied(int track, int index, double zoom, double steadied);
    void trackProgress(int track, int index, int node, int percent); // 100 = keyframes written, -1 = failed
    void projectCreateFinished(int request, bool created, const QString &error);
    void sequenceCreateFinished(int request, bool created, const QString &error);
    void sequencesChanged(const QVariantList &sequences, int active); // [{name, snapshot, position}]
    void sequenceImportFinished(const QString &name, const QStringList &problems, bool ok, const QString &error);
    void sequenceOpenFailed(const QString &name); // its media is missing; stayed on the previous one
    void captionsImported(int placed, int tracks);
    void captionsRestyled(int changed, int kept);
    void captionTrackDuplicated(int track, int captions);
    void previewOpened(bool ok, const QString &path, int duration, double fps);
    void previewFrameReady(const QImage &image, int position);
    void skimFrameReady(const QImage &image, int frame);
    // The whole working frame of a skimmed frame that was first shown the
    // quick way (see settleFrame); it answers no skim request.
    void skimFrameSettled(const QImage &image, int frame);
    // One timeline clip's worth of filmstrip tiles, decoded in the background.
    void mediaPreviewReady(const QString &purpose, const QString &token, const QImage &image, int frame);
    void mediaRelinked(bool ok, const QVariantMap &mapping, const QString &token, const QString &message);
    void stripSourceReady(const QString &key, const QString &path, int frame, int width,
                          const QVariantMap &inputTransform, const QVariantMap &workingManagement);
    void filmstripReady(int index, int start, const QStringList &keys,
                        const QList<QImage> &tiles, const QVariantMap &layout);
    // The previewed source clip's filmstrip (range-marking strip).
    void sourceStripReady(const QString &path, const QStringList &keys,
                          const QList<QImage> &tiles);
    // A library clip's row filmstrip, decoded lazily after import.
    void libraryStripReady(const QString &path, const QStringList &keys,
                           const QList<QImage> &tiles);
    // A library clip's peaks are made (WavePeaks::Store has them): which
    // sound, and its shape {id, rate, channels, samples}.
    void libraryWaveformReady(const QString &path, const QVariantMap &wave);
    // Multicam: sync result (message = why it failed, "" = ok) and the
    // viewer's angle frames (same order as asked; null = no picture).
    void syncedClipFinished(const QString &name, const QString &error);
    void clipsSynced(const QStringList &paths, const QList<int> &offsets,
                     const QString &message);
    void anglesReady(const QList<QImage> &frames, int seq);
    // The peaks one storyline clip's waveform reads are made (as libraryWaveformReady).
    void waveformReady(int index, int start, const QVariantMap &wave);
    // A connected clip's filmstrip (video-bearing lane clips only).
    void connectedFilmstripReady(int index, int start, const QStringList &keys,
                                 const QList<QImage> &tiles, const QVariantMap &layout);
    // The peaks a connected-lane clip's waveform reads are made (index into the connected list).
    void connectedWaveformReady(int index, int start, const QVariantMap &wave);
    // Per-channel peak of the audio just handed to the sink, in dBFS
    // (-100 = silence/reset) — drives the transport's level meters.
    void audioLevels(float left, float right);
    // Audio page meters, per playing frame: level taps (6 linear peaks per
    // track — in L/R, post-insert L/R, out L/R), the master's running R128
    // values with their momentary history, and a 48-bin spectrum in dBFS.
    void trackLevels(const QList<float> &peaks);
    void audioRoutingLevels(const QVariantList &meters);
    void loudnessChanged(const QVariantMap &values, const QList<float> &history);
    void spectrumChanged(const QList<float> &bins);
    // The offline pass: percent < 100 while it runs, then the full result.
    void loudnessAnalyzed(int percent, const QVariantMap &values, const QList<float> &history);
    void masterChanged(const QVariantMap &master);
    // After every announce: the settings as stored and whether the rate is
    // locked (some sequence holds clips).
    void projectSettingsChanged(const QVariantMap &settings, bool rateLocked);
    void adrCuesChanged(const QVariantList &cues);
    void captionSettingsChanged(const QVariantMap &settings);
    void captionTextsChanged(int changed);
    void captionsRetimed(int moved, int kept);
    void stopped(); // ran off either end of the timeline
    void playbackSpeedChanged(double speed, quint64 request);
    void renderSnapshotReady(int jobId, const QVariantMap &snapshot);
    void exportFallbackUsed(const QString &message);
    void exportProgress(int percent);
    void exportFinished(bool ok, const QString &path);
    // The full story: why it failed (or "Cancelled") and the encoder log.
    void exportEnded(bool ok, const QString &path, const QString &reason,
                     const QStringList &log);
    void exportStageChanged(const QString &stage); // pre/render/join/encode/captions/""
    void exportPausedChanged(bool paused);
    void projectSaved(bool ok, const QString &path, const QString &error = QString());
    void keptMediaAdopting(const QString &folder);
    void keptMediaAdopted(const QString &folder, int moved, const QStringList &problems);
    void keptMediaSwept(const QVariantMap &report);
    void recoverySaved(const QString &path, const QString &token, const QString &error);
    void projectArchiveReady(const QVariantMap &document, const QString &token);
    // `records` is `library` decoded (ProjectSafety::decodeLibraryState) on
    // this worker, with each clip's file checked, so the interface thread
    // neither parses the file's JSON nor stats its media. `library` keeps the
    // saved lines as written.
    void projectLoaded(bool ok, const QString &path, const QStringList &library, const QVariantList &records);
    void timelineCleared();

protected:
    bool event(QEvent *event) override;

private:
    QString preparedNest(int index);
    void resetTimeline(const QVariantMap &settings);
    void flushQueuedGrade();
    void applyGrade(int track, int index, const QVariantMap &grade, quint64 gesture);
    void recordGestureEdit(const QVariantList &before, quint64 gesture, const QString &action);
    void flushQueuedEditProperties();
    void applyEditProperties(int track, int index, const QVariantMap &patch, quint64 gesture);
    QVariantMap m_queuedEditProperties;
    int m_queuedEditTrack = -1, m_queuedEditIndex = -1;
    quint64 m_queuedEditGesture = 0, m_lastEditGesture = 0;
    bool m_editRolledBack = false; // the last recordEdit undid its edit: skip its marker ripple
    int m_lastGestureRevision = 0;
    bool m_editFlushPosted = false;
    QVariantMap m_queuedGrade;
    quint64 m_queuedGradeGesture = 0;
    int m_queuedGradeTrack = -1, m_queuedGradeIndex = -1;
    bool m_gradeFlushPosted = false;
    void pump();
    void pollExport();
    void recordEdit(const QVariantList &before, bool settleAudio = true, const QString &action = {});
    // A clip's sound alone, by the edit `mode` asks for (0 append, 1 insert,
    // 2 overwrite), on the audio track a whole edit's sound would have used.
    void placeSound(int mode, const QString &path, int position, int in, int out);
    // Set for the length of a picture-only edit: recordEdit leaves the sound
    // of the pictures it covered.
    bool m_pictureOnlyEdit = false;
    void reportRefusedNudge(int track, int delta); // why a nudge moved nothing
    void reportRefusedBlade(int track);            // why a blade cut nothing
    void reportRefusedPlacement(int track);        // why a placement placed nothing
    void reportFadeHeldByTransition(const QVariantMap &entry, int fadeIn, int fadeOut);
    void reportKeptTransitions(const QList<Timeline::KeptMix> &kept);
    void carryCopiedTransitions(const QVariantList &copies, const QVariantList &landed);
    bool pasteFrameClipboard(const QVariantList &entries, int position, int singleTrack,
                             bool insert, bool duplicate, QString *error, QVariantList *added,
                             const QVariantMap &exactPosition = {});
    bool restoreSnapshot(const QVariantList &snapshot, bool contentEdit = true);
    QVariantMap markerSentinel() const; // current m_markers, snapshot-ready
    QVariantMap inactiveColorSnapshots() const;
    QVariantList colorUndoSnapshot();
    void restoreColorSnapshots(const QVariantMap &snapshots);
    bool synchronizeColorLinks(const QVariantList &before);
    QVariantList snapshotNow();         // entries() + marker/track sentinel
    QStringList currentProjectLibrary(const QStringList &library);
    QVariantMap resolvedTimelineSettings();
    void announceTimeline(bool contentEdit = true);
    void announceTimelineState(bool contentEdit);
    QVariantMap audioEditContext() const;
    QString m_audioEditSession;
    QString m_audioEditSequence;
    int m_audioSequenceActivation = 0;
    void queueFilmstrips();
    void boundLibraryDecorJobs();
    void endTimelineDecorJobs();
    void queueSourceStrip();
    void queueLibraryStrip(const QString &path, int frames, double fps);
    void queueLibraryWave(const QString &path, int frames);
    void scheduleStripWork();
    void reportStripWork(bool registerNew = false);
    QElapsedTimer m_cacheFeedbackClock;
    qint64 m_cacheFeedbackNext = 0;
    void finishStripWork(int index, const QString &state = QStringLiteral("completed"), const QString &error = {});
    void processStripTile();
    bool activeEmpty() const;
    double activeFps() const;
    // `interactive`: for a seek (the playhead dragged, stepped, clicked into
    // place). Drawn as playback draws, when the viewer has said how it shows
    // a picture; the whole working frame follows once the playhead rests.
    void refreshFrame(bool interactive = false);
    bool interactiveShown() const;
    void settleDue(const QImage &shown, int skim = -1);
    void settleFrame();
    MediaPlayer::Decoded sourceShown(int position, bool audio);
    void settleSource();
    std::atomic<quint64> m_frameGeneration{0};
    std::shared_ptr<RenderRequest> frameRequest();
    void refreshMediaGang();
    void resetMediaGangAnchors();
    bool mediaGangReady();
    struct GangedSourceFrame { int position = -1; MediaPlayer::Decoded decoded; };
    GangedSourceFrame decodeGangedSource();
    bool m_mediaGang = false;
    double m_monitorVolume = 1.0;
    bool m_monitorMuted = false, m_monitorDim = false, m_monitorMono = false;
    QString m_monitorMapping = QStringLiteral("stereo");
    int m_gangSourceAnchor = 0, m_gangTimelineAnchor = 0;
    int m_gangSourceNum = 0, m_gangSourceDen = 1;
    QVariantMap m_gangFineAnchor, m_gangContext;
    QHash<QString, std::shared_ptr<MediaPlayer>> m_mediaPreviewPlayers;
    QHash<QString, QString> m_mediaPreviewPaths;
    QHash<QString, QVariantMap> m_mediaPreviewSettings;
    bool ensureAudio();
    bool audioBackpressured() const;
    void flushAudio(bool keepPaused = false);
    int frameAudioBytes() const;
    qint64 framePeriodNs() const;
    bool fineAudioSample(qint64 *sample) const;
    bool parkFinePlayback();
    void retainPausedAudio(qint64 played);
    void discardPausedAudio();
    int nextAudioPosition(int from, bool retained);
    QVariantMap m_fineAudioPosition, m_fineAudioContext;
    int m_fineAudioFrame = -1;
    qint64 m_playFirstAudioSample = -1;

    // Playback (see pump()). Frames are produced a few ahead of the picture
    // and wait in m_playQueue until due; sound goes, in order, through
    // m_audioPending to the sink. Late pictures are skipped, sound never is.
    struct QueuedFrame {
        int position = 0;
        qint64 dueNs = 0;
        qint64 durationNs = 0;
        qint64 audioOffset = 0;   // where its sound starts, in bytes since the anchor
        qint64 firstSample = -1;  // absolute timeline sample, including a cropped prefix
        bool scheduled = false;   // already handed to the viewer
        MediaPlayer::Decoded decoded;
        QList<float> trackPeaks;
        QVariantList routing;
        std::shared_ptr<const AudioEngine::MeterCapture> meterCapture;
        // What the mixer's inserts held before this block (sound made ahead
        // only): to mix again from here should a change take the block back.
        std::shared_ptr<const sound::MixMemory> mixMemory;
        QImage before;
    };
    QList<QueuedFrame> m_playQueue;
    // Keep processed sound until the output consumes it. Pictures are not
    // retained here; resume renders them without touching audio effect state.
    QList<QueuedFrame> m_audioHistory, m_pausedAudio;
    QVariantMap m_playContext, m_pausedContext;
    int m_pausedNext = -1;
    bool m_playAnchored = false;
    bool m_playSourceMode = false;
    bool m_playFineAudio = false;
    struct AudioSpan { qint64 offset = 0, firstSample = 0, bytes = 0; };
    QList<AudioSpan> m_audioSpans;
    int m_playNext = -1;           // next position to produce; -1 past the end
    int m_playShown = -1;          // last position presented
    qint64 m_playNextDueNs = -1;   // when m_playNext will be due
    double m_playFrameNs = 0;      // running cost of producing one frame
    double m_playSyncNs = 0;       // filtered sound-minus-picture offset
    qint64 m_playSyncBase = -1;    // the offset reading "in sync" at the start
    qint64 m_audioWaitNs = 0;     // output backpressure pauses the presentation clock
    QByteArray m_audioPending;     // sound produced, not yet taken by the sink
    qint64 m_audioQueued = 0, m_audioWritten = 0; // bytes since the anchor
    // Sound made ahead (playerworkerplayback.cpp, "Sound ahead of the picture").
    // All of it belongs to the worker's thread, as the rest of the pump does;
    // only m_fed's ring is shared, with the feeder thread (see FedAudioOutput).
    FedAudioOutput *m_fed = nullptr;   // m_audioOutput when the device is fed from a thread of its own
    bool m_soundAhead = false;         // this run of play makes its sound ahead (set at the anchor)
    QList<QueuedFrame> m_soundQueue;   // blocks in the ring, in play order, whose pictures are not made yet
    int m_soundNext = -1;              // the next position to mix; -1 past the end
    qint64 m_soundShallowUntilNs = 0;  // no sound further ahead than its picture before this (a change was just made)
    quint64 m_soundGeneration = 0;     // Timeline::soundGeneration() the queued sound was mixed under
    quint64 m_soundDry = 0;            // m_fed->dryTimes() when last looked at
    qint64 m_playSyncPlayed = -1;      // what the device had played at the last picture shown
    qint64 m_pumpNs = 0;               // when the pump last ran
    qint64 m_playHeldNs = 0;           // when the pump last found it had been away for a while
    QByteArray m_spliceOld;            // the start of the block a change cut away, to fade from
    int m_splicePosition = -1;
    bool produceSound();
    void produceAheadFrame();
    bool skipLateAhead();
    void fillSoundAhead();
    bool soundWanted() const;
    void soundChanged();
    void soundMayHaveChanged();
    void takePlayNext();
    void resetPlayback(bool keepPaused = false);
    void settlePlayback();
    int playDepth(bool audible) const;
    int nextPlayPosition(int from) const;
    void produceFrame(bool audible);
    bool skipLate(bool audible);
    void queueAudio(QueuedFrame &frame, bool audible);
    void trimAudioStart(QueuedFrame &frame, qint64 sample);
    void feedSink();
    void queueAudition(const QByteArray &audio);
    void presentFrame(QueuedFrame &frame, bool audible);
    bool cachedPlaybackFrame(int position, bool audio, MediaPlayer::Decoded *out);
    // Playing through a rendered section: the kept picture or the frame drawn
    // again, whichever has been the quicker there (see decodePlaybackFrame).
    struct PlayCost {
        double keptMs = -1, drawnMs = -1;  // running means
        double madeMs = -1;                // the background render's time where drawing was tried
        int kept = 0, trying = 0, tries = 0;
        bool draw = false;
    };
    QHash<int, PlayCost> m_playCosts;      // by a range's first frame
    QString m_playCostsOf;                 // the render, size and display they were measured for
    bool drawRatherThanKept(int position, PlayCost **cost);

    Timeline m_timeline;
    PreviewRenderCache *m_backgroundRender = nullptr;
    QTimer m_backgroundRenderDelay;
    bool m_backgroundRenderEnabled = true;
    int m_backgroundRenderLimit = 20;
    QString m_backgroundRenderProcessing = "auto";
    // The timeline as it was before an edit that stopped part way: put back
    // only when the attempt changed something (a rebuild reopens every
    // source). False when it could not all be put back; the window is then
    // shown what the timeline really holds, and told.
    bool putBack(const QVariantList &clips, const QVariantList &tracks);
    // Whether the request being handled names a clip by its place in a clip
    // list the window no longer has right: one sent after an undo, a redo
    // or a timeline switch was asked for and before the window drew the
    // result. Says so (editRefused) when it is.
    bool staleEdit();
    int m_replaySerial = 0;       // the clip lists announced by an undo, a redo or a timeline switch so far
    int m_replayDrawn = 0;        // the last of them the window said it has
    bool m_replaying = false;     // the request being handled is one of those
    bool m_replayAnswered = false; // a window answers (acknowledgeReplay)
    bool m_replayAwaited = false; // and has not yet answered for the last one
    bool m_dispatchFresh = false; // handling a request as it arrived, not an edit's own inner step
    void invalidateBackgroundRender();
    // An announce: the kept pictures go only when one of them changed.
    void picturesMayHaveChanged();
    QVariantMap m_backgroundRenderOf; // what they are pictures of (see PreviewRenderCache::samePictures)
    bool m_mixAnnounce = false;       // the announce in hand is of the mixer alone: no picture in it
    void updateBackgroundRenderBusy();
    MediaPlayer::Decoded decodePlaybackFrame(int position, bool audio);
    // entries() snapshots, oldest first; the last element of each is a
    // sentinel map {"omavid:markers": [...]} carrying the marker list.
    bool m_compare = false;
    int m_twoUpEdge = -1;
    struct Sequence {
        QString name; QVariantList snapshot; int position = 0; QString id;
        QVariantMap settings;
        bool configured = false; // the empty engine is not yet a user-created timeline
        bool lockFormat = false; // match the first clip once, then keep that format
        QVariantMap audioPosition = {};
        QString binId; // the media bin it is filed in; empty = the library root
        bool synced = false; // made by Sync Clips: lives in the library, no tab of its own
    };
    static QString newSequenceId();
    const QString m_nestSessionId;
    QString nestPath(const QString &id) const; // the saved-timeline file a nested sequence plays from
    bool writeNest(const Sequence &seq);        // (re)write it from the snapshot
    QString nestFolder() const;                 // this session's, removed with the worker
    bool nestCurrent(const Sequence &seq) const;
    // `children`: the file itself and each nested file it embeds, as they stood (size, time).
    struct NestWritten { QVariantList snapshot; QVariantMap settings, sources; QVariantMap children; };
    QHash<QString, NestWritten> m_nestsWritten; // by sequence id: what each nest file was last written from
    bool refreshNests();                        // children before their enclosing sequences
    QList<Sequence> m_sequences{Sequence{QStringLiteral("Sequence 1"), {}, 0, newSequenceId(), {}}};
    int m_activeSequence = 0;
    void announceSequences();
    void stashActiveSequence();
    QVariantMap parkedAudioPosition() const;
    void restoreAudioPosition(const QVariantMap &position, bool resetGang = true);
    QList<QVariantList> m_undoStack;
    QList<QVariantList> m_redoStack;
    QVariantMap sourceAttributeState(const QStringList &paths);
    QVariantList sourceAttributeCounterpart(const QVariantList &target);
    QString restoreSourceAttributeState(const QVariantMap &state, bool contentEdit, bool notifyLibrary = true, bool restoreSequences = false);
    void recordSequenceEdit(const QVariantMap &before, const QString &label);
    void restoreHistory(bool redo);
    QVariantList m_markers; // {frame,text,type} maps sorted by frame
    int markerIndexAt(int frame) const;
    void sortMarkers();
    // Markers ride storyline ripples: ones at/after pivot shift by delta,
    // ones landing inside a removed span die. Call AFTER recordEdit (the
    // sentinel must capture the pre-edit list); emits markersChanged.
    void rippleMarkers(int pivot, int delta);

    // A job's own things on the thread its tiles or peaks are made on: touched
    // there only, while a batch of the job runs (one batch of a job at a time).
    struct JobBench {
        bool opened = false, openFailed = false;  // its player
    };
    struct StripJob {
        int index = 0;
        int start = 0;
        QString sourcePath;  // set = decode from the open preview producer
        QString libraryPath; // set = decode from the job's own producer
        std::shared_ptr<MediaPlayer> player; // opened lazily for library jobs
        QVariantMap timedClip; // isolated timing for connected media
        std::shared_ptr<Timeline> timedTimeline;
        QString taskId;
        int reportedPercent = -2;
        QString reportedState;
        bool timeline = false; // includes connected clips decoded via libraryPath
        bool wave = false;   // audio-peak job — peaks instead of tiles
        int lane = 0;        // 0 = storyline, 1+ = connected lane (waves, and lane tile jobs via libraryPath)
        QStringList keys;
        QList<int> positions; // timeline (or source) frames to decode
        QList<QImage> tiles;  // filled one per event-loop pass
        QVariantMap stripLayout; // clip-relative first frame and frames per tile
        WavePeaks::Source source;  // wave jobs: the sound whose peaks are made
        qint64 waveMade = 0, waveTotal = 0;  // its samples read so far, and in all
        // Wave jobs: a batch of peaks is with the peak thread (makePeaks).
        bool making = false;
        std::shared_ptr<std::atomic_bool> stop;  // set when the job ends
        std::shared_ptr<JobBench> bench;
        bool onWorker = false; // its tiles are drawn on the worker's thread after all (the tile thread's timeline would not build)
        QString playerPath;   // what its player (or a wave's reader) opens, there
    };
    struct StabJob {
        int track = 0, index = 0, shakiness = 4, smoothing = 15;
        QString path, results;
        int in = 0, out = 0, pos = 0;
        double fps = 25;  // the timeline's clock, which in/out count on
        std::unique_ptr<render::MediaSource> source;  // its decoded frames, as the analysis reads them
        std::unique_ptr<render::MotionAnalysis> analysis;
    };
    std::unique_ptr<StabJob> m_stabJob;
    bool m_stabScheduled = false;
    void scheduleStabWork();
    // stabProgress 100, then stabApplied with what the clip's corrections are.
    void announceStabilised(int track, int index, const QString &results, int smoothing);
    struct TrackJob {
        int track=0, index=0, node=0, window=0, start=0, end=0, pos=0, step=1;
        QString nodeId, sequenceId;
        QVariantMap clip, originalWindow;
        QVariantList positions;
        std::unique_ptr<Timeline> timeline;
        WindowTracker tracker;
    };
    void finishWindowTracking(bool keep, const QString &message = QString());
    std::unique_ptr<TrackJob> m_trackJob;
    bool m_trackScheduled = false;
    void scheduleTrackWork();
    void dropTrackJob();
    void processTrackFrame();
    void refreshTwoUp();
    void dropStabJob(); // a new/opened project retires the running analysis
    void processStabFrame();
    // Imports being probed off this thread, in the order asked for (importClip).
    struct Probed {
        bool ok = false, picture = false;
        QVariantMap meta;
        QImage thumbnail;
        int frames = 0;
        double fps = 0;
    };
    struct Import {
        QString path;
        Probed result;      // written by the pool thread, read here once `done`
        bool done = false;  // this thread's
    };
    static Probed probeClip(const QString &path);
    void announceImports();
    void makePeaks(StripJob &job);
    struct WaveBuild;
    struct WaveAnswer;
    void peaksMade(const WaveAnswer &answer);
    void announceWave(const StripJob &job, const std::shared_ptr<WavePeaks::Data> &data);
    QThreadPool m_peakPool;  // one thread: waveform peaks are made off this one
    // Filmstrip tiles are made off this thread as well (makeTiles): one
    // thread, with a copy of the timeline and an engine of its own.
    struct TileBench;
    struct TileAnswer;
    void makeTiles(StripJob &job);
    void endTileThread();
    void tilesMade(const TileAnswer &answer);
    void landTile(StripJob &job, const QImage &tile, const QByteArray &packed);
    void announceStrip(const StripJob &job);
    std::shared_ptr<const QVariantMap> tileShot();
    std::shared_ptr<const QVariantMap> m_tileShot;  // the timeline as tiles are being made of it
    QVariantList m_tileStoryOf, m_tileStory;        // the timeline's clip list, and the storyline's clips picked out of it
    std::shared_ptr<TileBench> m_tileBench;         // the tile thread's
    // This thread's own copy of the storyline, when it draws tiles itself.
    std::unique_ptr<Timeline> m_tileOwn;
    std::shared_ptr<const QVariantMap> m_tileOwnShot;
    // True while the editor plays or exports: a batch under way ends there.
    std::shared_ptr<std::atomic_bool> m_tilesHeld = std::make_shared<std::atomic_bool>(false);
    // The viewer's own pictures come first: how many are being drawn now
    // (a ViewerDraw each, where this thread draws one) and how many times
    // one has been asked for (cancelFrame, from the window's thread). A tile
    // being drawn is given up when either moves, and the next waits.
    std::shared_ptr<std::atomic_int> m_viewerDrawing = std::make_shared<std::atomic_int>(0);
    struct ViewerDraw {
        std::shared_ptr<std::atomic_int> drawing;
        explicit ViewerDraw(PlayerWorker *worker) : drawing(worker->m_viewerDrawing) { drawing->fetch_add(1); }
        ~ViewerDraw() { drawing->fetch_sub(1); }
        ViewerDraw(const ViewerDraw &) = delete;
        ViewerDraw &operator=(const ViewerDraw &) = delete;
    };
    std::shared_ptr<std::atomic<quint64>> m_viewerAsked = std::make_shared<std::atomic<quint64>>(0);
    QThreadPool m_tilePool;
    QTimer m_tileIdle;
    int m_tileBatches = 0, m_tilesDrawn = 0, m_tilesKept = 0, m_tileRebuilds = 0, m_peaksRead = 0, m_peaksKept = 0;
    QList<std::shared_ptr<Import>> m_imports;
    QThreadPool m_probePool;
    QList<StripJob> m_stripJobs;
    QCache<QString, QImage> m_stripCache{64 * 1024}; // key → scaled tile, by source frame
    // Peaks being made, by the sound's id: a job that ends half way (an edit,
    // a scroll) leaves what it made for the next job of that sound. Each is
    // the peak thread's while a batch runs.
    QHash<QString, std::shared_ptr<WaveBuild>> m_waveBuilds;
    // The sounds of the clips in view, held so that the store keeps them
    // however small its spare allowance: an edit reads none of them again.
    QHash<QString, std::shared_ptr<WavePeaks::Data>> m_waveHeld;
    QList<QPair<QString, std::shared_ptr<WavePeaks::Data>>> m_waveLibrary;  // and the bin clips' last made, newest first
    // When a sound's peaks were last found to be of the file as it stands.
    QHash<QString, qint64> m_waveChecked;
    bool m_stripScheduled = false;
    bool m_decorWindowSet = false;
    double m_decorFirst = 0, m_decorLast = 0;
    int m_decorPixels = 0;

    QTimer m_pump;
    QTimer m_exportPoll;
    // Render & Replace in flight: its own timeline and consumer. A
    // frame-blended retime runs an ffmpeg pre-stage first (`pre`) that
    // makes the 1× intermediate the render stage then bakes.
    struct PreJob {
        int at = -1; // index into m_exportEntries
        QString out, stage, error;
        QStringList args;
        std::shared_ptr<QTemporaryDir> directory; // only this job's unfinished file
    };
    struct SinkJob;
    struct RenderJob {
        int track = 0, index = 0, frames = 0;
        QString path;
        QVariantMap original;
        QVariantMap solo; // the entry the scratch timeline restores
        QVariantMap settings, sources; // captured before an asynchronous sampling stage
        std::unique_ptr<Timeline> scratch;
        std::unique_ptr<SinkJob> sink;  // the editor's own encoder sink, on its own thread
        std::shared_ptr<RenderRequest> request;
        PreJob sampling; // outlives the process writing its private staging file
        std::unique_ptr<QProcess> pre;
        int prePercent = 0;
        QString sequenceId; // the render belongs to one sequence of one project…
        int generation = 0; // …and to one loaded project (see m_projectGeneration)
    };
    std::unique_ptr<RenderJob> m_render;
    int m_projectGeneration = 0; // bumped by loadProject / newTimeline
    QTimer m_renderPoll;
    void pollRender();
    void finishRender(bool ok);
    void startRenderStage();
    // Frame-blended retime intermediate (see Timeline::setFrameBlendAt):
    // the cache file for this clip's source span/speed/mode, and the
    // ffmpeg command that makes it (empty when it exists already).
    PreJob blendIntermediate(const QVariantMap &entry, const QVariantMap &profile,
                             const QVariantMap &sources) const;
    static bool wantsBlendRender(const QVariantMap &entry);
    static int ffmpegPercent(QProcess *proc, double seconds, int *last);
    // Export (see the "Export" section of the .cpp): stages, the scratch
    // timeline, checkpoint segments of a paused render stage, the ffmpeg
    // process of the other stages, the log for a failed job.
    enum class ExportStage { Idle, Check, Pre, Render, Concat, Post, Mux };
    ExportStage m_exportStage = ExportStage::Idle;
    QVariantMap m_exportSpec;
    bool m_mediaSourcesDeferred = false;
    QVariantMap m_deferredMediaSources;
    QString m_deferredMediaMode;
    QString m_exportPath;
    // Only this render owns the files inside this unique sibling directory.
    std::unique_ptr<QTemporaryDir> m_exportDirectory;
    QString m_exportWorkingPath;
    // Each finished caption sidecar (written beside its target, hidden) and
    // where it goes: published only after the main output is.
    QList<QPair<QString, QString>> m_exportSidecars;
    int m_exportFrames = 0;     // frames the render stage must produce
    int m_exportRangeIn = 0, m_exportRangeOut = -1;
    int m_exportCheckpoint = 0; // frames already in segments
    QStringList m_exportSegments;
    QList<int> m_exportSegmentFrames; // each segment's picture frames
    // A paused render's sound is rendered again whole, in one pass, once
    // its picture segments are done: the join then has no seam in it.
    bool m_exportAudioPass = false;
    // The levelling gain measured over the whole render range, reused by
    // every stage and the one-pass sound (NaN: not measured yet).
    double m_exportLoudnessGain = std::numeric_limits<double>::quiet_NaN();
    double m_exportLoudnessCeiling = std::numeric_limits<double>::quiet_NaN();  // the limiter's, with the gain
    QString m_exportAudio; // that pass's finished file
    bool m_exportPaused = false;
    bool m_exportCancelled = false;
    QStringList m_exportLog;
    std::unique_ptr<QProcess> m_exportProc;
    int m_exportProcPercent = 0;
    int m_exportReportedPercent = 0;
    QList<PreJob> m_exportPre;
    int m_exportPreDone = 0;
    QVariantList m_exportEntries;
    QVariantMap m_exportContext;
    double m_exportFps = 25;
    QVariantMap renderSnapshot();
    std::unique_ptr<Timeline> m_exportScratch;
    std::shared_ptr<RenderRequest> m_exportRenderRequest;
    // The editor's own encoder sink at work: a thread
    // writing the stage's file, watched by pollExport. Declared after the
    // scratch timeline it reads, so it stops first.
    struct SinkJob {
        std::thread thread;
        std::atomic<int> written{0};
        std::atomic<bool> stop{false}, done{false};
        bool ok = false, renderFailed = false;  // read once done
        QString error;
        // Levelled sound: the gain and limiter ceiling found for the whole
        // range (NaN: not measured here), what was measured, how the check
        // of the sound as delivered went, and what went out.
        double gain = std::numeric_limits<double>::quiet_NaN(), ceiling = std::numeric_limits<double>::quiet_NaN();
        QVariantMap measured, delivered;
        QString checked;
        std::shared_ptr<RenderRequest> request = std::make_shared<RenderRequest>();
        ~SinkJob() { stop = true; request->cancel(); if (thread.joinable()) thread.join(); }
    };
    std::unique_ptr<SinkJob> m_exportSink;
    std::unique_ptr<QTemporaryDir> m_exportNests;  // nested sequences' files for the export's scratch timeline
    bool m_exportRenderFailed = false;  // the sink could not make a picture or sound
    bool m_exportSinkFailed = false;    // the sink's own encoder stopped the render stage
    // Held while the export's working folder is in use: a later export removes
    // working folders whose render was killed, never one still owned.
    std::unique_ptr<QLockFile> m_exportOwner;
    void startExportSink(const QString &part);
    QString exportProblem();
    // `cancel`: abandon the frame in hand too (the file is thrown away).
    void stopExportSink(bool cancel = false);
    void continueExportPre();
    bool buildExportScratch();
    void startExportStage();
    void finishExportStage(bool ok);
    void afterRenderStage(const QString &part);
    void muxOrFinish(const QString &part);
    void finishDelivery(const QString &part);
    void endExport(bool ok, const QString &reason);
    void reportExportProgress(int stagePercent);
    void runExportProcess(QStringList args, double seconds, const std::function<void()> &done);
    void appendExportLog(const QString &line);
    QString exportStageTarget() const;
    int exportStageStart() const;
    void joinExportSegments();
    // Caption cues clipped to the render range, timed from its start.
    QVariantList exportCues(const QVariantList &all) const;

    // The device: fed by the pump itself (feedSink), or, when m_fed is set,
    // by a thread of its own from a ring the pump only appends to.
    std::unique_ptr<PlaybackAudioOutput> m_audioOutput;
    bool m_audioFailed = false;
    QString m_audioOutputId;
    int m_audioBufferFrames = 6;
    bool projectRateLocked() const;
    int m_position = 0;
    double m_speed = 0.0;
    QSize m_playbackDrawSize;     // see setPlaybackDrawSize
    QVariant m_playbackDisplay;   // see setPlaybackDisplay
    QVariant m_playbackScopes;    // see setPlaybackScopes (the worker's thread only)
    bool m_playedSmall = false;   // the picture on screen was drawn at it, or for the display only
    // Seeks and skims shown the quick way. All of it belongs to the worker's
    // thread: the timer is its child, and only its slots read these.
    QTimer m_settle;              // runs out once the playhead (or the skim) has rested
    bool m_settleSeek = false;    // the playhead's picture stands for a working frame not yet drawn
    int m_settleSkim = -1;        // and so does the skimmed frame's (-1: none)
    QSize m_sourceDrawSize;       // see setSourceDrawSize
    QVariant m_sourceDisplay;     // see setSourceDisplay
    bool m_settleSource = false;  // the source viewer's picture stands for a working frame not yet drawn
    quint64 m_speedRequest = 0;

    QVariantMap m_pendingTag; // multicam tag for the placement in flight
    void applyPendingTag(int track, int position); // clip starting there
    void applyPendingTagAt(int track, int index);
    // The multicam viewer's decoders, one per angle, and the one thread that
    // reads them: a grid of pictures is never made on the thread that plays.
    struct AngleBench { QHash<QString, std::shared_ptr<MediaPlayer>> players; };
    std::shared_ptr<AngleBench> m_angleBench = std::make_shared<AngleBench>();
    QThreadPool m_anglePool;

    MediaPlayer m_preview;
    void describePreview();
    QString m_previewPath;
    bool m_sourceMode = false;
    int m_previewPos = 0;

    // Audio page state.
    LoudnessMeter m_loudness;
    int m_loudnessTick = 0;
    bool m_loop = false;
    int m_sourceLoopIn = -1, m_sourceLoopOut = -1;
    int m_loopIn = -1, m_loopOut = -1;
    // `written`: the frames of the last two keys this pass wrote (-1: none yet).
    struct AutomationWrite {int track=0;QString key;double value=0;QVariantMap original;int written[2]={-1,-1};};
    QMap<QString, AutomationWrite> m_automationWrites;
    bool m_automationRecorded=false;
    bool m_automationUnannounced=false; // keys written since the timeline was last announced
    int m_editRevision=0, m_mixRevision=-1;
    QString m_mixUndoKey;
    QSet<QString> m_mixGestures;
    QElapsedTimer m_mixUndoClock;
    void recordMixEdit(const QVariantList &before, const QString &key);
    void writeAutomation(int frame);
    void announceMix();
    QVariantMap m_lastLoudnessAnalysis;
    QByteArray m_loudnessFingerprint;
    struct Analysis {
        int start = 0;
        QByteArray fingerprint;
        std::unique_ptr<Timeline> scratch;
        LoudnessMeter meter;
        int position = 0;
        int total = 0;
    };
    std::unique_ptr<Analysis> m_analysis;
    void continueAnalysis();
    void feedMeters(const QByteArray &audio);
    static QList<float> spectrumOf(const QByteArray &audio, int channels, int rate);
};
