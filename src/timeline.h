#pragma once
#include "audioengine.h"
#include "framesampling.h"
#include "mixshape.h"

#include <QElapsedTimer>
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVariantList>
#include <map>
#include <memory>
#include <optional>
#include <functional>
#include <vector>

#include "mediaplayer.h"
#include "exportspec.h"

#include "graph.h"

// The primary storyline: one edit::Playlist. Appending ripples clips onto the
// end, removing ripples the rest left, splitting blades one clip in two —
// ripple-editing semantics, as the edit graph keeps them. The profile
// locks to the first appended clip; the engine and the mixer conform later
// clips to it.
class LinearRenderer;
namespace engine { class Engine; struct OutputColour; struct YuvFrame; }
namespace render { class TimelineRenderer; }
namespace sound { class Mixer; struct MixMemory; }
class RenderRequest;
struct ClipEntry;

class Timeline {
public:
    // More tracks than any project has: a track number above this in a
    // snapshot or a project file is damage, not a track to make.
    static constexpr int kMaxTracks = 999;
    Timeline();
    ~Timeline();

    // One timeline track — the edit-page unit. `kind` is what the header
    // shows (V/A); an empty track may still be claimed by either kind.
    struct Track {
        enum Kind { Video, Audio };
        Kind kind = Video;
        std::unique_ptr<edit::Playlist> playlist;
        bool locked = false;     // refuses every edit, never rides a ripple
        bool disabled = false;   // video: picture hidden; audio: muted
        bool muted = false;      // this track's sound off (any kind)
        bool solo = false;       // only this track's sound (any kind)
        bool autoSelect = true;  // rides ripples from other tracks
        QString name;            // header rename ("" = the V1/A1 convention)
        // Audio-page mix state (role, color, volume dB, pan, armed,
        // monitor, input, output, sends[], fx[]) — see trackMix().
        QVariantMap mix;
        // Authoritative sample-precise cuts; playlist is only their render graph.
        // Empty keeps the legacy frame-based playlist model unchanged.
        QVariantList audioEntries;
    };
    int trackCount() const { return int(m_tracks.size()); }
    // A track's header state without its playlist — snapshots for restore()
    // and the project-file marker.
    struct TrackFlags {
        Track::Kind kind; bool locked, disabled, muted, solo, autoSelect;
        QString name;
        QVariantMap mix;
    };
    // Header state, QML-ready: [{index, name, kind: "video"|"audio",
    // locked, disabled, muted, solo, autoSelect, clips}]. Names follow the
    // edit-page convention — V1.. up the video tracks, A1.. down the audio
    // ones, by track order.
    QVariantList tracks() const;
    QString trackName(int track) const;
    Track::Kind trackKind(int track) const; // content decides; empty = stored
    bool trackLocked(int track) const;
    int trackClipCount(int track) const;
    // Flag setters — not undoable (edit-page convention); tractor() honours
    // disabled/muted/solo via the multitrack "hide" property.
    bool setTrackLocked(int track, bool locked);
    bool setTrackDisabled(int track, bool disabled);
    bool setTrackMuted(int track, bool muted);
    bool setTrackSolo(int track, bool solo);
    bool setTrackAutoSelect(int track, bool autoSelect);
    // Header rename (double-click the badge); "" goes back to V1/A1. Not
    // undoable, like the flags; survives rebuilds and the project file.
    bool setTrackName(int track, const QString &name);
    // User-managed tracks (edit page): tracks persist while empty, survive
    // undo (the flag snapshot recreates them) and are only removed on
    // request — never V1, never a track holding clips.
    bool addTrack(bool audio);
    bool removeTrack(int track);
    // Reorder: swap two user tracks' slots (never V1). Track order IS the
    // compositing order, so swapping two video tracks swaps which is on
    // top; unnamed tracks renumber (V2/V3, A1/A2) with the order.
    bool swapTracks(int a, int b);
    // The neighbour of `track`'s kind one step up (up == true) or down the
    // edit page — video tracks stack upwards from V1, audio ones hang
    // below it — or -1 when the track is already at that end / is V1.
    int trackNeighbour(int track, bool up) const;
    // ── Color page: a node-graph grade per clip ────────────────────────
    // grade = {nodes: [{id, label, enabled, primaries, curves, qualifier,
    // window, lut, keyframes}], input, output} — see defaultGradeNode() for
    // every key. Stored as JSON on the cut (omavid:grade carrier) and
    // rebuilt as ordered filters marked omavid:gradeFx (a masked node is
    // mask_start … mask_apply). An empty map clears the grade. Rides
    // entries()/restore like every attribute; "grade" is a bypass key.
    bool setGradeAt(int track, int index, const QVariantMap &grade);
    QVariantMap gradeAt(int track, int index) const;
    // Versions: [{name, grade}] with the active one applied.
    bool setGradeVersionsAt(int track, int index, const QVariantList &versions, int active);
    // A shared look (versions + applied grade) arriving from another member
    // of its colour group. Unlike the setters above it writes through a
    // track lock: the lock stops a clip joining or leaving a group, not the
    // group's look from reaching it.
    bool setSharedLookAt(int track, int index, const QVariantList &versions, int active,
                         const QVariantMap &grade);
    // Atomic named-version commands against the latest grade. Empty error
    // on false means a no-op; failures explain the missing/locked target.
    bool editGradeVersionAt(int track, int index, const QString &action,
                            int version, const QString &name, QString *error = nullptr);
    // Paste a node after afterNode, or replace the active grade. Returns the
    // node to select, or -1 with an error. Other versions/attributes survive.
    int pasteColorAt(int track, int index, const QString &kind, const QVariantMap &payload,
                     int afterNode, QString *error = nullptr, const QString &parentPath = QString());
    // Estimate neutral RGB gains from this clip's rendered frame and append
    // an editable node. Returns its index, or -1 with a reason and no edit.
    // One clip's frame as graded, drawn alone (no other tracks or master).
    QImage isolatedFrameAt(int track, int index, int clipFrame, QString *error = nullptr);
    // Modify ▸ Match Colour: a "Match Colour" correction taking the clip's
    // channel averages at clipFrame to the reference clip's at refFrame.
    // Its node index, or -1 with the reason in error.
    int matchColorAt(int track, int index, int clipFrame, int refTrack, int refIndex, int refFrame, QString *error = nullptr);
    int autoBalanceAt(int track, int index, int clipFrame, QString *error = nullptr);
    QImage effectInputAt(int track, int index, const QString &uid, int clipFrame, QString *error = nullptr);
    // The clip alone, its effects on it, at most `most`: over a check where it
    // is clear, or its matte in grey. Tagged working light, as the viewer takes.
    QImage effectPreviewAt(int track, int index, int clipFrame, bool matte, const QSize &most, QString *error = nullptr);
    bool setNodeLutAt(int track, int index, int node, const QString &nodeId,
                      const QString &path, double intensity, QString *error = nullptr);
    bool previewNodeLutAt(int track, int index, int node, const QString &nodeId,
                          int clipFrame, const QString &path, double intensity,
                          QImage *before, QImage *after, QString *error);
    QImage colorMatteAt(int track, int index, const QString &nodeId, int frame, QString *error);
    static QStringList gradeLutProblems(const QVariantMap &clip);
    static QVariantMap defaultGradeNode(const QString &label = QString());
    static QVariantMap defaultGrade();
    static bool gradeIsIdentity(const QVariantMap &grade);
    // The filter a grade becomes (the colour engine's), for tests and the
    // export scratch; none for an identity grade.
    static void applyGradeFilters(edit::Profile &profile, edit::Producer *cut,
                                  const QVariantMap &grade, int cutIn, int cutOut);

    // ── Audio page: per-track mix state and the master ─────────────────
    // A track's mix map: role ("dialogue" "ambience" "music" "effects"
    // "foley" "roomtone" "submix"), color, volume (dB), pan (-1..1), armed,
    // monitor, input, output ("master" / "bus:<n>"), sends [{bus, level,
    // pre}], fx [{id, bypass, mix, params}] — every key optional, absent =
    // default. Rides the flag snapshot (survives undo/restore) and the
    // project file (omavid:trackMix JSON); not undoable, like the flags.
    // Setting it rebuilds that track's real filter chain: fx inserts in
    // order → volume → pan → the level tap (refreshTrackFilters).
    QVariantMap trackMix(int track) const;
    bool setTrackMix(int track, const QString &key, const QVariant &value);
    bool setTrackMixMap(int track, const QVariantMap &mix); // replace
    // The master: the same map shape (volume, pan, fx), applied on the
    // tractor. Persisted as omavid:masterMix on V1's meta filter.
    QVariantMap masterMix() const { return m_master; }
    bool setMasterMix(const QString &key, const QVariant &value);
    bool setMasterMixMap(const QVariantMap &mix);
    bool masterActive() const; // anything on it that changes the sound
    // ADR cue list (Audio page): [{id, character, line, notes, in, out,
    // preRoll, postRoll, track, status, takes: [{path, rating, promoted}]}].
    // Project data, persisted beside the master mix; not undoable.
    QVariantList adrCues() const { return m_adrCues; }
    void setAdrCues(const QVariantList &cues) { m_adrCues = cues; }
    // The timeline's caption settings: {rules: the caption checks'
    // thresholds and Generate Captions' two-speaker choice (only the keys
    // set; see CaptionChecks), speakers: [{name, colour}]}. Project data
    // beside the ADR cues ("captions" in the project file, omavid:captions
    // in an undo snapshot); empty in a project from before it.
    QVariantMap captionSettings() const { return m_captionSettings; }
    void setCaptionSettings(const QVariantMap &settings) { m_captionSettings = settings; }
    // Project settings (File ▸ Project Settings…): {custom, width, height,
    // fpsNum, fpsDen, colorspace (601|709|2020), sampleRate, channels,
    // startFrame, startRate}. With `custom` off the frame size and rate follow the
    // first clip, as they always did; the colour space applies either way.
    // Persisted as omavid:project on V1's meta filter, beside the master
    // mix; read back BEFORE the clips are restored so the profile lock
    // honours it. Not undoable.
    QVariantMap projectSettings() const { return m_project; }
    QVariantMap snapshotDocument(const QStringList &library) const;
    static QVariantMap defaultProjectSettings();
    // Stores the settings; true when the profile SHAPE (size, rate, colour
    // space) changed and the caller should rebuild the timeline against it.
    // `ownClock` false: these are another timeline's settings (a project
    // opened, a timeline switched to or brought in), arriving while the clock
    // of the one they replace still runs, and a rebuild follows that locks
    // their own; their start is not counted at the clock being replaced.
    bool setProjectSettings(const QVariantMap &settings, bool ownClock = true);
    int projectStartFrame() const { return m_project.value(QStringLiteral("startFrame")).toInt(); }
    // The starting timecode is held as a frame count (startFrame) beside the
    // whole frames a second it was counted at (startRate; 0 is the timeline's
    // own rate, as every project had it before the rate was written down). A
    // start typed before the rate is known, or kept through a change of rate,
    // is counted again at the timeline's rate so it reads as it was typed.
    // This is that count: the same hours, minutes, seconds and frame number.
    static int startFrameAtRate(int frame, int fromRate, int toRate);
    // One frame's mixed audio (interleaved s16) without decoding a picture
    // — the offline loudness pass.
    QByteArray audioAt(int position, int frequency, int channels);
    // The same frame's mix as interleaved floats; empty with `error` set when it fails.
    std::vector<float> soundAt(int position, int frequency, int channels, QString *error = nullptr);
    // Level taps: per-track peak L/R (0..1 linear) of the LAST decoded
    // frames, as [inL, inR, postL, postR, outL, outR] per track — before
    // the inserts, after them (pre-fader) and after the fader/pan. Taking
    // them clears them.
    QList<float> takeTrackPeaks();
    // Which description of the timeline's sound a frame mixed now would be
    // made from: the number changes when a clip, a track's or the master's mix
    // map, a media setting or the rate has changed since sound was last asked
    // for, and only then. Sound made under one number stays what the timeline
    // sounds like until the number moves (playback mixes ahead on this).
    quint64 soundGeneration();
    // The mixer's memory of its inserts, for playback that mixes ahead and may
    // have to mix again from an earlier frame (sound::MixMemory): after
    // audioAt, what the inserts held before that frame; recalled newest first.
    void rememberSound(bool on);
    std::shared_ptr<const sound::MixMemory> takeSoundMemory();
    void recallSound(const sound::MixMemory &memory);
    QVariantList audioRoutingMeters() { return m_audioRouting.meters(); }
    class MeterScope {
    public:
        MeterScope(Timeline &timeline, int position, qint64 firstSample = -1,
                   int frequency = 48000, qint64 lastSample = -1, bool retain = false);
        ~MeterScope();
        MeterScope(const MeterScope &) = delete;
        MeterScope &operator=(const MeterScope &) = delete;
        std::shared_ptr<const AudioEngine::MeterCapture> capture() const { return m_capture; }
    private:
        Timeline &m_timeline;
        AudioEngine::MeterWindow m_previous;
        std::shared_ptr<AudioEngine::MeterCapture> m_capture;
    };
    // The fx ids the chain knows, for a UI's catalogue.
    static QStringList mixEffectIds();
    // [{id, name, mixable, params: {key: default}}] — the inserts the mixer
    // has, for the Effects tab.
    static QVariantList mixEffectCatalogue();

    // Header state as QVariant (for a worker's undo sentinel) and back.
    QVariantList trackFlagsVariant() const;
    static std::vector<TrackFlags> trackFlagsFromVariant(const QVariantList &list);
    int trackFrameCount(int track) const; // that track's own end

    // ── Uniform clip address: (track, index among that track's clips) ──
    // Every per-clip adjustment below works on ANY track; the older
    // storyline (index) / connected (flat index) entry points are shims.
    bool setAudioFadeCurveAt(int track, int index, const QString &curve);
    bool setVolumeAt(int track, int index, double db);
    bool setVolumeKfAt(int track, int index, const QVariantList &kf);
    bool setPanAt(int track, int index, double pan);
    bool setEqAt(int track, int index, const QString &preset);
    bool setOpacityAt(int track, int index, double alpha);
    bool setOpacityKfAt(int track, int index, const QVariantList &kf);
    // Picture-in-picture rect (fractions of the frame), rotation in
    // degrees, and the anchor the rotation turns about as a fraction of the
    // rect (0.5 0.5 = its centre). The transform rotates about the rect
    // centre only, so the anchor is honoured by offsetting the rect it is
    // handed: rotating about P equals rotating about the centre C then
    // moving by (P − C) − R(P − C). Entries: rectX/Y/W/H, rotation,
    // anchorX/anchorY.
    bool setRectAt(int track, int index, double x, double y, double w, double h,
                   double rotation = 0.0, double anchorX = 0.5, double anchorY = 0.5);
    // Crop fractions off the left/right/top/bottom edges of the clip's own
    // picture (applied before its transform). A hard crop is one qtcrop on
    // the cut; a feathered (soft-edged, 0…1 of the picture) or inverted
    // (the box is what goes) crop is frei0r.alphaspot lowering the alpha
    // instead — the tractor composites it. Entries: cropL/R/T/B,
    // cropFeather, cropInvert.
    bool setCropAt(int track, int index, double l, double r, double t, double b,
                   double feather = 0.0, bool invert = false);
    bool setClipCrop(int index, double l, double r, double t, double b,
                     double feather = 0.0, bool invert = false);
    bool setConnectedCrop(int index, double l, double r, double t, double b,
                          double feather = 0.0, bool invert = false);
    // Rotation in degrees about the clip's anchor (affine on the cut).
    bool setConnectedRotation(int index, double degrees);
    bool setConnectedAnchor(int index, double anchorX, double anchorY);
    // Frame sampling IDs are defined in framesampling.h and stored on the cut.
    // Explicit Floor/Nearest apply in native and linear rendering. Legacy
    // constant blend/flow still use the worker's external bake path.
    bool setEditProperties(int track, int index, const QVariantMap &patch);
    bool applyMotionPresetAt(int track, int index, const QString &preset,
                             const QVariantMap &options, QString *error = nullptr);
    void applyEditExtras(int track, int index, const ClipEntry &entry);
    bool setFrameBlendAt(int track, int index, int mode);
    // Adjustment clip: a transparent lane clip (color:#00000000) tagged
    // omavid:adjust. Its colour / lens / flip / crop filters are copied
    // onto the tractor over the clip's span when the composite is built,
    // so they grade everything under it (the whole composite — tracks
    // above it too: there is no per-track stack). Entries: adjust,
    // generator "adjust". Generators (solid / bars / noise / count) are
    // plain producer-string clips; entries: generator, generatorColor.
    bool setAdjustAt(int track, int index, bool adjust);
    static QString generatorResource(const QString &kind, const QString &color);
    static QString generatorKindOf(const QString &path);
    bool setColorAt(int track, int index, double brightness, double contrast,
                    double saturation);
    bool setDisabledAt(int track, int index, bool disabled);
    // Clip colour label ("" clears): a marked identity filter on the cut,
    // so it survives XML like every other cut attribute. Entries carry it
    // as `label`; the block tints by it.
    bool setLabelAt(int track, int index, const QString &label);
    bool setCategoryAt(int track, int index, const QString &category);
    // Link id ("" clears): a video clip and the audio detached from it
    // share one, so linked selection can pick, move and delete the pair
    // together. Same carrier as the label; entries report it as `link`.
    bool setLinkAt(int track, int index, const QString &link);
    // Clip markers — {frame (a SOURCE frame), type, text, notes} — ride
    // the cut as one JSON marker filter (omavid:markers), so trims, moves,
    // snapshots, paste and the project file keep them. Entries: `markers`.
    bool setClipMarkersAt(int track, int index, const QVariantList &markers);
    // Render & Replace: the entry map the rendered clip replaced, as a
    // tagged carrier (omavid:original) — Restore Original pastes it back.
    bool setOriginalAt(int track, int index, const QVariantMap &original);
    // Linked audio on its own track. settleLinkedAudio runs after every
    // storyline edit: any V1 clip still carrying its sound is split — the
    // video stays on V1, the audio lands on an audio track, both linked —
    // and every linked audio clip whose video moved/trimmed/retimed/faded/
    // dissolved since `before` is conformed to it. Returns true when it
    // changed anything.
    // `keepOverwritten`: the edit laid a picture alone over the timeline, so
    // the sound of what it covered stays where it is.
    bool settleLinkedAudio(const QVariantList &before, bool keepOverwritten = false);
    // The track a clip's sound alone is put on, as the sound of a whole edit
    // would be: the first unlocked audio track (free over [start, start+span)
    // when `free` is asked for), else one past the last, a new track.
    int soundTrackFor(int start, int span, bool free) const;
    // A recording: a file with sound and no picture. It has only its sound
    // to give, so an edit from the bin places it as a sound on an audio
    // track and nothing asks it for pictures.
    bool soundOnlySource(const QString &path) const;
    // Its four steps, in the order they run.
    bool removeOverwrittenSound(const QVariantList &before);
    bool detachPrimarySound();
    bool detachUpperTrackSound();
    bool conformLinkedSound(const QVariantList &before);
    bool pathHasAudio(const QString &path);
    // Multicam audio-follows-video: off, a linked audio clip keeps its own
    // angle when the video switches (it still rides trims by the same deltas).
    // (A clip with a sound source of its own, fixedSoundOf, pays it no heed.)
    void setAudioFollowsVideo(bool on) { m_audioFollowsVideo = on; }
    // The attributes an entries() map carries, re-applied to a clip (paste,
    // blade, restyle, render); a clip's map by address; the clip
    // whose span holds a frame.
    void applyEntryAttributes(int track, int index, const QVariantMap &entry);
    void applyEntryAttributes(int track, int index, const ClipEntry &entry);
    void decorateSplit(int track, int index, const QVariantMap &entry, int relative);
    // Boundaries for an edit that carves a span out of a track through the
    // playlist: every surviving piece of a cut clip keeps its attributes.
    bool cutKeepingAttributes(int track, int position, bool rightKeepsLink);
    void keepCarvedClips(int track, int from, int to);
    int clipIndexContaining(int track, int position) const;
    QVariantMap entryMapOn(int track, int index) const;
    bool setAudioTimingAt(int track, int index, const QVariantMap &timing, QString *error = nullptr);
    bool splitAudioAt(int track, int index, const QVariantMap &position, QString *error = nullptr);
    bool gestureAudioAt(int track, int index, const QString &action, const QVariantMap &changes,
                        int sampleRate, QVariantMap *selected = nullptr, QString *error = nullptr);
    // Sound-only transaction foundation. Insert ripples the affected sound
    // tracks once; callers coordinating picture/linked edits need a wider plan.
    bool pasteAudioClips(const QVariantList &copies, int sourceNum, int sourceDen,
                         const QVariantMap &position, bool insert,
                         QVariantList *added = nullptr, QString *error = nullptr);
    // Atomic picture/sound placement with one exact group origin. Insert opens
    // the group span once, respecting locks and other tracks' auto-selection.
    bool pasteClipboardClips(const QVariantList &copies, int sourceNum, int sourceDen,
                             const QVariantMap &position, QVariantList *added = nullptr,
                             QString *error = nullptr, bool insert = false);
private:
    QHash<QString, bool> m_audioProbe; // path → has an audio stream
    int m_brokenDissolves = 0;
    bool m_rippleFailed = false;
    // Breaking a dissolve rebuilds the timeline, and a rebuild with nothing to
    // reuse replaces the profile: a producer opened before it must be reopened.
    bool hasLaneDissolve() const;
    int breakLaneDissolves(int track, int from, int to);
    int breakRippledLaneDissolves(int sourceTrack, int position);
    int placementSpan(const QString &path, int in, int out, bool audioOnly, double speed, bool videoOnly);
    bool m_linkingSplit = false;       // a blade is splitting its linked mate
    bool m_audioFollowsVideo = true;
public:
    // Composite blend mode: the qtblend_mode filter tells the track's
    // qtblend transition how to composite THIS clip (a Qt composition
    // mode: 0 normal, 12 add, 13 multiply, 14 screen, 15 overlay, 16
    // darken, 17 lighten, 18 colour dodge, 19 colour burn, 20 hard light,
    // 21 soft light, 22 difference, 23 exclusion). Entries: `blend`.
    bool setBlendAt(int track, int index, int mode);
    // How this clip takes a frame of another size or shape ("fit", "fill",
    // "stretch", "original"); empty follows the timeline's own `scaling`.
    // Entries: `scaling` (present when the clip chooses).
    bool setScalingAt(int track, int index, const QString &scaling);
    // Lens correction (avfilter.lenscorrection): k1/k2 −1…1, centre 0…1.
    // Entries: lensK1 lensK2 lensCx lensCy (present when applied).
    bool setLensAt(int track, int index, double k1, double k2, double cx, double cy);
    bool setVideoEffectsAt(int track, int index, const QVariantList &effects,
                           QString *error = nullptr);
    // action: add (id + optional params), update (params/bypass), reset,
    // remove, move (direction -1/+1). Existing effects are addressed by uid.
    bool editVideoEffectAt(int track, int index, const QString &action,
                           const QString &uid, const QVariantMap &values, QString *error = nullptr);
    // Stabilisation: the vidstab filter APPLYING an analysis file (the
    // worker's background pass writes it). Empty `results` removes it.
    // Entries: stab {results, shakiness, smoothing, missing}.
    bool setStabAt(int track, int index, const QString &results, int shakiness, int smoothing);
    // Captions: SubRip / WebVTT text ↔ cues [{start, frames, text}] at
    // `fps`. Pure text helpers; the worker places cues as caption-tagged
    // titles (style key `caption`) on a "Captions" track.
    static QVariantList parseSubtitles(const QString &text, double fps);
    static QString formatSrt(const QVariantList &cues, double fps);
    // The same cues as a WebVTT file: its header, dotted time stamps, and
    // the characters its text may not hold written as entities.
    static QString formatVtt(const QVariantList &cues, double fps);
    // Whether this filter / producer / transition is offered (not
    // quarantined); Deliver's pre-flight fails a render on one that is not.
    static bool serviceAvailable(const QString &type, const QString &name);
    // Preferences ▸ Plug-ins: a provider ("frei0r", "avfilter", "vidstab",
    // "opencv") switched off, or a service quarantined after failing to
    // load, is reported unavailable even though the repository lists it.
    static bool serviceListed(const QString &type, const QString &name);
    static QString providerOf(const QString &name);
    static void setDisabledProviders(const QSet<QString> &providers);
    static QSet<QString> disabledProviders();
    static void setQuarantinedServices(const QVariantMap &services); // name → reason
    static QVariantMap quarantinedServices();
    QString newLinkId() const;
    // ── Multicam ──────────────────────────────────────────────────────────
    // A placed multicam clip is an ordinary clip of ONE angle, tagged with
    // the multicam description (compact JSON: {name, angles:[{path, name,
    // offset, frames}]}) and the angle it shows. Both ride a marked
    // identity filter on the cut (omavid:mcam / omavid:mcamAngle), like the
    // colour label, so snapshots, paste and the project file keep them.
    // "" clears the tag. Entries carry them as `mcam` / `mcamAngle`.
    bool setMulticamAt(int track, int index, const QString &json, int angle);
    // Show another angle for the same span: the clip's source range maps
    // through the two angles' offsets (virtual = in + offset[angle]). A
    // snapshot rebuild, so every other attribute (volume, label, fades,
    // crop…) is kept. Where the new angle has no footage for part of the
    // span, that part is a piece of its own that shows black and plays no
    // sound (angleWithoutFootage); the whole segment changes, those pieces
    // with it. Refused only for a retimed, held or reversed segment, or one
    // in a transition, that the angle does not cover.
    bool switchAngleOn(int track, int index, int angle);
    // Cut to an angle at `frame`: blade the clip there (when the frame is
    // strictly inside it) and switch the part AFTER the cut, to the end of
    // the segment. Refused when the frame is outside the clip.
    bool cutToAngleOn(int track, int index, int frame, int angle);
    // A stretch of a multicam segment its angle has no footage for: the
    // multicam clip still, on that angle, drawn as an opaque black solid of
    // its own kind (never one the editor can make), its frames the multicam
    // clip's own.
    static QString noFootagePath() { return QStringLiteral("color:#ff000000"); }
    static constexpr int kNoFootageFrames = 100000000;
    static bool angleWithoutFootage(const QVariantMap &entry);
    // A multicam clip made with a sound source of its own (one angle's sound,
    // or a recording that is no picture angle): the description's `sound`,
    // shaped as an angle is. Its linked sound plays that source at the
    // picture's moment whatever angle the picture shows, so an angle cut or
    // switch changes the picture only. {} for a clip whose sound follows its
    // picture (every clip made before the choice existed).
    static QVariantMap fixedSoundOf(const QVariantMap &entry);
    // The sound source chosen after the clip is made. A source has a number:
    // -1 the sound follows the picture; 0… an angle (its own sound); after
    // the angles, the recordings lined up with the clip that are no angle.
    // Those are kept in the description's `recordings` so that one not in
    // use is not lost (a clip made before that list was kept names its one
    // recording by `sound` alone, and reads the same). `camera` is the
    // description: a placed clip's tag, or a library item.
    static QVariantList soundSourcesIn(const QVariantMap &camera); // each {source, name, path, recording}
    static int soundSourceIn(const QVariantMap &camera);
    static bool chooseSoundSourceIn(QVariantMap &camera, int source);
    // The pieces of the placed multicam clip that clip `index` is one of:
    // those of the same multicam clip on its track that still lie where the
    // placed clip put them (in step with it), or that touch such a piece (a
    // ripple closed the space between them). Indices on `track`, in order.
    QList<int> placedPiecesOf(int track, int index) const;
    // Every piece of that placed clip takes its sound from `source`: the
    // picture cuts stay as they are, and each piece's linked sound becomes
    // what it would be had the clip been made so (conformLinkedSound). A
    // sound the editor removed or unlinked is not made again.
    struct SoundChange {
        int pieces = 0;   // of the placed clip
        int changed = 0;  // given the new source
        int removed = 0;  // of those, left with no sound because the editor removed or unlinked theirs
        int retimed = 0;  // of those, retimed or held pieces that lost their sound (the source does not cover all of them)
        QString refused;  // why nothing was changed
    };
    bool setSoundSourceOn(int track, int index, int source, SoundChange *done);
    // Collapse Multicam: every piece of a multicam clip becomes an ordinary
    // clip of the angle it shows now (a stretch without footage stays the
    // black it is). Only the tag goes: the cuts, the sources, their frames
    // and everything set on the clips stay, so the picture and the sound are
    // what they were. The sound of a clip that follows its picture stays
    // linked to its piece where it is the piece's own (the same file): the
    // one sound a link can hold, as the next edit of the picture writes the
    // picture's own sound over whatever it is linked to. Any other sound is
    // left where it is as a sound clip of its own, the pair unlinked: all the
    // sound of a clip with a sound source of its own (one run of a recording
    // or of one angle under every picture cut, which a later roll or trim of
    // the pictures above then leaves alone), and a sound kept on another
    // angle. Pieces inside a compound clip are collapsed with the rest.
    // Nothing is changed while a track that holds a piece, or a sound to be
    // unlinked, is locked.
    struct Collapse {
        QVariantList entries; // the clips afterwards
        int pieces = 0;       // pictures that were multicam
        int unlinked = 0;     // sounds left as clips of their own, unlinked
        int lockedTrack = -1; // a locked track in the way: nothing is to be changed
    };
    static int multicamPiecesIn(const QVariantList &entries);
    static Collapse multicamCollapsed(const QVariantList &entries, const QSet<int> &lockedTracks);
    bool collapseMulticam(Collapse *done);
    // Clip `index` and the next are two parts of one multicam clip cut to
    // different angles at the same moment (an angle cut, not two takes).
    bool angleCutOn(int track, int index) const;
    // Detach Audio at any address (the storyline detachAudio generalised):
    // the clip keeps its picture only, its sound lands as a connected
    // audio-only clip on an audio track over the same frames.
    bool detachAudioOn(int track, int index);
    // ── Flip / freeze frame / reattach ─────────────────────────────────
    // Flip the picture (avfilter.hflip / vflip on the cut, marked
    // omavid:flip; entries carry flipH / flipV). Any track.
    bool setFlipAt(int track, int index, bool h, bool v);
    // Hold one source frame across the whole cut (the "freeze" filter,
    // marked omavid:freeze = the source frame; entries carry `freeze`).
    bool setFreezeAt(int track, int index, int sourceFrame, bool on);
    // Freeze Frame at `frame` inside the clip: blade there and INSERT a
    // `length`-frame still of that moment (video-only, no sound). V1
    // ripples the auto-selected tracks like any insert; a lane shifts its
    // own later clips. Refused on gaps, titles, audio, dissolved edges.
    bool freezeFrameOn(int track, int index, int frame, int length);
    // The inverse of Detach Audio: a video-only clip takes back the
    // audio-only clip that still mirrors it (same file, frames, start and
    // speed); refused when that clip has moved or been trimmed away.
    bool reattachAudioOn(int track, int index);
    int sourceFrameCount(const QString &path); // a file's length in frames
    // Audio channel mapping ("" stereo as recorded, "mono" fold both to
    // the centre, "swap" L↔R, "left" / "right" that side on both) and
    // loudness normalisation (dynamic_loudness to −23 LUFS). Marked
    // filters on the cut; entries carry `audioMap` / `normalize`.
    bool setAudioMapAt(int track, int index, const QString &mode);
    bool setNormalizeAt(int track, int index, bool on);
    // Helpers for the angle maths, shared with the Backend's viewer.
    struct Angle { QString path; QString name; int offset = 0; int frames = 0; };
    static QList<Angle> anglesOf(const QString &json);
    static QList<int> multicamNativeFrames(const QVariantMap &entry, int relativeFrame, double timelineFps);
    bool angleSwitchAudioLocked(int track, int index) const;
private:
    bool switchAngleInEntry(QVariantMap &map, int angle);
    bool switchCoveredAngleOn(int track, int index, int angle);
    bool cutToCoveredAngleOn(int track, int index, int frame, int angle);
    bool angleCoverage(const QVariantMap &entry, int angle, int *lead, int *covered);
    bool showNoFootageIn(QVariantMap &map, int angle);
    void segmentPieces(int track, int index, int *first, int *last) const;
    bool angleFromOn(int track, int index, int frame, int angle);
    bool joinSegmentPieces(int track, int from, int end);
    // What the linked sound of a picture of such a clip is: `sound` is given
    // the source's path and the frames of it that lie under the picture
    // (its start, in, out and length; shorter than the picture where the
    // source begins after it or ends before it). False when the source has
    // nothing for any of the picture.
    bool fixedSoundFor(const QVariantMap &picture, QVariantMap &sound);
public:
    // Copy/paste: place an entries() map (media clip or title) on `track`
    // at `position` — overwrite, or insert (duplicate) — and re-apply the
    // attributes the entry carried (speed, volume, pan, EQ, opacity, rect,
    // crop, colour, fades, disabled, label). Compounds refuse.
    QVariantMap entryForPaste(const QVariantMap &entry, double originSeconds, QString *error = nullptr);
    bool pasteClip(int track, int position, const QVariantMap &entry,
                   bool insert);
    // Nudge a clip by `delta` frames (Alt+←/→). Tracks >= 1 just move it;
    // on V1 a rightward nudge opens that much empty space before the clip
    // and a leftward one eats it — refused when there is none to eat.
    bool nudgeAt(int track, int index, int delta);
    // The clip on `track` starting exactly at `position`, or -1.
    int clipIndexAt(int track, int position) const;
    // Flat connected index → (track, index-on-track); false when absent.
    bool connectedAddress(int connectedIndex, int *track, int *index) const;

    // ── Edit operations at any address (A2) ─────────────────────────────
    // Track 0 delegates to the V1 ripple-playlist ops; tracks >= 1 work on
    // their blank-padded playlists. `track == trackCount()` on insertOn /
    // overwriteOn / moveTo creates that track (a drop above the top one).
    // Kinds never mix: video clips on video tracks, audio-only on audio.
    bool splitOn(int track, int position);
    // Blade every unlocked picture/sound track, keeping linked groups together.
    // Gaps, transitions, boundaries and caption tracks are not blade targets.
    // Returns the number of clips cut, or -1 when the operation failed.
    int splitAllAt(int position);
    // ripple=false lifts (a blank stays); ripple=true closes the gap on that
    // track and ripples the auto-selected others from the clip's old end.
    bool removeOn(int track, int index, bool ripple);
    // Several clips at once ({track, index}: higher tracks first, then
    // higher indices), each lifted or, with `ripple`, its place closed once
    // for the whole set. `primaryClosed` takes, for each clip that left the
    // main track, where it began and the frames the track lost by it;
    // `keptGap` is set when a picture's place was kept because its linked
    // sound stays. False when nothing was removed.
    bool removeSet(const QList<QPair<int, int>> &clips, bool ripple, QList<QPair<int, int>> *primaryClosed = nullptr,
                   bool *keptGap = nullptr);
    // Insert pushes that track from `position` and ripples the others;
    // overwrite carves exactly its span and moves nothing else.
    // speed != 1 opens through timewarp (in/out then count timewarp
    // frames, like append); videoOnly drops the audio stream (V1 only).
    bool insertOn(int track, const QString &path, int position, int in = -1,
                  int out = -1, bool audioOnly = false, double speed = 1.0,
                  bool videoOnly = false, const QVariantMap &retime = {});
    bool overwriteOn(int track, const QString &path, int position, int in = -1,
                     int out = -1, bool audioOnly = false, double speed = 1.0,
                     bool videoOnly = false, const QVariantMap &retime = {});
    bool moveTo(int track, int index, int newTrack, int newStart);
    struct ClipMove { int track; int index; int newTrack; int newStart; };
    bool moveClips(const QList<ClipMove> &asked); // two-phase: lift all, then place all
    // ripple=false: edge moves, a gap forms/closes (newStart honoured);
    // ripple=true: the clip's start holds and everything downstream — on
    // this track and the auto-selected others — rides the length delta.
    bool trimOn(int track, int index, int in, int out, int newStart, bool ripple);
    // Dissolves and fades on any track (the V1 forms above are shims).
    bool mixOn(int track, int index, int length, bool followLane = true,
               const QString &style = QString(), const QString &align = QString(),
               int extA = 0, int extB = 0);
    // Dissolve alignment: "" = overlap (the pair's visible frames shorten,
    // the old behaviour), "centre" / "start" / "end" = the cut keeps its
    // frames and the dissolve is built from the clips' HANDLES (A's out
    // extended by extA, B's in pulled back by extB — clamped to what the
    // sources have, the remainder overlapping as before). The alignment and
    // the extensions ride A's cut (omavid:mixAlign/mixExtA/mixExtB) so a
    // later change can undo them exactly. Entries: mixAlign mixExtA mixExtB.
    bool setMixAlignOn(int track, int index, const QString &align);
    // A transition across the cut, 2026-11-15. Its parts, from A's entry:
    // extA frames after the cut (A's handle), extB before it (B's handle)
    // and `overlap` frames the pair shares, which is all of a plain overlap.
    using MixParts = MixShape::Parts;
    static MixParts mixParts(const QVariantMap &a) { return MixShape::parts(a); }
    // What the pair gives it: each source's handle past the cut (in all,
    // what the transition already uses included) and each clip's own frames
    // on its side of the cut.
    using MixRoom = MixShape::Room;
    static MixRoom mixRoom(const QVariantMap &a, const QVariantMap &b) { return MixShape::room(a, b); }
    // The most frames it can have before the cut (after = false) or after.
    static int mixSideLimit(const QVariantMap &a, const QVariantMap &b, bool after) { return MixShape::sideLimit(a, b, after); }
    // "" / "start" / "end" / "centre" / "custom": the name true of the parts.
    static QString mixAlignFor(int extA, int extB, int overlap);
    // Copies of clips, each taken out of the transitions it is in: the clip
    // as it is drawn, from its cut to its cut, without the frames a
    // transition had borrowed past either. `all` is the clip list the copies
    // were read from: the shape of a transition into a clip is kept by the
    // clip before it. A copy whose clip is no longer in `all` as it was
    // copied keeps the transition into it as it is.
    static QVariantList clipsApart(const QVariantList &copies, const QVariantList &all);
    // A new transition centred on the cut and made of the handles: the edit
    // keeps its length. A side without spare frames gives its share to the
    // other; only what neither has overlaps.
    bool addMixAcrossOn(int track, int index, int length, const QString &style = QString());
    // The frames before and after the cut, each bounded by its clip's
    // handle; the cut and the other side stay. 0 and 0 removes it.
    bool setMixSidesOn(int track, int index, int before, int after);
    bool unmixOn(int track, int index);
    // Dissolves an edit removed to make room for itself since the last call.
    int takeBrokenDissolves();
    // True once after an edit whose connected clips could not follow the
    // storyline (their lanes are left as they were; the caller rolls back).
    bool takeRippleFailure();
    bool setMixLengthOn(int track, int index, int length);
    bool setMixStyleOn(int track, int index, const QString &style);
    bool setFadesOn(int track, int index, int fadeIn, int fadeOut);
    // Fit to Fill: source in..out retimed so it covers exactly `frames` at
    // `position` (overwrite). Refused when the rate would leave 0.05…20×.
    bool fitToFill(int track, const QString &path, int position, int frames,
                   int in, int out, bool audioOnly, double sourceSpan = 0.);
    // Paste Attributes: the chosen groups of `source` (an entries() map)
    // REPLACE the clip's — a default in the source clears the target's.
    // Groups: color crop transform flip opacity level eq channels blend
    // lens fades speed.
    bool pasteAttributes(int track, int index, const QVariantMap &source,
                         const QStringList &groups, QString *error = nullptr);
    static QStringList attributeGroups();

    // in/out are optional frame bounds within the clip (-1 = whole clip),
    // in the producer's OWN frame space (a retimed clip counts timewarp
    // frames). speed != 1 retimes the clip,
    // pitch-preserved (warp_pitch on). videoOnly drops the
    // audio stream — the detached-video half of Detach Audio.
    bool append(const QString &path, int in = -1, int out = -1,
                double speed = 1.0, bool gap = false, bool videoOnly = false,
                const QVariantMap &retime = {});
    // Scope the physical source origin to one accepted placement. Nested
    // restores/pastes scope their own saved origins, including an absent one.
    bool withSourceOrigin(const QVariantMap &source, const std::function<bool()> &place);
    // Insert empty track space (Alt+W) at the position; everything after
    // ripples right. Gaps are playlist blanks (A3), never clips; the
    // timeline ends at its last clip so there is no gap AT the end.
    bool insertGapAt(int position, int frames);
    // Close the empty space under `position` on `track`: the clips after it
    // move left on that track and on every unlocked, auto-selected other
    // track, as far as all of them have empty room — never over a clip.
    // Returns the frames closed (*from: where the closed span began); 0
    // when nothing could move (*error says why), -1 when the edit failed.
    int closeGapOn(int track, int position, QString *error = nullptr, int *from = nullptr);
    // Delete Gaps: close every stretch that is empty on EVERY track between
    // two clips, moving all the tracks together so nothing slips out of
    // sync. A stretch with a locked track's clips after it stays. Returns
    // the stretches closed (*closedSpans: each {start, frames}, last
    // first), -1 when the edit failed.
    int closeCommonGaps(QList<QPair<int, int>> *closedSpans = nullptr);
    // Clip ▸ Disable (V): invisible and silent, but keeps its time.
    // Done with marked brightness+volume filters so project XML keeps it.
    bool setClipDisabled(int index, bool disabled);
    // Both honour optional source bounds, like append.
    bool insertAt(const QString &path, int position, int in = -1,
                  int out = -1, double speed = 1.0,
                  bool videoOnly = false, const QVariantMap &retime = {}); // pushes what follows right
    bool overwriteAt(const QString &path, int position, int in = -1,
                     int out = -1, double speed = 1.0,
                     bool videoOnly = false, const QVariantMap &retime = {}); // replaces what it covers
    bool remove(int index);
    // Lift (Shift+Del): the clip leaves, empty space of its length stays —
    // nothing downstream moves, on any track. Dissolves touching the clip
    // are cleared.
    bool lift(int index);
    bool removeRange(int start, int end); // ripple out a timeline span
    bool splitAt(int position); // absolute timeline frame; new clip starts there
    // (An edge a transition sits on trims too, the transition kept across
    // its cut as far as the clip's spare frames go: trimUnderMixOn.)
    bool trim(int index, int in, int out); // new bounds within the producer
    // Normal (non-ripple) trim on V1: the edge moves, the clip's content
    // stays put — empty space opens or closes beside it, clamped at the
    // neighbouring clip. Dissolved edges refuse.
    bool trimNormal(int index, int in, int out);
    bool move(int from, int to);
    void clear();

    // Cross dissolves between storyline neighbours, via the edit graph's mix
    // regions (luma video wipe + audio crossfade). A dissolve lives in the
    // playlist as an extra "mix" entry; entries() hides it and reports it as
    // mixOut frames on the clip before it (and mixIn on the one after).
    // followLane sync-locks the tracks above (clips downstream of the pair
    // ride the ripple); restore() passes false because snapshot track
    // positions already include the mix.
    // style: "" / "dissolve", or "wipe-right", "wipe-left", "wipe-down",
    // "iris" — a generated luma map drives the wipe.
    bool mixAt(int index, int length, bool followLane = true,
               const QString &style = QString(), const QString &align = QString(),
               int extA = 0, int extB = 0);
    bool unmixAt(int index);              // remove that dissolve
    bool setMixLength(int index, int length); // resize it (<1 removes)
    bool setMixStyle(int index, const QString &style); // restyle in place

    // Fade from/to black (video) with matching audio ramps, on a storyline
    // clip's edges. Implemented as brightness+volume filters attached to
    // the playlist CUT (cut filters survive XML; cut properties don't), in
    // parent-source frame anchors — so trims re-anchor them and a stale
    // anchor is simply inert.
    bool setFades(int index, int fadeIn, int fadeOut);

    // Per-clip volume in dB on a storyline clip; 0 resets, -60 = silence.
    bool setClipVolume(int index, double db);
    // Volume keyframes (guide p. 271): [{frame, db}] in clip-relative
    // frames — an animated volume filter anchored like a fade. An empty
    // list clears them; a static setClipVolume clears them too.
    bool setClipVolumeKf(int index, const QVariantList &kf);
    bool setConnectedVolumeKf(int index, const QVariantList &kf);
    // Lane-clip opacity (Video Animation): 0..1 composite alpha via a
    // brightness filter's alpha on the cut (probed 7.40: qtblend honours
    // it, animated included). 1.0 removes; kf = [{frame, alpha}].
    bool setConnectedOpacity(int index, double alpha);
    bool setConnectedOpacityKf(int index, const QVariantList &kf);
    // Same adjustment on a connected-lane clip.
    bool setConnectedVolume(int index, double db);

    // Stereo pan (Pan Amount): -1 full left … 0
    // centre … +1 full right, via the panner on the cut. 0
    // removes the filter.
    bool setClipPan(int index, double pan);
    bool setConnectedPan(int index, double pan);

    // Audio EQ presets (audio equalizer): "" = flat,
    // "bass-boost"/"bass-reduce" (±6 dB low shelf), "treble-boost"/
    // "treble-reduce" (±6 dB high shelf), "voice" (presence lift + slight
    // low cut), "hum" (50 Hz notch). avfilter bass/treble/equalizer on the
    // cut, marked omavid:eq.
    bool setClipEq(int index, const QString &preset);
    bool setConnectedEq(int index, const QString &preset);

    // Clip ▸ Detach Audio: the clip's sound moves to a connected
    // audio-only clip anchored at the same frames; the storyline clip keeps
    // only its picture (audio_index -1). Any clip volume moves with the
    // audio. Refused on gaps, already-detached clips, and dissolved edges.
    bool detachAudio(int index);

    // Colour-board lite (colour correction): brightness [-1..1, 0],
    // contrast [0..2, 1], saturation [0..3, 1] via one avfilter.eq on the
    // cut. All-neutral removes the filter.
    bool setClipColor(int index, double brightness, double contrast,
                      double saturation);

    // Retime a storyline clip (Slow/Fast/Normal): re-opens it through
    // timewarp at the new speed, bounds rescaled through source frames so
    // the same material plays. Track clips downstream ride the length change.
    bool setClipSpeed(int index, double speed);
    bool setClipSpeedOn(int track, int index, double speed); // any track
    double speedSourceLength(const QVariantMap &entry);
    QVariantMap speedRampState(int track, int index);
    bool setSpeedRampOn(int track, int index, const QVariantMap &definition, QString *error = nullptr, bool normal = false);

    // Connected clips: TRACKS ("lanes") above the storyline — B-roll,
    // composited over it (higher lanes win) and mixed into the audio.
    // Positions are absolute timeline frames; each lane playlist holds
    // blanks between clips. Overlapping placements AUTO-STACK into the
    // lowest free lane, growing new lanes as needed. SYNC-LOCK (the
    // conventional multi-track rule, r55): a storyline edit that changes
    // the storyline's length by delta at a downstream boundary P shifts
    // every lane clip whose start >= P by delta; clips over the edited
    // span hold their position, and no storyline edit ever deletes a lane
    // clip. Edits that keep the length (move, roll, compound, lift) leave
    // the lanes alone. `lane` (1-based, -1 = auto) exists for restore(),
    // which must rebuild the exact layout.
    // speed != 1 opens through timewarp like append (detached audio from a
    // retimed clip keeps its rate).
    bool connectAt(const QString &path, int position, int in = -1, int out = -1,
                   bool audioOnly = false, int lane = -1, double speed = 1.0,
                   bool videoOnly = false, const QVariantMap &retime = {});
    // A title: a transparent clip carrying a dynamictext filter, placed on
    // the connected lane. The text rides the producer as omavid:title, so
    // snapshots and project files rebuild it (filters don't survive our
    // path-based restore; the text property does).
    // style keys — place: "centre" (default) / "lower" (lower third);
    // size: "small"/"medium"/"large"; font: a family name
    // ("" = dynamictext default); color: "#rrggbb" ("" = white). All ride
    // the producer as omavid:title* props, so snapshots and XML keep them.
    // The style a title entry was made with, as connectTitleAt takes it.
    static QVariantMap titleStyleOf(const QVariantMap &entry);
    // A caption's look in a form two can be compared in: every drawn key,
    // what is left unset as the title reads it back. A caption made with a
    // caption track's style compares equal to that style.
    static QVariantMap captionLookOf(const QVariantMap &style);
    // Whether a caption entry keeps a look of its own rather than following
    // `trackStyle` (its caption track's): marked so (ownStyle), dragged
    // from its place, or drawn differently.
    static bool captionHasOwnLook(const QVariantMap &entry, const QVariantMap &trackStyle);
    // The look a caption track gives captions that follow it, from before
    // there were caption styles when it keeps none.
    static QVariantMap legacyCaptionStyle();
    bool connectTitleAt(const QString &text, int position, int length,
                        const QVariantMap &style = QVariantMap(),
                        int lane = -1);
    bool removeConnected(int index); // index among connected clips, not blanks
    bool moveConnected(int index, int newStart);
    // Lane trims are position-explicit: trimming the in edge moves the
    // clip's start with it (lane positions are absolute, nothing ripples).
    bool trimConnected(int index, int in, int out, int newStart);
    // Picture-in-picture: normalized frame fractions; full frame resets.
    bool setConnectedRect(int index, double x, double y, double w, double h);
    bool updateTitleAt(int index, const QString &text, // same spot, new look
                       const QVariantMap &style = QVariantMap());
    int connectedCount() const;

    // Roll edit (the precision editor's centre line, guide p. 548): move
    // the edit point between clip `index` and index+1 by delta frames —
    // A's out and B's in shift together, total length unchanged. Bounds
    // clamp to each side's media handles. A transition on the cut rolls
    // with it; one on the far cut of either clip refuses the roll.
    bool roll(int index, int delta);
    // (`applied`: the frames it moved, which are fewer than asked where a
    // transition on the cut stops it at the clips' spare frames.)
    bool rollOn(int track, int index, int delta, QString *error = nullptr, int *applied = nullptr); // any track; pair must touch
    // Slide (the slide tool): move clip `index` by delta between its two
    // neighbours — the previous clip's out and the next clip's in move
    // with it, the clip's content and the total length stay put. Both
    // seams must touch and be plain (no dissolves).
    bool slideOn(int track, int index, int delta);
    // Join (Ctrl+J): heal a blade cut — clip `index` and the next one on
    // the track must touch, come from the same media at the same speed,
    // and be contiguous in source frames. The first clip keeps its
    // attributes; the seam's fade-out / dissolve come from the second.
    bool joinOn(int track, int index);

    // Compound clips (Alt+G, guide p. 505): group the storyline
    // span [start, end] into ONE clip whose producer is a nested playlist,
    // rebuilt on every restore from a JSON entries list riding the producer
    // as omavid:compound (the omavid:path trick, scaled up). Straddling
    // clips are bladed at the range edges; the piece inside joins the
    // compound. v1 limits: no dissolves inside or touching the span, no
    // nested compounds, storyline only (lane clips stay where they are).
    bool makeCompound(int start, int end, QString *error = nullptr);
    // Break Apart Clip Items (Alt+Shift+G): replace a compound with
    // its contents, windowed by the compound's current in/out trim.
    bool breakApart(int index, QString *error = nullptr);

    // Rebuild the storyline from an entries() snapshot (path/in/out per clip).
    bool restore(const QVariantList &clips);
    // Same, but the header state comes from `flags` (a track-order undo)
    // instead of surviving the rebuild.
    bool restore(const QVariantList &clips, const std::vector<TrackFlags> &flags);
    // restore(wanted), but a part-way failure restores `fallback` instead of
    // leaving a wiped engine behind the UI's stale picture of the timeline.
    bool rebuild(const QVariantList &wanted, const QVariantList &fallback);
    bool rebuild(const QVariantList &wanted, const QVariantList &fallback,
                 const std::vector<TrackFlags> &flags);

    // Project files: the timeline's snapshot (tracks, clips, mix, media
    // settings and the library), written by ProjectSafety::writeDocument.
    void setMediaSources(const QVariantMap &sources, const QString &mode = QStringLiteral("original")) { m_mediaSources = sources; m_proxyMode = mode; }
    QVariantMap mediaSources() const { return m_mediaSources; }
    QString proxyMode() const { return m_proxyMode; }
    QString playbackPath(const QString &source) const;
    QVariantList soundEntries() const;
    // How many times any timeline has walked its clips for soundEntries() (for tests).
    static quint64 soundEntryWalks();
    void setAllowOffline(bool on) { m_allowOffline = on; }
    static void applyInputTransform(edit::Profile &profile, edit::Producer &producer, const QVariantMap &settings);
    // Clip Attributes ▸ frame rate: a source interpreted at `forceFps` plays
    // at forceFps/sourceFps of its recorded speed (25 fps read as 12.5 plays
    // at half speed, every frame twice). 1.0 when nothing is overridden.
    static double interpretationFactor(const QVariantMap &sourceSettings);
    bool saveDocument(const QString &path, const QStringList &library);
    bool loadDocument(const QString &path, QStringList *library);

    bool isEmpty() const { return clipCount() == 0 && connectedCount() == 0; }
    int clipCount() const;
    int lastClipIndex() const; // last real V1 clip by clip index (-1 if none)
    bool primaryHasBlanks() const; // V1 holds a ripple gap (a blank)
    // A V1 cut writes alpha (soft/inverted crop): only the tractor's
    // composite over black shows it, so decode and export through that.
    bool primaryNeedsComposite() const;
    int frameCount() const;
    // Storyline length alone — ripple deltas for marker re-anchoring.
    int primaryFrameCount() const;
    double fps() const;
    int width() const;
    int height() const;

    // One QVariantMap per clip: {name, path, start, frames, in, out, lane} —
    // QML-ready, and exact enough for restore() to rebuild both lanes.
    // Storyline entries come first, their list index == playlist index;
    // connected entries follow with lane=1 and their own lane "index".
    QVariantList entries() const;
    // How many times any timeline has built its clip list, for a test to
    // check that one edit builds it a fixed number of times however long the
    // timeline is (it once did so for every clip).
    static quint64 clipListBuilds();
    // How many times linked sound was conformed by putting back only what
    // changed, and by rebuilding the whole timeline (for tests).
    static quint64 narrowConforms();
    static quint64 wholeConforms();
    QVariantList clipEntries() const; // entries() before the silent-source flag

    MediaPlayer::Decoded decodeAt(int position, bool withAudio,
                                int frequency = 48000, int channels = 2,
                                bool withImage = true);
    void setExportAlpha(bool on) { m_exportAlpha = on; }
    // An export of another size than the timeline's: its pictures are made
    // `size` by `framing` ("fit" with black bars, "fill" cropped, "blur" fitted
    // over a blurred copy). Before prepareLinearOutput.
    void setExportFraming(const QSize &size, const QString &framing) { m_exportFrameSize = size; m_exportFraming = framing; }
    // For the export consumer; the tractor when the connected lane is in
    // use, the bare storyline otherwise. Null while the timeline is empty.
    // The same frame with every attribute filter switched off — the
    // "before" half of the comparison view. Filters are put back after.
    MediaPlayer::Decoded decodeBefore(int position);
    bool linearRendering() const;
    // Colour-managed pictures the engine cannot draw come from the older
    // linear renderer, which reads original media whatever the proxy choice.
    bool originalsOnly() { return linearRendering() && !engineRendering(); }
    // The engine draws this timeline's pictures (OMAEDIT_RENDERER=engine and
    // nothing in it the engine cannot draw yet).
    bool engineRendering();
    // The clips the last picture drew (hidden ones are not).
    int clipsDrawn() const;
    // Pictures read for the last frame drawn that were converted smaller than they are (drawn small while playing).
    int picturesReduced() const;
    // Of the last frame's decoded pictures: those fetched side by side before it was drawn, and those of them begun during the frame before.
    int picturesFetched() const;
    int picturesMadeSmall() const;  // of picturesReduced(): made small before they reached the engine
    int picturesFetchedAhead() const;
    QString engineUnsupported();
    // The engine draws on the CPU twin only (a CPU-only background render).
    void setEngineOnCpu(bool cpu);
    // Where the engine draws: "vulkan" or "cpu", empty before it has drawn.
    QString engineBackend() const;
    QImage renderEngineAt(int position, QString *error);
    QVariantMap linearSourceManagement(const QString &path) const;
    WorkingFrame renderLinearAt(int position, bool before, QString *error);
    QString prepareLinearOutput(const QString &outputSpace);
    // The picture an export writes for `frame` (after prepareLinearOutput).
    QImage deliveredPicture(int frame, QString *error = nullptr);
    // Frame `position` for the playback cache, made on the engine: working
    // light through the cache's storage curve, as full-range BT.709 in
    // `layout` (P010 or P416). False (and no error) when the engine does not
    // draw this timeline.
    bool cachePlanes(int position, int layout, engine::YuvFrame &out, QString *error);
    // Pictures drawn at this size rather than the timeline's (smaller only),
    // as a viewer showing them at that size needs them: playback's. Empty:
    // the timeline's own. A smaller picture carries "omaedit.fullSize".
    void setDrawSize(const QSize &size) { m_drawSize = size; }
    QSize drawSize() const { return m_drawSize; }
    // What its engine renderer may keep open to decode with (see
    // render::TimelineRenderer::setDecoderLimits): one of several timelines
    // rendering at once (the playback cache's) takes less than the viewer's.
    void setDecoderLimits(int sources, qint64 bytes, int threads, int perFile = 4);
    // Frames drawn while this is set are away from the playhead (a
    // filmstrip's tiles): their pictures come through decoders of their own,
    // and the playhead's stay where they are (render::MediaPool::setAside).
    void setAside(bool aside) { m_aside = aside; }
    // Media sources its renderer has open, in all or of one file: for tests.
    int openSources(const QString &path = {}) const;
    // The next engine-drawn pictures as a viewer shows them: the display
    // (its "gamut", "gamma" and "view", as the viewer's colour options name
    // them) applied on the engine, 8 bits, tagged "omaedit.shown" and
    // "omaedit.fullSize", with no working frame. For playback only: such a
    // picture cannot be measured, graded from or saved. Null: working frames.
    void setShownAs(const QVariant &display) { m_shownAs = display; }
    QVariant shownAs() const { return m_shownAs; }
    // With setShownAs: the scopes of each such picture's working frame are
    // counted on the engine as it is drawn, with these options (as
    // ScopeProcessing::measured gives them), and go with the picture
    // (ScopeProcessing::attach). Where the engine cannot count, the frame
    // comes as a working frame instead, to be measured as ever. Null: none.
    void setScopesAs(const QVariant &options) { m_scopesAs = options; }
    QVariant scopesAs() const { return m_scopesAs; }
    // A delivery written by the editor's own encoder sink:
    // frames [first, last] of this timeline as prepareLinearOutput set them up
    // (call it first when the spec has pictures), with the mixer's sound,
    // encoded as `spec` asks (`mezzanine`: the lossless intermediate).
    // `progress` gets each frame written; returning false from it stops there
    // with the file finished. `renderFailed` tells a picture or sound that
    // could not be made from an encoder or file that failed.
    // Pictures and sound are made under `request` (cancel it to stop a slow
    // one; a failure it reports fails the delivery).
    bool writeDelivery(const QString &path, const QVariantMap &spec, bool mezzanine, int first, int last,
                       const std::function<bool(int)> &progress, QString *error, bool *renderFailed = nullptr,
                       std::shared_ptr<RenderRequest> request = {});
    // What that delivery's encoder and container are given when its first
    // frame is `first`: the spec's settings, the rate it is delivered at, and
    // the timeline timecode it starts at (the starting timecode plus `first`).
    bool deliverySettings(const QVariantMap &spec, bool mezzanine, int first, ExportSpec::EncoderSettings *settings,
                          int *rateNum, int *rateDen, QString *error) const;
    // The same, encoded exactly as `settings` say, on the timeline's own clock
    // (Render & Replace's lossless intermediate).
    bool writeDelivery(const QString &path, const ExportSpec::EncoderSettings &settings, int first, int last,
                       const std::function<bool(int)> &progress, QString *error, bool *renderFailed = nullptr,
                       std::shared_ptr<RenderRequest> request = {});
    // Why the mixer cannot make this timeline's sound, or nothing.
    QString soundUnsupported();
    // The loudness of frames first..last as delivered (BS.1770: integrated
    // LUFS, true peak dBTP, ...; LoudnessMeter's snapshot), from one pass of
    // the mixer, and reconstructedPeak (dBTP, see sound::ReconstructedPeak).
    // `progress` gets each frame done and stops the pass on false.
    // With `gainDb` (not NaN): the sound as a levelled delivery makes it,
    // that gain and the true-peak limiter under `ceilingDb` applied first.
    bool measureSound(int first, int last, int rate, int channels, const std::function<bool(int)> &progress,
                      QVariantMap *reading, QString *error, double gainDb = std::numeric_limits<double>::quiet_NaN(),
                      double ceilingDb = -1.0);
    // The levelled sound of frames first..last (gainDb, the limiter under
    // ceilingDb) as the delivery `spec` gives it to a listener, measured as
    // measureSound does: a lossy codec's sound encoded alone into `path`
    // (removed after) and decoded again, a lossless one's as made.
    bool hearDeliveredSound(const QString &path, const QVariantMap &spec, int first, int last, double gainDb, double ceilingDb,
                            const std::function<bool(int)> &progress, QVariantMap *reading, QString *error,
                            std::shared_ptr<RenderRequest> request = {});
    // What the last delivery's levelled sound measured (empty: not levelled).
    QVariantMap deliveredSound() const { return m_deliveredSound; }
    // Bypass: an attribute stays applied but its filter is off
    // (`disable`), so one item of the Applied list can be A/B'd. Keys:
    // color crop flip opacity rect level (volume + pan) eq audioMap
    // normalize. Entries report the off ones as `bypass`; editing the
    // attribute re-creates its filter, switched on.
    bool setBypassAt(int track, int index, const QString &key, bool bypass);
    static QStringList bypassableKeys();
    edit::Profile *profile() { return m_profile.get(); }
    const edit::Profile *profile() const { return m_profile.get(); }

private:
    std::unique_ptr<LinearRenderer> m_linear;
    QSize m_drawSize;  // see setDrawSize
    bool m_aside = false;
    int m_decoderSources = 0, m_decoderThreads = 0, m_decoderPerFile = 4;  // see setDecoderLimits; 0 sources: the renderer's own
    qint64 m_decoderBytes = 0;
    QVariant m_shownAs;  // see setShownAs
    QVariant m_scopesAs; // see setScopesAs
    // The export's frames as the encoder takes them, made on the engine
    // (TimelineRenderer::deliver); unset when the engine does not draw it.
    std::function<bool(int, const engine::OutputColour &, engine::YuvFrame &, QString *)> m_deliverPlanes;
    std::unique_ptr<engine::Engine> m_engine;
    bool m_engineOnCpu = false;
    std::unique_ptr<render::TimelineRenderer> m_renderer;
    // The editor's own sound mixer: the timeline's sound.
    std::unique_ptr<sound::Mixer> m_mixer;
    mutable QVariantList m_soundEntries;  // soundEntries() as last made
    // What it was made from, and when the disk was last looked at for it.
    struct SoundEntriesFrom { QVariantList clips; QVariantMap sources; QString mode; QElapsedTimer checked; };
    mutable SoundEntriesFrom m_soundEntriesFrom;
    bool writeEncoded(const QString &path, const ExportSpec::EncoderSettings &settings, int rateNum, int rateDen, int origin,
                      int first, int last, const std::function<bool(int)> &progress, QString *error, bool *renderFailed,
                      std::shared_ptr<RenderRequest> request);
    QString m_deliverySpace;  // the output space prepareLinearOutput set up
    void configureMixer();
    bool mixedSamples(int position, int frequency, int channels, std::vector<float> *out);
    bool mixedAudio(int position, int frequency, int channels, QByteArray *out);
    bool configureEngineRenderer();
    bool lockProfileTo(const QString &firstClip);
    bool ensureBoundaryAt(int position);
    // The producer-construction half of append(), shared with the compound
    // builder; invalid producer = failure.
    edit::Producer makeClipProducer(const QString &path, int in, int out,
                                   double speed, bool gap, bool videoOnly,
                                   const QVariantMap &retime = {});
    edit::Producer wrapRetime(edit::Producer &source, const QVariantMap &retime);
    edit::Producer sourceProducer(const QString &resource, const QString &playback, double effective, int *nativeFrames = nullptr,
                                  double *nativeRate = nullptr);
    edit::Producer retimedProducer(const edit::Producer &source, int nativeFrames, double nativeRate, const QVariantMap &definition,
                                   bool explicitPictures, double sourceScale, QString *error);
    edit::Producer makeSampledProducer(const QString &path, bool audioOnly, double speed,
                                     bool videoOnly, const QVariantMap &retime, int sampling);
    // Nested playlist for one compound, built from its JSON entries list;
    // null = a sub-clip failed to open.
    std::unique_ptr<edit::Playlist> buildCompound(const QString &json);
    bool appendCompound(const QString &json, int in, int out);
    int entryIndexFor(int clipIndex) const;   // storyline clip → entry index
    void applyFades(edit::Producer *cut, int in, int out, int fadeIn, int fadeOut);
    bool hasMixes() const;
    // Multi-lane plumbing: flat connected index (lane 1 clips first, in
    // position order, then lane 2…) ↔ (lane vector index, playlist entry).
    int laneTotal() const { return qMax(0, trackCount() - 1); }
    edit::Playlist *ensureLane(int laneIdx); // 0-based; grows the vector
    bool laneSpanFree(int laneIdx, int start, int span) const;
    // Lanes stay homogeneous: audio-only connects pick among audio lanes,
    // everything else among video lanes (an empty lane takes either kind) —
    // so the UI can hang audio lanes BELOW the storyline.
    bool laneIsKind(int laneIdx, bool audioOnly) const;
    int pickLane(int start, int span, bool audioOnly) const;
    bool connectedLocate(int index, int *laneIdx, int *entryIdx) const;
    bool placeConnected(edit::Producer &producer, int position, int in, int out,
                        int laneHint = -1);
    void tidyConnected();
    // Ripple governance: re-place every lane clip at an explicit new start
    // (-1 drops it), or shift clips at/after a position by delta on every
    // OTHER track that is unlocked and auto-selected — the one primitive
    // every ripple funnels through (the auto-select rule).
    void replaceLaneStarts(const QList<int> &wanted);
    void rippleOthersFrom(int sourceTrack, int position, int delta, bool primaryRides = true) {
        rippleFrom(sourceTrack, position, delta, false, primaryRides);
    }
    // includeSource: the source track's own downstream clips ride too
    // (ripple trim); its lock/autoSelect never gate it — we're editing it.
    // primaryRides false: V1 (and the sound linked to it) holds still — a
    // lane dissolve takes or gives back frames of its own lane only, so
    // toggling one on and off never moves the storyline.
    void rippleFrom(int sourceTrack, int position, int delta, bool includeSource,
                    bool primaryRides = true);
    edit::Producer makeLaneProducer(const QString &path, bool audioOnly,
                                   double speed, bool videoOnly = false,
                                   const QVariantMap &retime = {});
    bool trimLaneEntry(int laneIdx, int entry, int in, int out, int newStart);
    // (track, clip index) → playlist entry index, skipping mixes and
    // blanks; -1 when absent or the track is locked. entryIndexFor(i) is
    // the V1 special case.
    int entryIndexOn(int track, int clipIndex) const;
    int clipIndexFromEntry(int track, int entry) const;
    std::unique_ptr<edit::ClipInfo> clipInfoAt(int track, int index) const;
    // Re-derive the per-track flag filters (disabled/muted/solo) after any
    // flag change or rebuild.
    void refreshTrackFilters();
    // Project-file track metadata: one marker filter per track playlist
    // (omavid:trackMeta) carrying kind + flags, written at save, read at
    // load — playlist FILTERS serialize, playlist properties don't.
    bool restoreClips(const QVariantList &clips); // restore() minus the flag dance
    bool restoreLaneEntry(const ClipEntry &e, int start, int clipIndex, bool last, bool *landed);
    bool dressPrimaryEntry(const ClipEntry &e, int index);
    // The main track's clip at `index` taken out and put back as `entry`
    // describes it, the same length in the same place; everything else is
    // left standing. False with nothing touched when a whole rebuild is
    // needed instead; false part way leaves the track for that rebuild.
    bool replacePrimaryEntry(int index, const QVariantMap &entry);
    // A rebuild that touches only what differs: the clips of `wanted` that
    // are not what the timeline holds are put back, the rest left standing.
    // False with nothing touched when the whole rebuild is needed (a clip
    // added or removed, a dissolve near a change, the main track's length
    // changing, anything it is not sure of); false part way leaves the
    // tracks for that rebuild to replace.
    bool rebuildNarrowly(const QVariantList &wanted);
    bool replacePrimaryRun(const QVariantList &now, const QVariantList &wanted, const QList<int> &run);
    // As a restore sets itself up for the sources `paths` name; the caller
    // holds the scopes on m_mediaSources and m_offlineLengths.
    void noteRestoreSources(const QVariantList &clips, const QSet<QString> &paths);
    // The clips at `moved` (places in both lists) taken off their tracks and
    // put back as `now` describes them, everything else left standing. False
    // with nothing touched when a whole rebuild is needed instead; false part
    // way leaves the tracks for that rebuild to replace.
    bool replaceLaneEntries(const QVariantList &was, const QVariantList &now, const QList<int> &moved);
    bool restoreAudioTrack(int track, const QVariantList &clips, QString *error = nullptr);
    bool prepareAudioTrack(int track, const QVariantList &clips, Track *prepared, QString *error);
    bool restoreAudioTracks(const QMap<int,QVariantList> &changes, QString *error);
    bool setAudioEntry(int track, int index, const QVariantMap &changes);
    // Scoped to structural edits, never project loading or profile changes.
    struct RebuildMedia;
    RebuildMedia *m_rebuildMedia = nullptr;
    bool m_rebuildProfileValid = false; // settings still match the live producers
    void resetForRestore();
    void linkDetached(int track, int index, const QString &existing);
    // The audio clip a detach just placed: by path and start, still
    // unlinked — NOT "the last connected clip", which it is only when
    // nothing on its lane starts later.
    bool placedAudioAt(const QString &path, int start, int *track, int *index) const;
    void applyLooks(int track, int index, const ClipEntry &entry);
    static int snapshotIndexOn(const QVariantList &clips, int track, int index,
                               int *boundary);
    bool shapeMixOn(int track, int index, int extA, int extB, int overlap,
                    const QString &align, const QString *style);
    // An edit that takes a clip out of a transition made of the pair's spare
    // frames, or cuts through one, takes the transition off first, as Remove
    // Transition does: the frames go back to the two sources and the edit
    // then works on the clips as they were at the cut. `outgoing` are the
    // clips of `track` whose transition into the next clip is in the way
    // (those not made of handles are left to the edit, as they always were);
    // `said` of them are counted for the status line. The caller's undo
    // snapshot predates both; an edit that then fails puts everything back.
    bool setClipSpeedPlain(int track, int index, double speed);
    bool updateTitlePlain(int index, const QString &text, const QVariantMap &style);
    bool withMixesHeld(int track, int index, const std::function<bool()> &edit);
public:
    // What became of a transition an edit kept (withMixesKept): the one into
    // the clip or out of it, the frames it had and the frames it has now.
    struct KeptMix { bool into; int was, now; };
    bool withMixesKept(int track, int index, const std::function<bool()> &edit, QList<KeptMix> *kept,
                       const std::function<void()> &settled = {});
    int putMixAcrossOn(int track, int index, int after, int before, const QString &style, const QString &align);
private:
    std::optional<bool> trimLaneClipInTransition(int track, int index, int in, int out, int newStart, bool ripple);
    std::optional<bool> trimUnderMixOn(int track, int index, int in, int out);
    bool withMixesReleased(int track, const QList<int> &outgoing, int said, const std::function<bool()> &edit);
    // The handle-made transitions of a track: the clip each leaves and
    // where that clip is drawn from, the frames it plays on [from, to), its
    // cut, and where the clip it enters ends.
    struct HandleMix { int index, start, from, to, cut, nextEnd; };
    QList<HandleMix> handleMixesOn(int track) const;
    // (`closed`: the other tracks are left where they are, and it takes the
    // frame they would have rippled from and by how many frames.)
    bool removePlain(int index, QPair<int, int> *closed = nullptr);
    bool liftPlain(int index);
    bool removeRangePlain(int start, int end);
    bool movePlain(int from, int to);
    bool insertAtPlain(const QString &path, int position, int in, int out,
                       double speed, bool videoOnly, const QVariantMap &retime);
    bool overwriteAtPlain(const QString &path, int position, int in, int out,
                          double speed, bool videoOnly, const QVariantMap &retime);
    void shiftSnapshotLane(QVariantList *clips, int track, int position,
                                  int delta) const;
    std::vector<TrackFlags> trackFlags() const;
    void applyTrackFlags(const std::vector<TrackFlags> &flags);
    // Old end (start + frames) of storyline clip `index` in an entries()
    // snapshot — the downstream boundary a ripple pivots on.
    static int storylineEndIn(const QVariantList &snapshot, int index);
    void shiftPrimaryFrom(int position, int delta); // V1's share of a ripple
    void tidyPrimary(); // merge/strip V1 blanks

    // The multi-track model (Phase A, 2026-08-31): every track is one
    // edit::Playlist. m_tracks[0] is V1 — historically "the storyline", the
    // one track whose edits ripple by playlist semantics; the rest are the
    // former "lanes" (lane N == track N), blank-padded, absolute positions.
    // Kind is fixed per track once it holds a clip (an empty track takes
    // either kind — see laneIsKind); the flags are the edit-page header
    // controls, honoured by tractor() (disabled/solo) and the ripple
    // governance (locked/autoSelect).
    edit::Playlist *primary() const {
        return m_tracks.empty() ? nullptr : m_tracks.front().playlist.get();
    }
    edit::Playlist *laneAt(int laneIdx) const { // 0-based lane == track idx+1
        return m_tracks[size_t(laneIdx) + 1].playlist.get();
    }
    edit::Playlist *playlistOn(int track) const { // any track, V1 included
        return m_tracks[size_t(track)].playlist.get();
    }
    // Whether a clip of this kind may sit on `track`: V1 takes pictures
    // only; the others keep to one kind (laneIsKind).
    bool trackTakes(int track, bool audioOnly) const {
        return track == 0 ? !audioOnly : laneIsKind(track - 1, audioOnly);
    }

    QVariantMap m_mediaSources;
    QVariantMap m_fallbackProfile;
    QString m_proxyMode = QStringLiteral("original");
    bool m_allowOffline = false;
    bool m_restoring = false;
    // Fresh media uses Floor. Snapshot restoration scopes this to the saved
    // mode (including missing/legacy 0), so loading never migrates pictures.
    int m_restoreSampling = FrameSampling::Floor;
    std::optional<double> m_sourceOrigin;
    QHash<QString, int> m_offlineLengths;
    // An edit may prepare independent picture playlists against this immutable
    // clock, then transfer them only after every picture/sound graph succeeds.
    std::shared_ptr<edit::Profile> m_profile;
    bool m_exportAlpha = false;
    QSize m_exportFrameSize;
    QString m_exportFraming;
    std::vector<Track> m_tracks;
    // The clip list as last built, and what it was built from. It is the
    // same list until the edit graph changes (edit::revision), a track is
    // given another playlist or other sample-precise cuts, or the rate
    // changes; every picture shown and every step of an edit asks for it.
    struct ClipListCache {
        bool valid = false;
        quint64 graph = 0;
        double fps = 0;
        // Per track: its playlist, and its sample-precise cuts (kept, so a
        // change in place makes the track's own list a different one).
        std::vector<std::pair<const edit::Playlist *, QVariantList>> tracks;
        QVariantList clips;    // clipEntries()
        bool hasEntries = false;
        QVariantList entries;  // entries(): clips with the silent-source flag
    };
    mutable ClipListCache m_clipList;
    bool clipListCurrent() const;
    QVariantMap oneEntryOn(int track, int index) const;
    QVariantMap m_master;
    QVariantMap m_deliveredSound;
    QVariantList m_adrCues;
    QVariantMap m_captionSettings;
    QVariantMap m_project = defaultProjectSettings();
    void applyProjectProfile(edit::Profile &profile) const;
    // Counts the starting timecode at the timeline's rate once it has one
    // (`rate` whole frames a second; 0 is the live clock's).
    void settleStartFrame(int rate = 0);
    // Level-tap storage: 6 floats per track, written by the tap filters
    // from get_audio, read by takeTrackPeaks() on the same thread.
    std::vector<float> m_trackPeaks;
    AudioEngine::Routing m_audioRouting;
    AudioEngine::MeterWindow m_meterWindow;
    void setMeterWindow(const AudioEngine::MeterWindow &window);
    void clearAudioMeters();
    friend struct TapAccess;
};
