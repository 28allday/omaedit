#pragma once
#include "backgroundtasks.h"
#include <tuple>
#include <atomic>
#include "projectarchive.h"
#include "workspacelayouts.h"
#include "effectshelf.h"
#include "hardwaresettings.h"
#include "transcriber.h"
#include "beatmapper.h"
#include "lutlibrary.h"
#include "volumes.h"
#include "audiometers.h"
#include "loudnesspass.h"
#include "commandregistry.h"
#include <QLockFile>

#include <QHash>
#include <QImage>
#include <QObject>
#include <QProcess>
#include <memory>
#include <functional>
#include <QPair>
#include <QSet>
#include <QString>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "mediacatalog.h"
#include "mediatasks.h"
#include "recorder.h"
#include "renderqueue.h"
#include "soundlibrary.h"
#include "stills.h"
#include <QElapsedTimer>

class ColorMaskService;
class PlayerWorker;
class FileChooser;
class ThumbnailProvider;

// The one object QML talks to. Everything that touches the filesystem, a
// subprocess, D-Bus or the engine lives behind here; QML only reads
// properties, calls invokables and reacts to signals. Decoding itself happens
// on a worker thread (PlayerWorker) so the GUI never waits on a codec.

class ViewerProcessor;
class ScopeProcessor;

class Backend : public QObject {
    friend class AgentSession; // CLI waits for the existing worker and uses atomic staged saves.
    Q_OBJECT
    Q_PROPERTY(QVariantList videoEffectCatalogue READ videoEffectCatalogue NOTIFY pluginsChanged)
    // The transitions a cut can take, [{key, name}], the cross dissolve (key "") first.
    Q_PROPERTY(QVariantList transitionStyles READ transitionStyles CONSTANT)
    Q_PROPERTY(CommandRegistry* commands READ commands CONSTANT)
    Q_PROPERTY(WorkspaceLayouts* workspaceLayouts READ workspaceLayouts CONSTANT)
    Q_PROPERTY(bool projectBusy READ projectBusy NOTIFY projectBusyChanged)
    Q_PROPERTY(QVariantList recentProjectEntries READ recentProjectEntries NOTIFY recentChanged)
    Q_PROPERTY(QVariantMap viewerSettings READ viewerSettings NOTIFY viewerSettingsChanged)
    Q_PROPERTY(QVariantMap playbackStats READ playbackStats NOTIFY playbackStatsChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(BackgroundTasks *backgroundTasks READ backgroundTasks CONSTANT)
    Q_PROPERTY(QVariantMap mediaHealth READ mediaHealth NOTIFY mediaHealthChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QVariantList recoveryCopies READ recoveryCopies NOTIFY safetyChanged)
    Q_PROPERTY(QVariantList projectBackups READ projectBackups NOTIFY safetyChanged)
    Q_PROPERTY(QString lastRecoverySave READ lastRecoverySave NOTIFY safetyChanged)
    Q_PROPERTY(int backupLimit READ backupLimit WRITE setBackupLimit NOTIFY preferencesChanged)
    Q_PROPERTY(ProjectArchive *projectArchive READ projectArchive CONSTANT)
    Q_PROPERTY(QVariantList library READ library NOTIFY libraryChanged)
    Q_PROPERTY(MediaCatalog *mediaCatalog READ mediaCatalog CONSTANT)
    Q_PROPERTY(MediaTasks *mediaTasks READ mediaTasks CONSTANT)
    Q_PROPERTY(QString mediaPlayback READ mediaPlayback WRITE setMediaPlayback NOTIFY mediaPlaybackChanged)
    Q_PROPERTY(QVariantMap mediaPreviews READ mediaPreviews NOTIFY mediaPreviewsChanged)
    Q_PROPERTY(bool mediaGang READ mediaGang WRITE setMediaGang NOTIFY mediaGangChanged)
    Q_PROPERTY(QVariantList missingMedia READ missingMediaEntries NOTIFY libraryChanged)
    Q_PROPERTY(QVariantList timeline READ timeline NOTIFY timelineEntriesChanged)
    Q_PROPERTY(QVariantList connected READ connected NOTIFY timelineEntriesChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY historyChanged)
    Q_PROPERTY(QString redoText READ redoText NOTIFY historyChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY timelineEntriesChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY timelineEntriesChanged)
    Q_PROPERTY(QVariantList markers READ markers NOTIFY markersChanged)
    // Decor — filmstrip URLs and waveform peaks — parallel to `timeline` /
    // `connected`, announced on its own signal so a strip landing never
    // rebuilds the clip delegates (and never kills a drag in progress).
    Q_PROPERTY(QVariantList timelineDecor READ timelineDecor NOTIFY decorChanged)
    Q_PROPERTY(quint64 decorationRevision READ decorationRevision NOTIFY decorChanged)
    Q_PROPERTY(QVariantList connectedDecor READ connectedDecor NOTIFY decorChanged)
    // Track headers: [{index, name, kind, locked, disabled, muted, solo,
    // autoSelect, clips}] — the edit page's left column.
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
    // Destination track for E/W/D (edit page): 0 = V1. Q/Shift+Q keep
    // placing on top regardless.
    Q_PROPERTY(int destTrack READ destTrack WRITE setDestTrack NOTIFY destTrackChanged)
    // What an edit from the bin or the source viewer takes of its clip:
    // "both" (picture and sound), "picture" or "sound". Kept between runs.
    Q_PROPERTY(QString editTakes READ editTakes WRITE setEditTakes NOTIFY editTakesChanged)
    // Trim mode (T): off = normal trims (edge moves, gap opens/closes);
    // on = ripple trims (downstream rides the delta).
    Q_PROPERTY(bool rippleTrim READ rippleTrim WRITE setRippleTrim NOTIFY rippleTrimChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(QStringList recentProjects READ recentProjects NOTIFY recentChanged)
    // Attribute presets (Inspector ▸ Applied ▸ Save Preset): [{name, groups, attributes}].
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    // Mounted drives and the project volume's free space: see volumes.h.
    Q_PROPERTY(Volumes *volumes READ volumes CONSTANT)
    Q_PROPERTY(bool dirty READ dirty NOTIFY projectChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY videoChanged)
    Q_PROPERTY(int duration READ duration NOTIFY videoChanged)
    Q_PROPERTY(double fps READ fps NOTIFY videoChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoChanged)
    Q_PROPERTY(double videoDisplayAspect READ videoDisplayAspect NOTIFY frameChanged)
    Q_PROPERTY(int position READ position NOTIFY positionChanged)
    // How far past the last frame the timeline's playhead may go: the
    // empty tracks the timeline shows after the edit.
    Q_PROPERTY(int pastEndFrames READ pastEndFrames WRITE setPastEndFrames NOTIFY pastEndFramesChanged)
    Q_PROPERTY(QVariantMap audioPosition READ audioPosition NOTIFY audioPositionChanged)
    Q_PROPERTY(double audioPositionFrames READ audioPositionFrames NOTIFY audioPositionChanged)
    Q_PROPERTY(bool audioPositionPending READ audioPositionPending NOTIFY audioPositionChanged)
    Q_PROPERTY(bool viewingSource READ viewingSource NOTIFY sourceChanged)
    Q_PROPERTY(int sourcePosition READ sourcePosition NOTIFY sourcePositionChanged)
    Q_PROPERTY(bool sourceLinear READ sourceLinear NOTIFY sourceFrameChanged)
    Q_PROPERTY(int sourceDuration READ sourceDuration NOTIFY sourceChanged)
    // Frames a second of the source being viewed, as its timecode counts.
    Q_PROPERTY(double sourceFps READ sourceFps NOTIFY sourceChanged)
    Q_PROPERTY(int previewLibraryIndex READ previewLibraryIndex NOTIFY sourceChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(QVariantMap monitorSettings READ monitorSettings NOTIFY monitorVolumeChanged)
    Q_PROPERTY(double monitorVolume READ monitorVolume WRITE setMonitorVolume NOTIFY monitorVolumeChanged)
    // ── Multicam ────────────────────────────────────────────────────────
    // multicamMode: the viewer shows the angle grid for the multicam clip
    // under the playhead (multicamClip: {has, track, index, angle, name,
    // angles: [{name, path}], start, frames}); angleFrames are one
    // image://thumbs URL per angle ("" = that angle has no picture here).
    // multicamAudioFollows: a switch takes the new angle's sound too; off,
    // the sound playing at the first switch stays (detached) and only the
    // picture changes from then on. It governs clips whose sound follows
    // their picture; a clip made with a sound source of its own (multicamClip
    // then has `soundFrom`, the source's name) never changes its sound.
    // Comparison view: the program monitor wipes between the frame with
    // every attribute off (before) and the edit (after).
    Q_PROPERTY(bool compareMode READ compareMode WRITE setCompareMode NOTIFY compareModeChanged)
    // Two-up trim view: the timeline frame of the selected edit point (−1
    // off); the monitor shows the frame before it and the frame at it.
    Q_PROPERTY(int twoUpEdge READ twoUpEdge WRITE setTwoUpEdge NOTIFY twoUpEdgeChanged)
    // The stabiliser's background analysis, {track, index, percent} while one
    // runs (empty otherwise); the Inspector shows it on that clip.
    Q_PROPERTY(QVariantMap stabJob READ stabJob NOTIFY stabJobChanged)
    // Sequences (timelines) in the project: [{name}] and the active index.
    Q_PROPERTY(QVariantList sequences READ sequences NOTIFY sequencesChanged)
    Q_PROPERTY(int activeSequence READ activeSequence NOTIFY sequencesChanged)
    Q_PROPERTY(QVariantList timelineItems READ timelineItems NOTIFY timelineItemsChanged)
    Q_PROPERTY(bool multicamMode READ multicamMode WRITE setMulticamMode NOTIFY multicamChanged)
    Q_PROPERTY(bool multicamAudioFollows READ multicamAudioFollows WRITE setMulticamAudioFollows NOTIFY multicamChanged)
    Q_PROPERTY(QVariantMap multicamClip READ multicamClip NOTIFY multicamChanged)
    Q_PROPERTY(QStringList angleFrames READ angleFrames NOTIFY angleFramesChanged)
    Q_PROPERTY(bool syncing READ syncing NOTIFY syncingChanged)
    Q_PROPERTY(double speed READ speed NOTIFY playingChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY exportingChanged)
    // Deliver: the render queue, the preset browser's model and what this
    // machine's ffmpeg can encode ({name: bool, nvenc/vaapi: yes|no|probing}).
    Q_PROPERTY(RenderQueue *renderQueue READ renderQueue CONSTANT)
    Q_PROPERTY(RenderPresets *renderPresets READ renderPresets CONSTANT)
    Q_PROPERTY(HardwareSettings *hardware READ hardware CONSTANT)
    Q_PROPERTY(int captionCount READ captionCount NOTIFY timelineEntriesChanged)
    // The timeline's caption settings (rules, speakers) as stored: what the
    // caption list binds to; captionRules and captionSpeakers read them whole.
    Q_PROPERTY(QVariantMap captionSettings READ captionSettings NOTIFY captionSettingsChanged)
    Q_PROPERTY(QString defaultRenderDir READ defaultRenderDir CONSTANT)
    Q_PROPERTY(int exportPercent READ exportPercent NOTIFY exportingChanged)
    Q_PROPERTY(QVariantList audioRenderTracks READ audioRenderTracks NOTIFY tracksChanged)
    Q_PROPERTY(AudioMeters *meters READ meters CONSTANT)
    // Audio page. The meters are in audiometers.h (backend.meters). master:
    // the master mix map (volume, pan, fx); adrCues: the project's ADR cue
    // list; mixEffects: the fx catalogue the chain understands.
    Q_PROPERTY(QVariantMap master READ master NOTIFY masterChanged)
    Q_PROPERTY(QVariantList adrCues READ adrCues NOTIFY adrCuesChanged)
    Q_PROPERTY(QVariantList mixEffects READ mixEffects CONSTANT)
    Q_PROPERTY(QVariantMap audioServices READ audioServices CONSTANT)
    Q_PROPERTY(SoundLibrary *soundLibrary READ soundLibrary CONSTANT)
    Q_PROPERTY(Recorder *recorder READ recorder CONSTANT)
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)
    Q_PROPERTY(LoudnessPass *loudnessPass READ loudnessPass CONSTANT)
    Q_PROPERTY(QString auditionPath READ auditionPath NOTIFY sourceChanged)
    // Color page. gradeClip: the video clip under the playhead
    // ({track, index, name, start, frames, grade, gradeVersions,
    // gradeVersion, bypass}) — what the controls edit; colorClips: every
    // video clip in time order for the clip strip; scopes render into the
    // thumbs provider as scope:waveform / parade / vectorscope / histogram
    // and scopeTick bumps when they do; trackJob: {track, index, node,
    // percent} while a window is tracked; luts: see lutlibrary.h.
    Q_PROPERTY(QVariantMap gradeClip READ gradeClip NOTIFY gradeClipChanged)
    Q_PROPERTY(bool hasCopiedKeyframes READ hasCopiedKeyframes NOTIFY keyframeClipboardChanged)
    Q_PROPERTY(bool hasCopiedGrade READ hasCopiedGrade NOTIFY colorClipboardChanged)
    Q_PROPERTY(bool hasCopiedNode READ hasCopiedNode NOTIFY colorClipboardChanged)
    Q_PROPERTY(QVariantList colorClips READ colorClips NOTIFY colorClipsChanged)
    Q_PROPERTY(int scopeTick READ scopeTick NOTIFY scopeTickChanged)
    Q_PROPERTY(bool scopesActive READ scopesActive WRITE setScopesActive NOTIFY scopesActiveChanged)
    Q_PROPERTY(QVariantMap scopeOptions READ scopeOptions WRITE setScopeOptions NOTIFY scopeOptionsChanged)
    Q_PROPERTY(QVariantMap trackJob READ trackJob NOTIFY trackJobChanged)
    Q_PROPERTY(StillsGallery *stills READ stills CONSTANT)
    Q_PROPERTY(LutLibrary *lutLibrary READ lutLibrary CONSTANT)
    Q_PROPERTY(QVariantMap lutPreview READ lutPreview NOTIFY lutPreviewChanged)
    Q_PROPERTY(QString colorSpace READ colorSpace NOTIFY projectSettingsChanged)
    // File ▸ Timeline Settings…: {custom, width, height, fpsNum, fpsDen,
    // colorspace, sampleRate, channels, startFrame, startRate} (see
    // Timeline::projectSettings). rateLocked: the frame rate cannot
    // change while the active timeline holds clips. startFrame offsets every
    // programme timecode this object formats.
    Q_PROPERTY(QVariantMap projectSettings READ projectSettings NOTIFY projectSettingsChanged)
    Q_PROPERTY(bool projectRateLocked READ projectRateLocked NOTIFY projectSettingsChanged)
    Q_PROPERTY(int startFrame READ startFrame NOTIFY projectSettingsChanged)
    // Preferences: the playback output device (QAudioDevice id, "" = system
    // default) and the autosave interval in minutes (0 = off). Both live in
    // QSettings (playback/audioOutput, general/autosaveMinutes).
    Q_PROPERTY(QString audioOutput READ audioOutput WRITE setAudioOutput NOTIFY preferencesChanged)
    Q_PROPERTY(int audioBufferFrames READ audioBufferFrames WRITE setAudioBufferFrames NOTIFY preferencesChanged)
    Q_PROPERTY(bool backgroundRenderEnabled READ backgroundRenderEnabled WRITE setBackgroundRenderEnabled NOTIFY preferencesChanged)
    Q_PROPERTY(int backgroundRenderLimit READ backgroundRenderLimit WRITE setBackgroundRenderLimit NOTIFY preferencesChanged)
    Q_PROPERTY(QVariantMap backgroundRender READ backgroundRender NOTIFY backgroundRenderChanged)
    Q_PROPERTY(QString backgroundRenderProcessing READ backgroundRenderProcessing WRITE setBackgroundRenderProcessing NOTIFY preferencesChanged)
    // Preferences ▸ Audio: device-level channel mapping (AudioEngine::channelModes minus "mono", which is the monitor toggle) and the recording input side.
    Q_PROPERTY(QString monitorMapping READ monitorMapping WRITE setMonitorMapping NOTIFY preferencesChanged)
    Q_PROPERTY(QString inputChannel READ inputChannel WRITE setInputChannel NOTIFY preferencesChanged)
    // Preferences ▸ Plug-ins: the providers OmaEdit's features depend on.
    Q_PROPERTY(QVariantList plugins READ plugins NOTIFY pluginsChanged)
    // Preferences ▸ Storage (storagepreferences.h): saved values apply at the
    // next launch; `storageActivePaths` is what this process really uses.
    Q_PROPERTY(QVariantMap storageSettings READ storageSettings NOTIFY storageSettingsChanged)
    Q_PROPERTY(QVariantMap storageDefaults READ storageDefaults CONSTANT)
    Q_PROPERTY(QVariantMap storageLimits READ storageLimits CONSTANT)
    Q_PROPERTY(QVariantMap storageDefaultPaths READ storageDefaultPaths NOTIFY projectChanged)
    Q_PROPERTY(QVariantMap storageActivePaths READ storageActivePaths NOTIFY projectChanged)
    Q_PROPERTY(bool storageRestartRequired READ storageRestartRequired NOTIFY storageSettingsChanged)
    Q_PROPERTY(int autosaveMinutes READ autosaveMinutes WRITE setAutosaveMinutes NOTIFY preferencesChanged)
    Q_PROPERTY(int defaultColorspace READ defaultColorspace WRITE setDefaultColorspace NOTIFY preferencesChanged)
    // The starting timecode a new timeline is offered, "hh:mm:ss:ff". Kept as
    // the timecode and not as a count of frames, so it is the same start at
    // whatever rate the timeline turns out to have.
    Q_PROPERTY(QString newTimelineStart READ newTimelineStart WRITE setNewTimelineStart NOTIFY preferencesChanged)
    Q_PROPERTY(int recordTrack READ recordTrack NOTIFY recordingChanged)
    Q_PROPERTY(bool recordingTake READ recordingTake NOTIFY recordingChanged)

public:
    CommandRegistry *commands() const { return m_commands; }
    WorkspaceLayouts *workspaceLayouts() const { return m_workspaceLayouts; }

    explicit Backend(QObject *parent = nullptr);
    ~Backend() override;

    QString status() const { return m_status; }
    bool busy() const { return m_busy; }
    // True while any asynchronous editor work is still running or queued:
    // project I/O, media and archive tasks, sound scans, renders and their
    // snapshots, stabilise/track/mask jobs, interchange, loudness,
    // transcription, LUT/matte previews and hardware probes. A caller that
    // must see settled state (the agent CLI) waits for this to clear. Add
    // every new background job here.
    bool hasPendingWork() const;
    // How many times the table of media sources has been built from the
    // library: work that grows with the library, so it is counted.
    int mediaSourceSyncs() const { return m_mediaSourceSyncs; }
    QVariantList library() const { return m_library; }
    MediaCatalog *mediaCatalog() const { return m_mediaCatalog; }
    MediaTasks *mediaTasks() const { return m_mediaTasks; }
    QString mediaPlayback() const { return m_mediaPlayback; }
    double monitorVolume() const { return m_monitorVolume; }
    void setMonitorVolume(double volume);
    QVariantMap monitorSettings() const { return m_monitorSettings; }
    Q_INVOKABLE void setMonitorOptions(bool muted, bool dim, bool mono);
    Q_INVOKABLE void saveSourceFrame(const QString &path);
    QVariantMap mediaPreviews() const { return m_mediaPreviews; }
    bool mediaGang() const { return m_mediaGang; }
    void setMediaGang(bool on);
    Q_INVOKABLE void previewMedia(int index, int frame, bool audio = false, const QString &purpose = QStringLiteral("skim"));
    Q_INVOKABLE void stackMedia(const QVariantList &indexes, const QString &name);
    Q_INVOKABLE void unstackMedia(const QVariantList &indexes);
    Q_INVOKABLE void autoStackMedia();
    void setMediaPlayback(const QString &mode);
    QVariantList missingMediaEntries() const;
    Q_INVOKABLE QString relinkMedia(const QVariantMap &mapping, const QVariantMap &matchOptions = {});
    Q_INVOKABLE QString attachMediaProxy(int index, const QString &path);
    Q_INVOKABLE void detachMediaProxy(const QVariantList &indexes);
    Q_INVOKABLE void setMediaInput(const QVariantList &indexes, const QString &lut, int space);
    Q_INVOKABLE QString setMediaRaw(const QVariantList &indexes, double exposure, double red, double blue);
    // Clip Attributes and Timecode (Media ▸ Clip Attributes…); "" = applied.
    Q_INVOKABLE QString setClipAttributes(const QVariantList &indexes, const QVariantMap &attributes);
    Q_INVOKABLE int applyClipAttributes(const QVariantList &indexes, const QVariantMap &attributes, bool preserveSelectedFootage);
    // The clip's start timecode in frames: the editor's override, else the
    // container's, else -1.
    static qint64 clipTimecodeFrames(const QVariantMap &entry);
    Q_INVOKABLE void preferMediaProxy(const QVariantList &indexes, bool on);
    Q_INVOKABLE void refreshMediaStatus();
    Q_INVOKABLE void revealMediaPath(const QString &path);
    Q_INVOKABLE QString exportMediaCsv(const QString &path, const QVariantList &indexes = {});
    Q_INVOKABLE QString importMediaCsv(const QString &path);
    Q_INVOKABLE void setCustomMediaFields(const QVariantList &indexes, const QVariantMap &fields);
    // An import that copies or moves onto the very drive it reads from is
    // refused (status line, no task, ""): the copies would leave with the drive.
    Q_INVOKABLE QString startMediaTask(const QString &kind, const QStringList &paths, const QVariantMap &options = {});
    // Where an import from a drive copies to unless the editor says otherwise:
    // the folder the last copying import used (when it is still there and not
    // on sourceDrive), else the saved project's own Media folder, else
    // "OmaEdit Media" in the user's videos folder.
    Q_INVOKABLE QString copyImportDestination(const QString &sourceDrive = {}) const;
    Q_INVOKABLE void moveMediaToBin(const QVariantList &indexes, const QString &binId);
    // A Photoshop document's pixel layers as separate stills (PNG files in a
    // folder beside it, imported in layer order) plus its whole picture with
    // exact transparency. Returns a sentence when it cannot start.
    Q_INVOKABLE QString splitPhotoshopLayers(int libraryIndex);
    Q_INVOKABLE bool isPhotoshopMedia(int libraryIndex) const;
    Q_INVOKABLE void duplicateMedia(int index, const QString &name = QString());
    QVariantList timeline() const { return m_timeline; }
    QVariantList connected() const { return m_connected; }
    QVariantList timelineDecor() const { return m_timelineDecor; }
    QVariantList connectedDecor() const { return m_connectedDecor; }
    QString undoText() const { return m_undoText; }
    QString redoText() const { return m_redoText; }
    bool canUndo() const { return m_canUndo; }
    bool canRedo() const { return m_canRedo; }
    QString projectName() const;
    bool dirty() const { return m_dirty; }
    QVariantList recoveryCopies() const { return m_recoveryCopies; }
    QVariantList projectBackups() const { return m_projectBackups; }
    QString lastRecoverySave() const { return m_lastRecoverySave; }
    int backupLimit() const { return m_backupLimit; }
    ProjectArchive *projectArchive() const { return m_projectArchive; }
    Q_INVOKABLE void setBackupLimit(int limit);
    Q_INVOKABLE void autosaveNow();
    Q_INVOKABLE void refreshProjectSafety();
    // The window starts clean: an empty render queue, the recovery folder
    // rid of what no session can use, settings the editor no longer has
    // forgotten. Recovery copies of unsaved work are kept and offered.
    void startClean();
    Q_INVOKABLE void restoreProjectCopy(const QString &path);
    Q_INVOKABLE void removeRecoveryCopy(const QString &path);
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void chooseArchiveFolder();
    Q_INVOKABLE void archiveProject(const QString &folder, const QString &name, bool includeUnused, bool trim, int handles);
    QVariantList markers() const;
    QVariantList tracks() const { return m_tracks; }
    // Header toggles: flag = locked/disabled/muted/solo/autoSelect.
    Q_INVOKABLE void setTrackFlag(int track, const QString &flag, bool on);
    int destTrack() const { return m_destTrack; }
    QString editTakes() const;
    void setEditTakes(const QString &takes);
    // The same for one library clip, as its edits will have it: a clip with
    // no sound, or no picture, takes what it has.
    Q_INVOKABLE QString editTakesFor(int libraryIndex) const;
    bool rippleTrim() const { return m_rippleTrim; }
    void setRippleTrim(bool on);
    void setDestTrack(int track);
    Q_INVOKABLE void addTrack(bool audio);
    Q_INVOKABLE void removeTrack(int track);
    // Header menu ▸ Move Track Up / Down: swap with the neighbour of the
    // same kind (video climbs away from V1, audio descends). The
    // destination marker follows the moved track. Undoable.
    Q_INVOKABLE void moveTrack(int track, bool up);
    Q_INVOKABLE int trackNeighbour(int track, bool up) const; // -1 at the end
    // Drag a track clip onto another track (newTrack == tracks.length
    // creates one); index is the clip's index ON its track.
    Q_INVOKABLE void moveClipTo(int track, int index, int newTrack, int newStart);
    // Batch C edit ops. Paste places an entries() map (the QML clipboard)
    // at `frame` on `track`: overwrite, or insert when `insert` (duplicate
    // lands right after its source). Nudge moves a clip by frames.
    Q_INVOKABLE void pasteClip(int track, int frame, const QVariantMap &entry,
                               bool insert = false);
    Q_INVOKABLE void nudgeClip(int track, int index, int delta);
    // Group edits over a multi-selection ({track, index} lists), one undo
    // step each — see PlayerWorker for the ordering rules.
    Q_INVOKABLE void removeClips(const QVariantList &addrs, bool ripple);
    Q_INVOKABLE void nudgeClips(const QVariantList &addrs, int delta);
    Q_INVOKABLE void moveClips(const QVariantList &moves);
    Q_INVOKABLE void setClipsDisabled(const QVariantList &addrs, bool disabled);
    Q_INVOKABLE void linkClips(const QVariantList &addrs);
    Q_INVOKABLE void unlinkClips(const QVariantList &addrs);
    Q_INVOKABLE void setLabels(const QVariantList &addrs, const QString &label);
    // Paste Attributes (Alt+V): the chosen groups of `source` (an entry
    // map — the clipboard, or a preset's attributes) onto every address.
    Q_INVOKABLE void pasteAttributes(const QVariantList &addrs, const QVariantMap &source,
                                     const QStringList &groups);
    QVariantList presets() const { return m_presets; }
    Q_INVOKABLE void savePreset(const QString &name, const QVariantMap &source,
                                const QStringList &groups);
    Q_INVOKABLE void deletePreset(const QString &name);
    // Effects shelf (Edit ▸ Effects tab): see effectshelf.h. Deleting a
    // preset drops it from the shelf.
    Q_PROPERTY(EffectShelf *effectShelf READ effectShelf CONSTANT)
    EffectShelf *effectShelf() const { return m_effectShelf; }
    Q_INVOKABLE QVariantMap preset(const QString &name) const;
    // Bypass one applied attribute (Inspector ▸ Applied ▸ eye): the value
    // stays, its filter is off. Keys as Timeline::setBypassAt.
    Q_INVOKABLE void setBypass(int track, int index, const QString &key, bool on);
    // Captions: SubRip / WebVTT in as caption titles on a "Captions"
    // track; every caption title (or any title on that track) out as SubRip.
    // Looks (Inspector ▸ Composite / Lens Correction / Stabilize).
    // Dissolve alignment on the clip at (track, index): "" overlap,
    // "centre" / "start" / "end" on the cut, built from the handles.
    Q_INVOKABLE void setDissolveAlign(int track, int index, const QString &align);
    // A grip on either edge of the transition on the clip at (track, index):
    // its frames before and after the cut. The cut and the other side stay
    // and the edit keeps its length; 0 and 0 removes it.
    Q_INVOKABLE void setTransitionSides(int track, int index, int before, int after);
    // The most frames that transition can have before the cut (after =
    // false) or after it: what the clip on that side has spare.
    Q_INVOKABLE int transitionSideLimit(int track, int index, bool after) const;
    Q_INVOKABLE QVariantMap creationContext() const;
    Q_PROPERTY(bool hasTimeline READ hasTimeline NOTIFY sequencesChanged)
    bool hasTimeline() const { return m_sequences.value(m_activeSequence).toMap().value("configured", true).toBool(); }
    // GUI hosts enable the creation prompt; headless clients can still place directly.
    Q_INVOKABLE void enableTimelineCreationPrompt() { m_timelineCreationPrompt = true; }
    Q_INVOKABLE void cancelTimelinePlacement();
    Q_INVOKABLE QVariantMap newTimelineDefaults() const;
    Q_INVOKABLE QString nextSequenceName() const;
    Q_INVOKABLE QString sequenceCreationProblem(const QVariantMap &context, const QString &name, const QVariantMap &settings = {}) const;
    Q_INVOKABLE int createSequence(const QVariantMap &context, const QString &name, const QVariantMap &settings = {});
    Q_INVOKABLE void addSequence(const QString &name = QString());
    Q_INVOKABLE void switchSequence(int index);
    Q_INVOKABLE void renameSequence(int index, const QString &name);
    Q_INVOKABLE void moveSequenceToBin(int index, const QString &binId);
    Q_INVOKABLE void duplicateSequence(int index);
    Q_INVOKABLE void closeSequence(int index);
    // Nested sequence: sequence `index` inserted on V1 at the playhead as
    // one clip (it plays the sequence's latest edit).
    Q_INVOKABLE void nestSequence(int index);
    Q_INVOKABLE void dropSequenceAt(int index, int track, int frame, bool audio = false);
    Q_INVOKABLE QString sequenceNameForId(const QString &id) const;
    // ── Transcription (Inspector ▸ Transcript) ──────────────────────────
    // whisper.cpp's CLI over the clip's audio (ffmpeg extracts 16 kHz mono
    // to the cache first). An explicit, labelled AI action: nothing runs
    // until asked, it runs on this machine, and it only ANNOTATES the
    // library entry — `transcript`: [{start, end, text}] in ms, plus
    // `transcriptLanguage` — never the media or the timeline. Command and
    // model live in QSettings (transcriptCommand / transcriptModel /
    // transcriptLanguage); any whisper.cpp-compatible CLI will do.
    Q_PROPERTY(Transcriber *transcriber READ transcriber CONSTANT)
    Transcriber *transcriber() const { return m_transcriber; }
    Q_INVOKABLE void transcribe(int libraryIndex);
    // Beat map: the beats of a recording's music, found in the background
    // (BeatMapper) and written onto every library clip of that file as
    // `beatMap`: {bpm, beats: [seconds of the file's sound], duration, …}.
    // Never the media or the timeline, and not an undo step, as a transcript
    // is not. Being in the file's own time they follow each timeline clip of
    // it through trims, slips, moves, cuts and changes of speed.
    Q_PROPERTY(int beatMapRevision READ beatMapRevision NOTIFY beatMapsChanged)
    int beatMapRevision() const { return m_beatMapRevision; }
    Q_INVOKABLE bool mapBeats(int libraryIndex);
    Q_INVOKABLE bool mapBeatsOn(int track, int index); // a timeline clip's file
    Q_INVOKABLE void clearBeatMap(int libraryIndex);
    Q_INVOKABLE void clearBeatMapOn(int track, int index);
    Q_INVOKABLE void cancelBeatMap(const QString &path);
    // {bpm, count, mapping} for a file; bpm 0 and count 0 without a map.
    Q_INVOKABLE QVariantMap beatMapOf(const QString &path) const;
    // The beats a timeline clip plays, as frames from its start.
    Q_INVOKABLE QVariantList clipBeats(int track, int index) const;
    // Every beat on the timeline, as timeline frames: what a drag snaps to.
    Q_INVOKABLE QVariantList beatFrames() const;
    // Every beat on the timeline with what it is, for the beat lines drawn
    // over the tracks: {frame (of the timeline), level, bar, track, index}.
    // `level` says how far out the line is still drawn: 0 a beat, 1 the first
    // beat of a bar, 2 of every fourth bar, 3 of every sixteenth, and so on
    // by fours. `bar` is whether it is drawn as a bar: a map made before the
    // bar was found has none, and its lines are all beats (thinned in fours
    // from its first). `track` and `index` are the clip whose music it is.
    Q_PROPERTY(QVariantList beatLines READ beatLines NOTIFY beatLinesChanged)
    Q_INVOKABLE QVariantList beatLines() const;
    BeatMapper *beatMapper() const { return m_beatMapper; }
    Q_INVOKABLE void clearTranscript(int libraryIndex);
    // Stacked ("pancake") timeline: another sequence shown above the main
    // one to pull clips from; -1 = none. Its entries are that sequence's
    // snapshot (taken when it was last active). Switching TO the stacked
    // sequence, or closing it, unstacks it.
    Q_PROPERTY(int stackedSequence READ stackedSequence WRITE setStackedSequence NOTIFY stackedSequenceChanged)
    Q_PROPERTY(QVariantList stackedEntries READ stackedEntries NOTIFY stackedSequenceChanged)
    int stackedSequence() const { return m_stackedSequence; }
    void setStackedSequence(int index);
    QVariantList stackedEntries() const { return sequenceEntries(m_stackedSequence); }
    Q_INVOKABLE QVariantList sequenceEntries(int index) const;
    // A clip pulled out of the stacked sequence: the same material with
    // its attributes, overwriting `track` at `position` (undoable).
    Q_INVOKABLE void placeEntryAt(const QVariantMap &entry, int track, int position);
    Q_INVOKABLE int sequenceIndexForId(const QString &id) const;
    // Render & Replace / Restore Original (clip menu, Inspector ▸ Applied).
    Q_INVOKABLE void renderAndReplace(int track, int index);
    Q_INVOKABLE void restoreOriginal(int track, int index);
    QVariantMap renderJob() const { return m_renderJob; }
    QVariantList sequences() const;
    QVariantList timelineItems() const;
    int activeSequence() const { return m_activeSequence; }
    Q_INVOKABLE void setBlend(int track, int index, int mode);
    // How a clip takes a frame of another size or shape: "fit", "fill",
    // "stretch", "original", or "" to follow the timeline's choice.
    Q_INVOKABLE void setClipScaling(int track, int index, const QString &scaling);
    Q_INVOKABLE void setLens(int track, int index, double k1, double k2, double cx, double cy);
    QVariantList videoEffectCatalogue() const;
    QVariantList transitionStyles() const;
    Q_INVOKABLE bool editVideoEffect(int track, int index, const QString &action,
                                     const QString &uid, const QVariantMap &values);
    Q_INVOKABLE void stabilize(int track, int index, int shakiness, int smoothing);
    Q_INVOKABLE void removeStab(int track, int index);
    QVariantMap stabJob() const { return m_stabJob; }
    Q_PROPERTY(QVariantMap renderJob READ renderJob NOTIFY renderJobChanged)
    // Edit ▸ Captions. Add: a caption at the playhead. Split: one caption per
    // line of a caption. Join: neighbouring captions on one track become one.
    // Generate: captions from the timeline's speech, using each clip's
    // transcript; clips without one are transcribed first, one at a time,
    // and the captions follow when the last is done. Indices are flat
    // connected indices. False (and a status line) when nothing can happen.
    // Clip ▸ Detect Edits: finds the cuts inside a clip's shown range (the
    // ffmpeg scene filter) in the background, then blades the clip at each
    // (one undo step) or, with blade false, marks each on the clip.
    // editsDetected reports how many; -1 when it could not look.
    Q_INVOKABLE bool detectEdits(int track, int index, bool blade);
    Q_INVOKABLE void cancelDetectEdits();
    Q_PROPERTY(bool detectingEdits READ detectingEdits NOTIFY detectingEditsChanged)
    bool detectingEdits() const { return static_cast<bool>(m_detectProc); }
    Q_INVOKABLE bool addCaption();
    Q_INVOKABLE bool splitCaption(int connectedIndex);
    Q_INVOKABLE bool joinCaptions(const QVariantList &connectedIndices);
    Q_INVOKABLE bool generateCaptions();
    // Edit ▸ Captions ▸ Caption Style: one look for the timeline's captions,
    // kept on its caption tracks and used by Add, Import and Generate.
    // Keys: font, size (small|medium|large) or pixelSize, color (#rrggbb),
    // weight ("" | bold), italic, outline, bg ("" or #aarrggbb), align
    // (left|centre|right), place (top|centre|bottom), margin (percent of the
    // height, 0-45), width (percent of the width, 20-100), lineSpacing,
    // tracking; and for Generate Captions maxChars (16-80), maxLines (1-3),
    // minSeconds (0.3-10). captionStyleSettings gives every key.
    Q_INVOKABLE QVariantMap captionStyleSettings() const;
    // Sets the keys given (the rest stay). The captions that still have the
    // old look take the new one; with everyCaption all of them do. One undo
    // step. False, with the reason in status, for an unknown key or value.
    Q_INVOKABLE bool setCaptionStyle(const QVariantMap &style, bool everyCaption);
    // Each caption track keeps a style of its own. The fuller look beyond the
    // keys above: shadow (true/false), shadowColor (#rrggbb or #aarrggbb),
    // shadowOffset (-50 to 50 pixels) and shadowSoftness (0-50 pixels);
    // outlineColor (#rrggbb) and outlineWidth (0-20 pixels, 2 = the usual
    // edge); boxPadding (-1 = automatic, a quarter of the text height, or
    // 0-200 pixels), boxRadius (0-200 pixels) and boxOpacity (0-100, the
    // alpha of bg). Pixels are the timeline frame's. trackCaptionStyle gives
    // every key of one caption track's style (-1: the first caption track's,
    // as captionStyleSettings); setTrackCaptionStyle sets the keys given on
    // that track (-1: every caption track, each keeping the rest of its own
    // style), as setCaptionStyle does. One undo step.
    Q_INVOKABLE QVariantMap trackCaptionStyle(int track) const;
    Q_INVOKABLE bool setTrackCaptionStyle(int track, const QVariantMap &style, bool everyCaption);
    // One caption (a flat connected index). captionLook: every style key as
    // that caption is drawn, with x and y (its offset from where the style
    // places it, in parts of the frame, as a title's position), speaker,
    // track, and ownStyle (true when it keeps a look of its own rather than
    // following its track's style: marked so, dragged, or drawn otherwise).
    // setCaptionLook sets the keys given (style keys, x, y); the caption
    // then keeps its own look. useTrackCaptionStyle(true) puts it back in its
    // track's style and place; false keeps its look as it is, no longer
    // changed with the track's. Each one undo step.
    Q_INVOKABLE QVariantMap captionLook(int connectedIndex) const;
    // captionLook's ownStyle alone (C++: a query, not a command line operation,
    // where false would read as a refusal).
    bool captionOwnStyle(int connectedIndex) const;
    Q_INVOKABLE bool setCaptionLook(int connectedIndex, const QVariantMap &look);
    Q_INVOKABLE bool useTrackCaptionStyle(int connectedIndex, bool on);
    // The speaker of these captions ("" = none; at most 80 letters). One
    // undo step.
    Q_INVOKABLE bool setCaptionSpeaker(const QVariantList &connectedIndices, const QString &speaker);
    // A caption track's language: {code (BCP 47, such as en, fr, pt-BR;
    // "und" when not set), name, shown ("Not set" when not set)}. It is kept
    // as the track's mix "language" {code, name}. The name may be left empty
    // for a code captionLanguages knows. Each one undo step.
    Q_INVOKABLE QVariantMap captionTrackLanguage(int track) const;
    Q_INVOKABLE bool setCaptionTrackLanguage(int track, const QString &code, const QString &name);
    Q_INVOKABLE QVariantList captionLanguages() const;
    // A new caption track with a copy of every caption on `track` and its
    // style, in the language given, named "Captions (<language>)" and not
    // burnt in. One undo step.
    Q_INVOKABLE bool duplicateCaptionTrack(int track, const QString &code, const QString &name);
    // ── The caption list (Edit page) ──
    // Its rows: every caption of the timeline in time order, as
    // [{index (flat connected), track, start, end (the frame after its
    // last), frames, seconds, language {code, shown}, speaker, colour, text,
    // problems [{kind, reason}]}]. `filter` (every key optional): track (a
    // caption track; -1 every one), language (a code), speaker (a name; ""
    // the captions with none), search (words in the text, any case),
    // problems (true: only those with a problem).
    Q_INVOKABLE QVariantList captionList(const QVariantMap &filter) const;
    // The caption checks' thresholds, kept with the timeline (Caption Style
    // ▸ Rules): charsPerSecond (17), lineLetters (42), lines (2),
    // shortestSeconds (0.83), longestSeconds (7), gapFrames (2: a gap under
    // this and not none is a problem), noOverlap (true); and twoSpeakers
    // (false: Generate Captions labels the speakers of a stereo clip's
    // left and right channels, leftSpeaker and rightSpeaker, with whisper's
    // --diarize; it needs a microphone per person, each on its own
    // channel). setCaptionRules sets the keys given. One undo step.
    Q_INVOKABLE QVariantMap captionRules() const;
    Q_INVOKABLE bool setCaptionRules(const QVariantMap &rules);
    // The timeline's speakers, [{name, colour, captions}]: those named in
    // the list (a colour each) and any a caption has, in a colour its name
    // gives. setCaptionSpeakerColour adds a speaker or sets its colour
    // (#rrggbb; "" the next free one); renameCaptionSpeaker renames one in
    // the list and on every caption ("" takes it off both). One undo step each.
    Q_INVOKABLE QVariantList captionSpeakers() const;
    Q_INVOKABLE bool setCaptionSpeakerColour(const QString &speaker, const QString &colour);
    Q_INVOKABLE bool renameCaptionSpeaker(const QString &speaker, const QString &newName);
    // The colours a speaker can be given (#rrggbb).
    Q_INVOKABLE QStringList captionSpeakerColours() const;
    // A caption's words, typed in the list (lines by \n). One undo step.
    Q_INVOKABLE bool setCaptionText(int connectedIndex, const QString &text);
    // Every `find` in these captions' words (none listed: every caption)
    // replaced; matchCase false finds it in any case. One undo step; the
    // number of captions changed.
    Q_INVOKABLE int replaceCaptionText(const QString &find, const QString &replacement,
                                       const QVariantList &connectedIndices, bool matchCase);
    // Timing (none listed: every caption; each one undo step; the number of
    // captions that will move, or -1 when nothing can be done):
    // snapCaptionsToCuts moves a caption's start or end onto a cut of the
    // picture at most `frames` away; extendCaptionsToNext makes each last
    // until the next caption on its track, less the rules' gapFrames;
    // closeCaptionGaps makes a caption followed by a gap under `frames`
    // last until the next; shiftCaptions moves them by `frames` (- earlier).
    Q_INVOKABLE int snapCaptionsToCuts(const QVariantList &connectedIndices, int frames);
    Q_INVOKABLE int extendCaptionsToNext(const QVariantList &connectedIndices);
    Q_INVOKABLE int closeCaptionGaps(const QVariantList &connectedIndices, int frames);
    Q_INVOKABLE int shiftCaptions(const QVariantList &connectedIndices, int frames);
    // Generate Captions again, in place of every caption on the caption
    // tracks (words typed into them are lost; one undo step brings them back).
    Q_INVOKABLE bool regenerateCaptions();
    // The cues Generate Captions makes of words [{text, start, end}] (ms):
    // lines of at most maxChars, at most maxLines a caption, a caption ending
    // at a sentence's end or a pause, shown at least minSeconds where the
    // next one leaves room. Each cue {start, end (ms), text (lines by \n)}.
    static QVariantList captionCuesFromWords(const QVariantList &words, const QVariantMap &lengths);
    // A transcript segment's words: its own (from the transcription), or
    // its text shared out over its time by length when it has none.
    static QVariantList transcriptWords(const QVariantMap &segment);
    Q_INVOKABLE void importCaptionsDialog();
    Q_INVOKABLE void importCaptions(const QUrl &url);
    Q_INVOKABLE void exportCaptionsDialog();
    // Captions to a file, its format by its name (.srt, .vtt, .itt, .scc):
    // exportCaptionFiles(url, format, -1).
    Q_INVOKABLE bool exportCaptions(const QUrl &url);
    // Export Captions…: the formats [{format, name, suffix}] and the caption
    // tracks that hold captions [{track, name, language: {code, name, shown}, count}].
    Q_INVOKABLE QVariantList captionFormats() const;
    Q_INVOKABLE QVariantList captionExportTracks() const;
    // The Export Captions window's choice, then where to save it.
    Q_INVOKABLE void chooseCaptionExportFile(const QString &format, int track);
    // Captions as files in `format` (srt, vtt, itt, scc). `track` -1: every
    // caption track, a file each beside `url` named by its language
    // ("film.fr.srt"; just `url` when the one caption track has no language);
    // a caption track's index: that track's captions, to `url`. The files
    // written; none, with the reason in the status, when they cannot be.
    Q_INVOKABLE QStringList exportCaptionFiles(const QUrl &url, const QString &format, int track);
    // Each caption track's captions, in track order: [{track, code, name,
    // place, align, cues: [{start, frames, text}]}] (code "" when not set).
    Q_INVOKABLE QVariantList captionTrackCues() const;
    // Timeline interchange (File ▸ Import Timeline… / Export As ▸ …): OTIO,
    // CMX 3600 EDL, XML timeline (.fcpxml), AAF. Import adds a sequence to this
    // project and brings its media into the library; export writes the
    // active sequence. Both return "" or the problem.
    Q_INVOKABLE QStringList timelineFormats() const;
    Q_INVOKABLE QString timelineFormatName(const QString &format) const;
    Q_INVOKABLE bool timelineFormatAvailable(const QString &format) const;
    Q_INVOKABLE void importTimelineDialog();
    Q_INVOKABLE QString importTimeline(const QUrl &url);
    Q_INVOKABLE void exportTimelineDialog(const QString &format);
    Q_INVOKABLE QString exportTimeline(const QUrl &url, const QString &format);
    QVariantList captionCues() const; // [{start, frames, text}] in time order
    // Clip markers on a placed clip (M with a clip selected):
    // `sourceFrame` is a frame of the clip's source, so the marker rides
    // trims and moves. One marker per source frame.
    Q_INVOKABLE void addClipMarkerOn(int track, int index, int sourceFrame, const QString &type,
                                     const QString &text, const QString &notes);
    Q_INVOKABLE void removeClipMarkerOn(int track, int index, int sourceFrame);
    Q_INVOKABLE void setClipMarkerOn(int track, int index, int sourceFrame, const QString &type,
                                     const QString &text, const QString &notes);
    QVariantMap entryOn(int track, int index) const; // {} when there is none
    bool compareMode() const { return m_compareMode; }
    void setCompareMode(bool on);
    QImage beforeFrame() const { return m_beforeFrame; }
    int twoUpEdge() const { return m_twoUpEdge; }
    void setTwoUpEdge(int edge);
    QImage twoUpOutgoing() const { return m_twoUpOutgoing; }
    QImage twoUpIncoming() const { return m_twoUpIncoming; }
    Q_INVOKABLE void pasteClips(const QVariantList &entries, int frame);
    // Clips as a copy carries them: each taken out of the transitions it is
    // in, the clip drawn from its cut to its cut (Timeline::clipsApart).
    Q_INVOKABLE QVariantList clipsApart(const QVariantList &entries) const;
    Q_INVOKABLE QVariantMap clipboardClock() const;
    // A timeline frame as a paste position, on the clipboard clock.
    Q_INVOKABLE QVariantMap frameTime(int frame) const;
    Q_INVOKABLE int pasteClipboardSnapshot(const QVariantMap &clipboard,
                                           const QVariantMap &position, int destinationTrack = -1,
                                           bool insert = false);
    Q_INVOKABLE int duplicateClips(const QVariantList &entries);
    // Replace edit (Shift+R): the library clip's material — from its range
    // start — over the selected clip's exact span on its track.
    // `fromEnd`: counted back from its range end instead, so the last
    // frame of the range lands on the clip's last frame.
    Q_INVOKABLE void replaceClip(int libraryIndex, int track, int index, bool fromEnd = false,
                                 const QString &takes = {});
    // Fit to Fill: the library clip's range (or all of it) retimed to
    // cover `frames` from `position` on `track`.
    // (`replaces`: the clip of `track` these frames are, when the source is
    // fitted over a clip: the transitions on its cuts stay on the new one.)
    Q_INVOKABLE void fitToFill(int libraryIndex, int track, int position, int frames, int replaces = -1);
    // How long a clip is as it is drawn, in its own frames (Change Duration).
    Q_INVOKABLE int durationNow(bool connected, int index) const;
    // Match Frame: preview a library clip parked on `frame`, with I/O set
    // to `rangeIn`..`rangeOut` (−1 leaves the range alone).
    Q_INVOKABLE void previewClipAt(int libraryIndex, int frame, int rangeIn = -1, int rangeOut = -1);
    // Multicam: build a multicam clip in the library from ≥ 2 library clips
    // — method "audio" (waveform, async), "timecode" (the container's
    // timecode track, else creation time), "markers" (each clip's first
    // marker), "start" (all at 0). Placing one puts its active angle on the
    // timeline, tagged; cutToAngle blades at the playhead and switches the
    // rest of the segment (cut = false: the whole segment, with no cut);
    // switchAngle addresses a clip directly (context menu). Where the angle
    // has no footage the segment shows black (Timeline::angleWithoutFootage).
    // Sync Clips: line up pictures and recordings by the same methods, into a
    // new clip in the library. The originals are not touched.
    Q_INVOKABLE void createSyncedClip(const QString &name, const QVariantList &libraryIndices,
                                      const QString &method, bool silenceCameras);
    Q_INVOKABLE void setClipCategory(const QVariantList &addrs, const QString &category);
    // `soundFrom`: the library row (one of `libraryIndices`) the clip's
    // sound comes from, whatever angle its picture shows: an angle's own
    // sound, or a recording, which is then lined up with the angles but is
    // no picture angle. -1: the sound follows the picture.
    Q_INVOKABLE void createMulticam(const QString &name, const QVariantList &libraryIndices,
                                    const QString &method, int soundFrom = -1);
    Q_INVOKABLE void cutToAngle(int angle, bool cut);
    Q_INVOKABLE void switchAngle(int track, int index, int angle);
    // The sound source of a multicam clip, chosen after it is made. `source`:
    // -1 the sound follows the picture; 0… that angle's own sound; after the
    // angles, the recordings lined up with the clip that are no angle.
    // setMulticamSound changes the whole placed clip that the clip at
    // (track, index) is a piece of, across its angle cuts, as one undo step,
    // and not the library item; setLibraryMulticamSound changes the library
    // item for later placements, and no clip already placed.
    // The rows of the two "Sound From" menus: {source, text, current}, the
    // sources that have sound, "Follows the picture" first.
    Q_INVOKABLE void setMulticamSound(int track, int index, int source);
    Q_INVOKABLE void setLibraryMulticamSound(int libraryIndex, int source);
    Q_INVOKABLE QVariantList multicamSoundSources(const QString &mcam) const;
    Q_INVOKABLE QVariantList libraryMulticamSoundSources(int libraryIndex) const;
    // Collapse Multicam, on the timeline at `index` of sequences, open or
    // not: every multicam piece on it becomes an ordinary clip of the angle
    // it shows, with nothing else changed (Timeline::multicamCollapsed), as
    // one undo step. multicamPiecesIn says how many there are to collapse.
    Q_INVOKABLE int multicamPiecesIn(int index) const;
    Q_INVOKABLE void collapseMulticam(int index);
    Q_INVOKABLE void detachAudioOn(int track, int index);
    // Reattach Audio (the inverse of Detach), Flip H/V, Freeze Frame at
    // the playhead (the selected clip if it is under the playhead, else the
    // topmost clip there; two seconds). All undoable.
    Q_INVOKABLE void reattachAudio(int track, int index);
    Q_INVOKABLE void setFlip(int track, int index, bool h, bool v);
    Q_INVOKABLE void setAudioMap(int track, int index, const QString &mode);
    Q_INVOKABLE void setNormalize(int track, int index, bool on);
    Q_INVOKABLE void freezeFrame();
    Q_INVOKABLE void freezeFrameAt(int track, int index, int frame);
    Q_INVOKABLE QVariantMap clipUnderPlayhead() const; // {has, track, index, ...}
    bool multicamMode() const { return m_multicamMode; }
    void setMulticamMode(bool on);
    bool multicamAudioFollows() const { return m_multicamAudioFollows; }
    void setMulticamAudioFollows(bool on);
    QVariantMap multicamClip() const { return m_multicamClip; }
    QStringList angleFrames() const { return m_angleFrames; }
    bool syncing() const { return m_syncing; }
    // Clip colour labels ("" clears) and the header rename ("" = V1/A1).
    Q_INVOKABLE void setClipLabel(int timelineIndex, const QString &label);
    Q_INVOKABLE void setConnectedLabel(int connectedIndex, const QString &label);
    Q_INVOKABLE void renameTrack(int track, const QString &name);
    // Slide tool: move a clip between its neighbours; Ctrl+J: join a clip
    // with the next one on its track when they are one piece of source.
    Q_INVOKABLE void slideClip(int track, int index, int delta);
    Q_INVOKABLE void joinClip(int track, int index);
    // A title dropped from the Titles tab: like addTitle, at `frame`.
    Q_INVOKABLE void addTitleAt(const QString &text, int frame,
                                const QVariantMap &style = QVariantMap());
    // Generators and adjustment clips (Titles tab ▸ Generators): kind is
    // solid / bars / noise / count / adjust; a lane clip 4 s long at the
    // playhead or at `frame`. A solid's colour can change in place.
    // On an empty timeline a generator starts the edit on V1 instead; an
    // adjustment clip grades what lies under it, so there it is refused.
    // False (with the reason in the status) when nothing was placed.
    Q_INVOKABLE bool addGenerator(const QString &kind, const QString &color = QString());
    Q_INVOKABLE bool addGeneratorAt(const QString &kind, int frame, const QString &color = QString());
    Q_INVOKABLE void setGeneratorColor(int track, int index, const QString &color);
    // A transition dropped from the Transitions tab: a dissolve into the
    // next clip on `track` at (track, index) — created if absent — in
    // `style` ("" = plain dissolve, or wipe-right/left/down, iris).
    Q_INVOKABLE void applyTransitionAt(int track, int index, const QString &style);
    // A one-line status from the UI side (e.g. "Copied clip").
    Q_INVOKABLE void announce(const QString &text) { setStatus(text); }
    // Why a key did nothing: shown where it is read, as a refused edit's reason is.
    Q_INVOKABLE void explain(const QString &text) { sayRefused(text); }
    QStringList recentProjects() const { return m_recent; }
    BackgroundTasks *backgroundTasks() const { return m_backgroundTasks; }
    QVariantMap mediaHealth() const { return m_mediaHealth; }

    bool hasVideo() const { return m_duration > 0; }
    int duration() const { return m_duration; }
    double fps() const { return m_fps; }
    int videoWidth() const { return m_videoWidth; }
    int videoHeight() const { return m_videoHeight; }
    int position() const { return m_position; }
    int pastEndFrames() const { return m_pastEndFrames; }
    void setPastEndFrames(int frames) { frames = qMax(0, frames); if (frames != m_pastEndFrames) { m_pastEndFrames = frames; emit pastEndFramesChanged(); } }
    bool viewingSource() const { return m_viewingSource; }
    int sourcePosition() const { return m_sourcePosition; }
    double sourceFps() const { return m_sourceFps; }
    int sourceDuration() const { return m_sourceDuration; }
    int previewLibraryIndex() const { return m_previewLibraryIndex; }
    bool playing() const { return m_speed != 0.0; }
    double speed() const { return m_speed; }
    bool exporting() const { return m_exporting; }
    int exportPercent() const { return m_exportPercent; }
    AudioMeters *meters() const { return m_audioMeters; }

    // ── Color page ──────────────────────────────────────────────────────
    QVariantMap gradeClip() const { return m_gradeClip; }
    QVariantList colorClips() const { return m_colorClips; }
    int scopeTick() const { return m_scopeTick; }
    bool scopesActive() const { return m_scopesActive; }
    void setScopesActive(bool on);
    QVariantMap scopeOptions() const { return m_scopeOptions; }
    void setScopeOptions(const QVariantMap &options);
    QVariantMap trackJob() const { return m_trackJob; }
    StillsGallery *stills() const { return m_stills; }
    LutLibrary *lutLibrary() const { return m_lutLibrary; }
    QString colorSpace() const;
    Q_INVOKABLE QVariantMap colourPipelineInfo() const;
    QVariantMap projectSettings() const { return m_projectSettings; }
    bool projectRateLocked() const { return m_projectRateLocked; }
    int startFrame() const { return m_projectSettings.value(QStringLiteral("startFrame")).toInt(); }
    Q_INVOKABLE void setProjectSettings(const QVariantMap &settings);
    // A starting timecode's frame count, counted at `rate` whole frames a
    // second (the settings' startRate; 0 is the timeline's own), as the same
    // timecode at `fps`: what a settings form shows and takes.
    Q_INVOKABLE int startFrameAtRate(int frame, int rate, double fps) const;
    Q_INVOKABLE QVariantMap defaultProjectSettings() const;
    QString audioOutput() const { return m_audioOutput; }
    void setAudioOutput(const QString &deviceId);
    int audioBufferFrames() const { return m_audioBufferFrames; }
    void setAudioBufferFrames(int frames);
    QString monitorMapping() const { return m_monitorMapping; }
    void setMonitorMapping(const QString &mode);
    QString inputChannel() const { return m_inputChannel; }
    void setInputChannel(const QString &mode);
    QVariantList plugins() const { return m_plugins; }
    Q_INVOKABLE void rescanPlugins();
    Q_INVOKABLE void setPluginEnabled(const QString &id, bool enabled);
    Q_INVOKABLE void clearPluginQuarantine(const QString &id);
    QVariantMap storageSettings() const;
    QVariantMap storageDefaults() const;
    QVariantMap storageLimits() const;
    QVariantMap storageDefaultPaths() const;
    QVariantMap storageActivePaths() const;
    bool storageRestartRequired() const;
    Q_INVOKABLE QString saveStorageSettings(const QVariantMap &draft);
    Q_INVOKABLE QVariantMap storageUsage() const; // {thumbnailDiskBytes, thumbnailMemoryBytes}
    // Kept media (Render & Replace files, stabilisation analyses) that nothing
    // the editor can check still uses: the open project with its timelines
    // and history, the recent projects' files, and the saved versions,
    // recovery copies and queued renders the application holds. Projects
    // elsewhere cannot be checked, so the window asks before removing.
    // `findUnusedKeptMedia` answers with unusedKeptMediaFound({folder, files,
    // bytes}); `removeUnusedKeptMedia` looks again, deletes, and answers with
    // unusedKeptMediaRemoved({folder, files, bytes, failed}).
    Q_INVOKABLE void findUnusedKeptMedia();
    Q_INVOKABLE void removeUnusedKeptMedia();
    Q_INVOKABLE QString defaultProxyDir() const;
    // Preferences ▸ Reset: "general", "playback", "media", "storage", "audio",
    // "color", "hardware" or "all". QML-owned bits are reset by the dialog.
    Q_INVOKABLE void resetPreferences(const QString &category);
    bool backgroundRenderEnabled() const { return m_backgroundRenderEnabled; }
    int backgroundRenderLimit() const { return m_backgroundRenderLimit; }
    QVariantMap backgroundRender() const { return m_backgroundRender; }
    QString backgroundRenderProcessing() const { return m_backgroundRenderProcessing; }
    Q_INVOKABLE void setBackgroundRenderProcessing(const QString &preference);
    void setBackgroundRenderEnabled(bool enabled);
    void setBackgroundRenderLimit(int gib);
    Q_INVOKABLE void backgroundRenderAction(const QString &action);
    int autosaveMinutes() const { return m_autosaveMinutes; }
    void setAutosaveMinutes(int minutes);
    int defaultColorspace() const { return m_defaultColorspace; }
    void setDefaultColorspace(int colorspace);
    QString newTimelineStart() const { return m_newTimelineStart; }
    void setNewTimelineStart(const QString &timecode);
    // The whole frames a second the preference's frame number is counted at
    // when it is handed to a new timeline: room for any rate's frames.
    static constexpr int kNewTimelineStartRate = 60;
    // "hh:mm:ss:ff" as written, or 00:00:00:00 for anything that is not a
    // timecode (hours to 23, minutes and seconds to 59, frames to 59).
    static QString startTimecodeText(const QString &timecode);
    Q_INVOKABLE void clearRecentProjects();
    // Grade edits are undoable edits on the clip (worker-side).
    Q_INVOKABLE void setGrade(int track, int index, const QVariantMap &grade);
    Q_INVOKABLE void setGradeVersions(int track, int index, const QVariantList &versions, int active);
    // The source format and grading space (input, inputGamma, gradeSpace) on more
    // clips: "file", the ones from this clip's file; "timeline", all of them. One undo.
    Q_INVOKABLE void applyColourSource(int track, int index, const QString &scope, const QVariantMap &fields);
    Q_INVOKABLE void editGradeVersion(int track, int index, const QString &action,
                                     int version = -1, const QString &name = QString());
    // Color has separate session clipboards for a complete grade and a node.
    // The caller supplies its live draft, including any unflushed field edit.
    bool hasCopiedGrade() const { return !m_copiedGrade.isEmpty(); }
    bool hasCopiedNode() const { return !m_copiedNode.isEmpty(); }
    Q_INVOKABLE bool copyColor(const QString &kind, const QVariantMap &grade, int node = 0);
    Q_INVOKABLE void pasteColor(int track, int index, const QString &kind, int afterNode = 0, const QString &parentPath = QString());
    Q_INVOKABLE void autoBalance(int track, int index, int clipFrame);
    // Modify ▸ Match Colour / Match Audio. Set Match Reference keeps a clip and
    // the frame of it to match (frames from its start). Match Colour adds a
    // correction taking a clip's colour balance and brightness at clipFrame
    // to the reference frame's. Match Audio moves a clip's volume so its
    // sound is as loud as the reference's (EBU R128, their shown parts, as
    // their volumes stand; keyframes move together). matchFinished reports
    // each: ok, and what happened.
    // Modify ▸ Reframe to Fit: each clip ([{track, index}]) whose shape differs
    // from the timeline's is scaled to fill the frame and moved to follow what
    // is interesting in it (position keyframes each second, smoothed), one
    // clip after another in the background, one undo step each.
    // reframeToFitFinished reports each clip; reframing while any remain.
    Q_INVOKABLE bool reframeToFit(const QVariantList &clips);
    Q_INVOKABLE void cancelReframeToFit();
    Q_PROPERTY(bool reframing READ reframing NOTIFY reframingChanged)
    bool reframing() const { return static_cast<bool>(m_reframeProc) || !m_reframeQueue.isEmpty(); }
    Q_INVOKABLE bool setMatchReference(int track, int index, int clipFrame);
    Q_INVOKABLE bool matchColor(int track, int index, int clipFrame);
    Q_INVOKABLE bool matchAudio(int track, int index);
    Q_PROPERTY(QVariantMap matchReference READ matchReference NOTIFY matchReferenceChanged)
    QVariantMap matchReference() const { return m_matchReference; }
    // The external face / object mask provider (docs-color-mask-provider.md):
    // a hook for a program of the user's own, with nothing that comes with
    // OmaEdit to run in it. Not offered in this version: the Colour page
    // shows no way to it and the command line lists none of its calls. The
    // built-in AI mask of the people in the picture is offered.
    static constexpr bool kMaskProviderOffered = false;
    Q_PROPERTY(bool maskProviderOffered READ maskProviderOffered CONSTANT)
    bool maskProviderOffered() const { return kMaskProviderOffered; }
    Q_PROPERTY(QVariantMap maskProvider READ maskProvider WRITE setMaskProvider NOTIFY maskProviderChanged)
    Q_PROPERTY(QVariantMap maskJob READ maskJob NOTIFY maskJobChanged)
    QVariantMap maskProvider() const { return m_maskProvider; }
    QVariantMap maskJob() const { return m_maskJob; }
    void setMaskProvider(const QVariantMap &settings);
    Q_INVOKABLE void generateColorMask(int track,int index,const QString &nodePath,int frame,const QString &kind,const QString &prompt);
    Q_INVOKABLE void cancelColorMask();
    Q_INVOKABLE void shareColor(int track, int index, const QString &action, const QString &name, const QVariantList &addresses);
    Q_PROPERTY(QVariantList colorGroups READ colorGroups NOTIFY sequencesChanged)
    QVariantList colorGroups() const;
    Q_INVOKABLE void requestColorMatte(int track, int index, const QString &nodeId, int frame);
    Q_INVOKABLE void clearColorMatte();
    // A clip alone with its effects, for the effects window: over a check where
    // it is clear, or its matte; at most width × height.
    Q_INVOKABLE void requestEffectPreview(int track, int index, int frame, bool matte, int width, int height);
    Q_INVOKABLE void clearEffectPreview();
    Q_INVOKABLE void chooseColorAsset(const QString &action, const QString &id = QString());
    Q_PROPERTY(QVariantList nodePresets READ nodePresets NOTIFY nodePresetsChanged)
    QVariantList nodePresets() const { return m_nodePresets; }
    Q_INVOKABLE QString saveNodePreset(const QString &name, const QVariantMap &node);
    Q_INVOKABLE void removeNodePreset(const QString &id);
    Q_INVOKABLE void applyNodePreset(const QString &id, int track, int index, int afterNode, const QString &parentPath = QString());
    QImage colorMatteFrame() const { return m_colorMatte; }
    QImage effectPreviewFrame() const { return m_effectPreview; }
    Q_INVOKABLE void trackWindow(int track, int index, int node, double x, double y, double w, double h);
    Q_INVOKABLE void trackWindowRange(int track, int index, int node, int window, int frame, int direction, bool singleFrame, const QString &nodePath = QString());
    Q_INVOKABLE void stopWindowTracking(bool keep);
    Q_INVOKABLE QVariantMap defaultGrade() const;
    // The programme frame's pixel at a fraction of the picture, as #rrggbb
    // ("" without a frame) — the qualifier's eyedropper.
    Q_INVOKABLE QString colorAt(double fx, double fy) const;
    Q_INVOKABLE QVariantMap defaultGradeNode(const QString &label) const;
    // Stills: grab the programme frame with the current clip's grade; apply a
    // still's grade to clips ([{track, index}]); the thumbs provider serves
    // still:<id>.
    Q_INVOKABLE QString grabGradeStill(const QString &name, const QString &album);
    Q_INVOKABLE void applyStill(const QString &id, const QVariantList &addrs);
    Q_INVOKABLE void chooseLutFolder();
    Q_INVOKABLE void chooseLutFile();
    Q_INVOKABLE void setNodeLut(int track, int index, int node, const QString &nodeId, const QString &path, double intensity);
    Q_INVOKABLE void requestLutPreview(int track, int index, int node, const QString &nodeId, int frame, const QString &path, double intensity);
    Q_INVOKABLE void clearLutPreview();
    QVariantMap lutPreview() const { return m_lutPreview; }
    // The video clip covering a frame (topmost track), {} when none.
    Q_INVOKABLE QVariantMap videoClipAt(int frame) const;

    // ── Audio page ──────────────────────────────────────────────────────
    QVariantMap master() const { return m_master; }
    QVariantList adrCues() const { return m_adrCues; }
    QVariantList mixEffects() const;
    QVariantMap audioServices() const;
    SoundLibrary *soundLibrary() const { return m_soundLibrary; }
    Recorder *recorder() const { return m_recorder; }
    bool loop() const { return m_loop; }
    void setLoop(bool on);
    LoudnessPass *loudnessPass() const { return m_loudnessPass; }
    QString auditionPath() const { return m_auditionPath; }
    int recordTrack() const { return m_recordTrack; }
    bool recordingTake() const { return m_recordTrack >= 0; }
    // Mix state (not undoable — mixer convention; dirties the project).
    Q_INVOKABLE void setAudioFadeCurve(int track, int index, const QString &curve);
    Q_INVOKABLE void endAudioGesture(int track, const QString &key);
    Q_INVOKABLE void beginAudioGesture(int track, const QString &key);
    Q_INVOKABLE void setTrackMix(int track, const QString &key, const QVariant &value);
    Q_INVOKABLE void setTrackMixMap(int track, const QVariantMap &mix);
    Q_INVOKABLE void setMasterMix(const QString &key, const QVariant &value);
    Q_INVOKABLE void setAdrCues(const QVariantList &cues);
    // Sound library audition in the viewer's source side (loops while
    // `loop` is on); drop places a library sound on a track (one past the
    // last = a new audio track), importing it into the project library.
    Q_INVOKABLE void auditionSound(const QString &path);
    Q_INVOKABLE void stopAudition();
    Q_INVOKABLE void dropSoundAt(const QString &path, int track, int frame);
    Q_INVOKABLE void setLoopRange(int in, int out); // -1,-1 = whole timeline
    Q_INVOKABLE void scrubTo(int frame);            // seek and hear the frame
    Q_INVOKABLE void resetLoudness();
    // Recording: the input to a WAV, transport rolling from the playhead;
    // stop places the take on `track` at the frame recording began.
    Q_INVOKABLE void startAdrRecording(int cueIndex);
    Q_INVOKABLE void promoteAdrTake(int cueIndex, int takeIndex, int in = -1, int out = -1);
    Q_INVOKABLE bool startRecording(int track, const QString &prefix = QString());
    Q_INVOKABLE QString stopRecording();
    // Where a finished take goes: the armed track when the stretch it covers
    // is free there, and a new audio track when sound already lies under it,
    // so recording never writes over what the track holds.
    Q_INVOKABLE int trackForTake(int track, int start, int frames) const;
    Q_INVOKABLE void placeTake(const QString &path, int track, int start, int frames);
    Q_INVOKABLE QString recordingsDir() const;
    QVariantList audioRenderTracks() const;
    Q_INVOKABLE QVariantList audioOutputs() const; // [{id, name, default}]
    Q_INVOKABLE void chooseSoundFolder();          // built-in picker → soundLibrary.addFolder
    // The programme frame under the playhead to ~/Pictures/OmaEdit Stills as
    // FLOAT EXR for linear frames, otherwise PNG; returns the path.
    Q_INVOKABLE QString saveStill();

    // The active view's frame (what a single mode-switching viewer shows),
    // plus each monitor's own frame for the two-up source/program layout.
    QImage currentFrame() const { return m_viewingSource ? m_sourceFrame : m_frame; }
    QImage programFrame() const { return m_frame; }
    // The stored pixels the program viewer shows a timeline picture at
    // (empty: whole): playback draws at that size.
    void setViewerDrawSize(const QSize &size);
    // The display the program viewer shows ("gamut", "gamma", "view"), so
    // playback can make its pictures for it on the engine; null when the
    // viewer needs working frames. Scopes need them too, and get them.
    void setViewerDisplay(const QVariant &display);
    // The program picture whole: the one on screen, or (when playback drew
    // it small for the viewer) drawn again at full size.
    QImage wholeProgramFrame() const;
    double videoDisplayAspect() const {
        const double pixelAspect=m_frame.text("omaedit.pixelAspect").isEmpty()?1:m_frame.text("omaedit.pixelAspect").toDouble();
        return m_videoHeight>0?double(m_videoWidth)*pixelAspect/m_videoHeight:16./9;
    }
    QImage sourceFrame() const { return m_sourceFrame; }
    // The same for the source viewer: the stored pixels it shows its clip's
    // picture at, and its display, so a played or scrubbed source picture is
    // made for it on the engine (the working frame follows once it rests).
    void setSourceViewerDrawSize(const QSize &size);
    void setSourceViewerDisplay(const QVariant &display);
    bool sourceLinear() const;

    void setThumbnailProvider(ThumbnailProvider *provider);

    // Pure helpers live here as statics so the tests can reach them without a
    // window, an engine or a desktop.
    static QString displayNameFor(const QUrl &url);
    static QString timecodeFor(int frame, double fps);
    static double nextShuttleSpeed(double current, int direction);

    Volumes *volumes() const { return m_volumes; }
    QString projectPath() const { return m_projectPath; }

    Q_INVOKABLE void importDialog();        // several files at once
    Q_INVOKABLE void importFolderDialog();  // a folder, recursed for media
    // Files and folders dropped in from a file manager; folders recurse.
    Q_INVOKABLE void importUrls(const QList<QUrl> &urls);
    // Whether a library clip's media is on the timeline (any track or lane).
    Q_INVOKABLE bool clipIsUsed(int libraryIndex) const;

    // Logging — per library clip, saved with the project, each dirties it.
    Q_INVOKABLE void setClipRating(int libraryIndex, int rating);      // 0..5
    Q_INVOKABLE void setClipFavorite(int libraryIndex, bool on);
    Q_INVOKABLE void setClipRejected(int libraryIndex, bool on);
    Q_INVOKABLE void setClipTags(int libraryIndex, const QStringList &tags);
    Q_INVOKABLE void setClipKeywords(int libraryIndex, const QStringList &keywords);
    Q_INVOKABLE void setClipNotes(int libraryIndex, const QString &notes);
    Q_INVOKABLE void renameLibraryClip(int libraryIndex, const QString &name);
    // Production fields: reel / scene / shot / take / angle / goodTake / comments.
    Q_INVOKABLE void setClipField(int libraryIndex, const QString &key, const QString &value);
    Q_INVOKABLE void setMediaFields(const QVariantList &indexes, const QVariantMap &fields);
    Q_INVOKABLE bool editMediaMarker(int index, int oldFrame, int frame, const QString &type,
                                    const QString &text, const QString &notes);
    Q_INVOKABLE void addClipMarker(int libraryIndex, int frame, const QString &type,
                                   const QString &text);
    Q_INVOKABLE void removeClipMarker(int libraryIndex, int frame);
    // A new library entry over the clip's marked range (whole clip if none).
    Q_INVOKABLE void createSubclip(int libraryIndex, const QString &name);
    Q_INVOKABLE void removeFromLibrary(int libraryIndex);
    Q_INVOKABLE void revealClip(int libraryIndex); // its folder, in the file manager

    // ── Deliver ─────────────────────────────────────────────────────────
    RenderQueue *renderQueue() const { return m_renderQueue; }
    RenderPresets *renderPresets() const { return m_renderPresets; }
    HardwareSettings *hardware() const { return m_hardware; }
    int captionCount() const { return captionCues().size(); }
    QString defaultRenderDir() const;
    // A job on the queue: `spec` in ExportSpec keys (range included); the
    // captions ride along from the timeline. Returns the job id.
    Q_INVOKABLE int enqueueRender(const QString &name, const QString &path, const QVariantMap &spec,
                                  const QString &presetName, const QStringList &warnings = {});
    // Pre-flight: [{level: error|warning|ok, text}] for these settings and path.
    Q_INVOKABLE QVariantList validateRender(const QVariantMap &spec, const QString &path) const;
    // The stems these settings would make of the open timeline beside the
    // main file at `path`: [{name, file}], the name a track's or a
    // category's, the file as it will be called (of the first part, when the
    // render is split). Empty with stems off or nothing to make one of.
    Q_INVOKABLE QVariantList renderStems(const QVariantMap &spec, const QString &path) const;
    // The sound categories with a clip in use on the open timeline, in the
    // project's order: each gets a stem when stems are made by category.
    // Last, when a sound in use has no category, a row for the stem those
    // sounds get (id ClipCategories::uncategorisedStem(), "Uncategorised").
    QVariantList audioCategoriesInUse() const;
    Q_INVOKABLE QVariantMap renderEstimate(const QVariantMap &spec, const QString &path) const; // {bytes, freeBytes, seconds, bytesText, freeText}
    // Both at once, from one look at the timeline: {validation, estimate},
    // what the Export page asks after every change.
    Q_INVOKABLE QVariantMap renderChecks(const QVariantMap &spec, const QString &path) const;
    // A name made tidy as every render's file name is (ExportSpec::tidyFileName).
    Q_INVOKABLE QString tidyRenderName(const QString &name) const;
    Q_INVOKABLE QString renderSummary(const QVariantMap &spec) const;
    Q_INVOKABLE QString renderExtension(const QVariantMap &spec) const;
    // {project} {sequence} {date} {time} {resolution} {fps} {preset} {version} expanded.
    Q_INVOKABLE QString renderFileName(const QString &pattern, const QString &presetName, const QString &dir,
                                       const QVariantMap &spec) const;
    Q_INVOKABLE void chooseRenderFolder(); // answers with renderFolderChosen
    ViewerProcessor *viewerProcessor();
    Q_INVOKABLE QString systemInformation() const;
    Q_INVOKABLE bool copySystemInformation();
    // Help ▸ About: where this installation keeps the licence texts it ships (its own, Lucide icons, Vulkan headers, bundled libraries).
    Q_INVOKABLE QString licensesFolder() const;
    Q_INVOKABLE void quitWhenRendersFinish();
    Q_INVOKABLE void openRenderOutput(int jobId);
    Q_INVOKABLE void revealRenderOutput(int jobId);

    Q_INVOKABLE void exportDialog(); // uses the last preset (default H.264)
    // preset: h264-high / h264-medium / prores / audio.
    Q_INVOKABLE void exportWithPreset(const QString &preset);
    // Export straight to a path (scripts, the demo recorder): what the
    // Save dialog's answer does.
    Q_INVOKABLE void exportTo(const QUrl &url, const QString &preset = QString());
    Q_INVOKABLE void saveProject(); // straight to the current file, else Save As
    Q_INVOKABLE void saveProjectAs(const QUrl &url);
    Q_INVOKABLE void saveProjectDialog();
    Q_INVOKABLE void openProjectDialog();
    Q_INVOKABLE void loadProject(const QUrl &url);
    Q_INVOKABLE void openRecent(const QString &path);
    bool projectBusy() const { return m_savePending || m_projectLoadPending || m_clipAttributesPending; }
    Q_INVOKABLE void closeProject();
    Q_INVOKABLE void removeRecentProject(const QString &path);
    Q_INVOKABLE void locateRecentProject(const QString &path);
    Q_INVOKABLE void refreshRecentProjects() { emit recentChanged(); }
    QVariantList recentProjectEntries() const;
    QVariantMap viewerSettings() const { return m_viewerSettings; }
    QVariantMap playbackStats() const;
    Q_INVOKABLE void setViewerOption(const QString &key, const QVariant &value);
    Q_INVOKABLE void resetPlaybackStats();
    Q_INVOKABLE void exportFrameDialog();
    Q_INVOKABLE void saveProgramFrame(const QString &path);
    // Why the shown frame cannot be saved at path, or empty when it can.
    QString frameExportProblem(const QString &path, bool source = false) const;
    quint64 presentationSerial() const { return m_presentationSerial; }
    void framePresented(quint64 serial); // accepts scene-graph acknowledgements on either thread
    Q_INVOKABLE QVariantMap newProjectDefaults() const;
    Q_INVOKABLE QString projectCreationProblem(const QVariantMap &context, const QVariantMap &settings) const;
    Q_INVOKABLE int createProject(const QVariantMap &context, const QVariantMap &settings);
    Q_INVOKABLE void newProject(); // Ctrl+N — QML guards the dirty state
    Q_INVOKABLE void open(const QUrl &url, bool appendWhenImported = false);
    Q_INVOKABLE void previewClip(int libraryIndex); // source view in the viewer
    Q_INVOKABLE void viewTimeline();                // back to the timeline
    Q_INVOKABLE void markRangeIn();  // I — on the previewed clip
    Q_INVOKABLE void markRangeOut(); // O
    Q_INVOKABLE void setRange(int in, int out); // dragging range edges
    // A library clip's I/O without previewing it (scripts, the demo).
    Q_INVOKABLE void setLibraryRange(int libraryIndex, int in, int out);
    Q_INVOKABLE void clearRange();
    // Read-only project-frame range for a browser placement preview.
    Q_INVOKABLE QVariantMap libraryPlacementRange(int libraryIndex) const;
    // `takes` on the edits below: "both", "picture" or "sound" for this edit
    // alone; left empty, the standing choice (editTakes).
    Q_INVOKABLE void appendClipAt(int libraryIndex, const QString &takes = {});
    // Drag-and-drop from the media panel: overwrite at `frame` on `track`
    // (an audio track takes the audio only); onto an empty V1 it just
    // starts the edit.
    // `audio` only matters when `track` is one past the last: the new
    // track it creates is an audio one (drop below the audio rows).
    Q_INVOKABLE void dropClipAt(int libraryIndex, int track, int frame,
                                bool audio = false, const QString &takes = {});
    Q_INVOKABLE void insertClipAt(int libraryIndex, const QString &takes = {});    // W — at the playhead
    Q_INVOKABLE void overwriteClipAt(int libraryIndex, const QString &takes = {}); // D — at the playhead
    // Backtimed: the clip's range ends at the playhead (its last frame is the
    // one before it); a range longer than the room before the playhead loses its head.
    Q_INVOKABLE void overwriteClipBacktimed(int libraryIndex, const QString &takes = {});
    Q_INVOKABLE void removeClip(int timelineIndex);
    // Shift+Del — lift: the clip leaves, a same-length gap stays.
    Q_INVOKABLE void liftClip(int timelineIndex);
    // Range delete (timeline I/O selection): ripples the span out.
    Q_INVOKABLE void deleteRange(int start, int end);
    Q_INVOKABLE void rollEdit(int timelineIndex, int delta);
    // Precision editor on any track: (track, index on that track).
    Q_INVOKABLE void rollEditOn(int track, int index, int delta);
    Q_INVOKABLE void trimClipOn(int track, int index, int in, int out);
    Q_INVOKABLE void trimToPlayhead(int track, int index, bool end);
    // Volume keyframes: kf = [{frame, db}] in clip-relative frames; an
    // empty list clears them back to a flat 0 dB line.
    Q_INVOKABLE void setClipVolumeKf(int timelineIndex, const QVariantList &kf);
    Q_INVOKABLE void setConnectedVolumeKf(int connectedIndex,
                                          const QVariantList &kf);
    // Lane-clip opacity (Video Animation, Ctrl+V): 0..1; empty kf clears.
    Q_INVOKABLE void setConnectedOpacity(int connectedIndex, double alpha);
    Q_INVOKABLE void setConnectedOpacityKf(int connectedIndex,
                                           const QVariantList &kf);
    Q_INVOKABLE void makeCompound(int start, int end);
    Q_INVOKABLE void makeCompoundSelection(const QVariantList &addresses);
    Q_INVOKABLE void breakApartClip(int timelineIndex);
    Q_INVOKABLE void insertGapAtPlayhead(); // Alt+W — ~3 s black spacer
    // Ripple the empty space under `frame` on `track` shut (Delete on a
    // selected gap); the auto-selected tracks follow as far as they can.
    Q_INVOKABLE void closeGap(int track, int frame);
    // Edit ▸ Delete Gaps: every stretch that is empty on all tracks.
    Q_INVOKABLE void deleteGaps();
    Q_INVOKABLE void toggleClipDisabled(int timelineIndex); // V
    Q_INVOKABLE void connectClipAt(int libraryIndex); // Q — above the playhead
    Q_INVOKABLE void connectAudioClipAt(int libraryIndex); // Shift+Q
    // T — title at playhead. style keys: place ("centre"/"lower"), size
    // ("small"/"medium"/"large"), font (family name), color ("#rrggbb").
    Q_INVOKABLE void addTitle(const QString &text,
                              const QVariantMap &style = QVariantMap());
    Q_INVOKABLE void updateTitle(int connectedIndex, const QString &text,
                                 const QVariantMap &style = QVariantMap());
    Q_INVOKABLE void removeConnectedClip(int connectedIndex);
    Q_INVOKABLE void moveConnectedClip(int connectedIndex, int newStart);
    Q_INVOKABLE void trimConnectedClip(int connectedIndex, int in, int out,
                                       int newStart);
    // Picture-in-picture on a connected clip: normalized frame fractions;
    // 0,0,1,1 (or w/h <= 0) resets to full frame.
    Q_INVOKABLE void setConnectedRect(int connectedIndex, double x, double y,
                                      double w, double h);
    Q_INVOKABLE void bladeAtPlayhead();
    Q_INVOKABLE void bladeAllAtPlayhead();
    Q_INVOKABLE void bladeAt(int track, int frame); // the blade tool
    Q_INVOKABLE void toggleDissolve(int timelineIndex); // Ctrl+T, ~1s
    Q_INVOKABLE void resizeDissolve(int timelineIndex, int frames); // drag its edge
    Q_INVOKABLE void setDissolveStyle(int timelineIndex, const QString &style);
    Q_INVOKABLE void toggleFade(int timelineIndex, bool outEdge); // Ctrl+[ / ]
    // The same on a track clip (flat connected index).
    Q_INVOKABLE void toggleConnectedDissolve(int connectedIndex);
    Q_INVOKABLE void resizeConnectedDissolve(int connectedIndex, int frames);
    Q_INVOKABLE void setConnectedDissolveStyle(int connectedIndex, const QString &style);
    Q_INVOKABLE void toggleConnectedFade(int connectedIndex, bool outEdge);
    // Adjust Volume: nudge by ±1 dB (Ctrl+Up/Down), or set an absolute
    // level (0 = reset, -60 = silence).
    Q_INVOKABLE void adjustVolume(int timelineIndex, double deltaDb);
    Q_INVOKABLE void setClipVolume(int timelineIndex, double db);
    // The same controls for connected-lane clips.
    Q_INVOKABLE void adjustConnectedVolume(int connectedIndex, double deltaDb);
    Q_INVOKABLE void setConnectedVolume(int connectedIndex, double db);
    // Stereo pan: -1 full left … 0 centre … +1 full right.
    Q_INVOKABLE void setClipPan(int timelineIndex, double pan);
    Q_INVOKABLE void setConnectedPan(int connectedIndex, double pan);
    // Audio EQ presets ("" = flat).
    Q_INVOKABLE void setClipEq(int timelineIndex, const QString &preset);
    Q_INVOKABLE void setConnectedEq(int connectedIndex, const QString &preset);
    // Clip ▸ Detach Audio on a storyline clip.
    Q_INVOKABLE void detachAudio(int timelineIndex);
    // Retime: 0.5/0.25 slow, 2/4 fast, 1.0 back to normal.
    Q_INVOKABLE void setClipSpeed(int timelineIndex, double speed);
    Q_INVOKABLE void setConnectedSpeed(int connectedIndex, double speed);
    // Colour-board lite: brightness/contrast/saturation; neutral resets.
    Q_INVOKABLE void setConnectedColor(int connectedIndex, double brightness,
                                       double contrast, double saturation);
    // Crop fractions off each edge; rotation in degrees (track clips only);
    // fade lengths in frames (0 removes).
    // Crop is of the clip's own picture, before its transform places it;
    // feather (0…1 of the picture) softens the crop's edge, invert keeps the
    // outside instead — both ride frei0r.alphaspot in place of qtcrop.
    Q_INVOKABLE void setClipCrop(int timelineIndex, double l, double r, double t, double b,
                                 double feather = 0.0, bool invert = false);
    Q_INVOKABLE void setConnectedCrop(int connectedIndex, double l, double r, double t, double b,
                                      double feather = 0.0, bool invert = false);
    Q_INVOKABLE void setConnectedRotation(int connectedIndex, double degrees);
    // The point the rotation turns about, as a fraction of the clip's rect.
    Q_INVOKABLE void setConnectedAnchor(int connectedIndex, double anchorX, double anchorY);
    // Retime frame blending: 0 nearest, 1 frame blend, 2 optical flow —
    // Constant speeds bake on export; speed maps support live frame blending.
    Q_INVOKABLE void setEditProperties(int track, int index, const QVariantMap &patch);
    // Keyframes > Copy/Cut/Paste: every animated picture property and the
    // opacity of one clip. Paste lands the first key at `clipFrame` (frames
    // from the clip's start) on any clip; keys outside the pasted stretch
    // stay, keys past the clip's end are left off. Each is one undo step.
    Q_INVOKABLE bool copyKeyframes(int track, int index);
    // A tracked window's path ([{frame, x, y}] in the clip's frames, as the
    // Colour page's tracker leaves it) onto the keyframe clipboard as motion:
    // pasted, another clip's position follows it (from where that clip
    // stands), at the same moments of the timeline whatever the playhead.
    Q_INVOKABLE bool copyTrackAsMotion(int track, int index, const QVariantList &points);
    Q_INVOKABLE bool cutKeyframes(int track, int index);
    Q_INVOKABLE bool pasteKeyframes(int track, int index, int clipFrame);
    bool hasCopiedKeyframes() const { return !m_keyframeClipboard.isEmpty(); }
    Q_INVOKABLE bool applyMotionPreset(int track, int index, const QString &preset, const QVariantMap &options);
    Q_INVOKABLE void beginEditGesture();
    Q_INVOKABLE void endEditGesture();
    Q_INVOKABLE void setFrameBlend(int track, int index, int mode);
    Q_INVOKABLE void setFadeLengths(int timelineIndex, int fadeIn, int fadeOut);
    Q_INVOKABLE void setConnectedFadeLengths(int connectedIndex, int fadeIn, int fadeOut);
    Q_INVOKABLE void setClipColor(int timelineIndex, double brightness,
                                  double contrast, double saturation);
    // Captures the marker and its source context; stale dialogs cannot edit a new target.
    Q_INVOKABLE QVariantMap retimeTarget(int track, int index) const;
    Q_INVOKABLE QVariantMap audioEditTarget(int track, int index) const;
    Q_INVOKABLE QString audioEditProblem(const QVariantMap &target) const;
    Q_INVOKABLE QVariantMap audioTimingFields(const QVariantMap &timing, const QString &unit, int sampleRate) const;
    Q_INVOKABLE QVariantMap previewAudioTiming(const QVariantMap &target, const QVariantMap &fields,
                                             const QString &unit, int sampleRate) const;
    Q_INVOKABLE int setAudioTiming(const QVariantMap &target, const QVariantMap &timing);
    Q_INVOKABLE int splitAudio(const QVariantMap &target, const QVariantMap &position);
    Q_INVOKABLE QVariantMap previewAudioGesture(const QVariantMap &target, const QString &action,
                                               const QVariantMap &changes, int sampleRate) const;
    Q_INVOKABLE int applyAudioGesture(const QVariantMap &target, const QString &action,
                                     const QVariantMap &changes, int sampleRate);
    Q_INVOKABLE QVariantMap audioSnapPoints(const QVariantMap &exclude, double firstFrame, double lastFrame) const;
    // … with or without the beats of mapped music among them (they go with View ▸ Show Beat Map).
    Q_INVOKABLE QVariantMap audioSnapPoints(const QVariantMap &exclude, double firstFrame, double lastFrame, bool beats) const;
    Q_INVOKABLE QString retimeProblem(const QVariantMap &target, bool segments = false) const;
    Q_INVOKABLE QVariantMap previewRetime(const QVariantMap &target, double value, bool duration, bool reverse) const;
    Q_INVOKABLE int applyRetime(const QVariantMap &target, double speed);
    Q_INVOKABLE int prepareSpeedRamp(const QVariantMap &target);
    Q_INVOKABLE QVariantMap editSpeedRampDraft(const QVariantMap &definition, const QString &action, int segment, const QVariantMap &options = {}) const;
    Q_INVOKABLE QVariantMap previewSpeedRamp(const QVariantMap &definition) const;
    Q_INVOKABLE int applySpeedRamp(const QVariantMap &target, const QVariantMap &definition, bool normal = false);
    // Modify ▸ Retime one-click edits on a clip, through the speed-segment
    // engine: "hold" (2 s freeze at clipFrame), "blade" (split the speed at
    // clipFrame), "replay" (the clip plays twice) or "rewind" (forward, back
    // at `rate` times speed, forward). clipFrame counts from the clip's
    // start. Returns the request; retimeFinished reports the result.
    Q_INVOKABLE int quickRetime(int track, int index, const QString &kind, int clipFrame = 0, double rate = 1.);
    Q_INVOKABLE QVariantMap speedRampSourceMatch(const QVariantMap &entry, int position) const;
    Q_INVOKABLE QVariantMap markerEditTarget(int track, int index, int frame) const;
    Q_INVOKABLE QString markerEditProblem(const QVariantMap &target) const;
    Q_INVOKABLE int applyMarkerEdit(const QVariantMap &target, int frame, const QString &type,
                                    const QString &text, const QString &notes, bool remove = false,
                                    const QString &colour = QString());
    // The colours a marker can be given (MarkerEdit::colours).
    Q_INVOKABLE QStringList markerColours() const;
    Q_INVOKABLE void toggleMarker(); // M — at the playhead
    Q_INVOKABLE void removeMarkersIn(int first, int last); // inclusive
    Q_INVOKABLE void copyText(const QString &text);
    // Shift+M — retitle/retype the marker at the playhead (Modify
    // Marker); type is standard/chapter/todo/done.
    Q_INVOKABLE void setMarkerInfo(int frame, const QString &text,
                       const QString &type, const QString &notes = QString());
    Q_INVOKABLE void trimClip(int timelineIndex, int in, int out);
    // Change Duration: a clip (storyline, or connected when `connected`)
    // becomes `frames` long by moving its end; storyline clips after it
    // follow. Media can only grow as far as its source; stills, titles and
    // generators as far as a still's length. The longest it can be is
    // durationLimit(); false (and a status line) when nothing changed.
    Q_INVOKABLE bool changeDuration(bool connected, int index, int frames);
    Q_INVOKABLE int durationLimit(bool connected, int index) const;
    // Slip tool: shift a V1 clip's in/out by `delta` frames, same length,
    // same place — nothing downstream moves.
    Q_INVOKABLE void slipClip(int timelineIndex, int delta);
    Q_INVOKABLE void moveClip(int from, int to);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void shuttle(int direction); // J = -1, L = +1
    quint64 decorationRevision() const { return m_decorationRevision; }
    Q_INVOKABLE QVariantMap timelineDecoration(bool connected, int index) const { return (connected ? m_connectedDecor : m_timelineDecor).value(index).toMap(); }
    Q_INVOKABLE void setTimelineDecorationWindow(double first, double last, int pixels);
    Q_INVOKABLE void seek(int frame);
    // Skimming: the viewer shows `frame` (the pointer over the ruler) while
    // the playhead stays where it is; endSkim shows the playhead's picture
    // again at once.
    Q_INVOKABLE void skim(int frame);
    Q_INVOKABLE void endSkim();
    QVariantMap audioPosition() const;
    double audioPositionFrames() const;
    bool audioPositionPending() const { return !m_requestedAudioPosition.isEmpty(); }
    Q_INVOKABLE QString audioPositionText(const QString &unit) const;
    Q_INVOKABLE bool seekAudioPosition(const QVariantMap &position, bool audition = false);
    Q_INVOKABLE bool seekAudioText(const QString &text, const QString &unit);
    Q_INVOKABLE bool stepAudioSamples(int samples);
    Q_INVOKABLE void step(int frames);
    Q_INVOKABLE QString timecode(int frame) const;
    Q_INVOKABLE QString sourceTimecode(int frame) const;
    // A frame count at the timeline's rate as a timecode from zero: a place
    // in a clip's source, which the timeline's starting timecode is no part of.
    Q_INVOKABLE QString frameTimecode(int frame) const;

signals:
    void decorationWindowRequested(double first, double last, int pixels);
    void compoundCreated(int index);
    void statusChanged();
    // An edit the worker was asked for changed nothing; the status says why.
    void editRefused(const QString &why);
    void projectBusyChanged();
    void recentProjectMissing(const QString &path);
    void viewerSettingsChanged();
    void playbackStatsChanged();
    void backgroundRenderChanged();
    void projectPlaybackReset();
    void frameExported(bool ok, const QString &path);
    void mediaHealthChanged();
    void mediaStatusRefreshed();
    void busyChanged();
    void libraryChanged();
    void clipAttributesFinished(int request, const QString &error);
    void mediaPlaybackChanged();
    void photoshopLayersSplit(const QStringList &paths, const QString &summary);
    void mediaPreviewsChanged();
    void mediaGangChanged();
    void timelineEntriesChanged();
    void decorChanged();
    void videoChanged();
    void positionChanged();
    void pastEndFramesChanged();
    void sourceChanged();
    void sourcePositionChanged();
    void playingChanged();
    void monitorVolumeChanged();
    void sourceFrameSaved(bool ok, const QString &path);
    void frameChanged();
    void sourceFrameChanged();
    void exportingChanged();
    void masterChanged();
    void adrCuesChanged();
    void captionSettingsChanged();
    void loopChanged();
    void recordingChanged();
    void takeRecorded(const QString &path, int track, int frame);
    void gradeClipChanged();
    void keyframeClipboardChanged();
    void colorClipboardChanged();
    void colorPasted(int track, int index, int node);
    void autoBalanceFinished(int track, int index, int node);
    void matchFinished(bool ok, const QString &message);
    void reframeToFitFinished(int track, int index, bool ok, const QString &message);
    void reframingChanged();
    void matchReferenceChanged();
    void matchColorRequested(int track, int index, int clipFrame, int refTrack, int refIndex, int refFrame);
    void colorClipsChanged();
    void scopeTickChanged();
    void scopesActiveChanged();
    void scopeOptionsChanged();
    void trackJobChanged();
    void lutFileChosen(const QString &path);
    void lutPreviewChanged();
    void lutApplyFinished(bool changed, const QString &message);
    void setNodeLutRequested(int track, int index, int node, const QString &nodeId, const QString &path, double intensity);
    void previewNodeLutRequested(int request, int track, int index, int node, const QString &nodeId, int frame, const QString &path, double intensity);
    void projectSettingsChanged();
    void preferencesChanged();
    void setProjectSettingsRequested(const QVariantMap &settings);
    void setAudioOutputRequested(const QString &deviceId);
    void setAudioBufferFramesRequested(int frames);
    void storageSettingsChanged();
    void unusedKeptMediaFound(const QVariantMap &report);
    void unusedKeptMediaRemoved(const QVariantMap &report);
    void keptMediaSweepRequested(const QStringList &documents, const QStringList &alsoUsed, bool remove);
    void pluginsChanged();
    void markersChanged();
    void recentChanged();
    void presetsChanged();
    void projectChanged();
    void projectSaveFinished(bool ok);
    void chooserCanceled();
    void exportDone(const QString &path);
    void failed(const QString &message);

    // Internal: queued requests to the worker thread.
    void importRequested(const QString &path);
    void previewRequested(const QString &path);
    void closePreviewRequested();
    void appendRequested(const QString &path, int in, int out, int takes = 0);
    void insertRequested(const QString &path, int position, int in, int out, int takes = 0);
    void overwriteRequested(const QString &path, int position, int in, int out, int takes = 0);
    void removeRequested(int index);
    void liftRequested(int index);
    void removeRangeRequested(int start, int end);
    void insertGapRequested(int position, int frames);
    void closeGapRequested(int track, int position);
    void deleteGapsRequested();
    void rollRequested(int index, int delta);
    void rollOnRequested(int track, int index, int delta);
    void trimOnRequested(int track, int index, int in, int out);
    void setVolumeKfRequested(int index, const QVariantList &kf);
    void setConnectedVolumeKfRequested(int index, const QVariantList &kf);
    void setOpacityRequested(int index, double alpha);
    void setOpacityKfRequested(int index, const QVariantList &kf);
    void makeCompoundRequested(int start, int end);
    void breakApartRequested(int index);
    void setDisabledRequested(int index, bool disabled);
    void connectRequested(const QString &path, int position, int in, int out,
                          bool audioOnly);
    void connectTitleRequested(const QString &text, int position, int length,
                               const QVariantMap &style);
    void updateTitleRequested(int index, const QString &text,
                              const QVariantMap &style);
    void removeConnectedRequested(int index);
    void moveConnectedRequested(int index, int newStart);
    void trimConnectedRequested(int index, int in, int out, int newStart,
                                bool ripple);
    void setConnectedRectRequested(int index, double x, double y, double w,
                                   double h);
    void splitRequested(int position);
    void splitAllRequested(int position);
    void trimToFrameRequested(int track, int index, int frame, bool end, bool ripple);
    void splitOnRequested(int track, int position);
    void addTransitionRequested(int track, int index, int length, const QString &style);
    void setTransitionSidesRequested(int track, int index, int before, int after);
    void removeConnectedDissolveRequested(int index);
    void resizeConnectedDissolveRequested(int index, int length);
    void restyleConnectedDissolveRequested(int index, const QString &style);
    void setConnectedFadesRequested(int index, int fadeIn, int fadeOut);
    void removeDissolveRequested(int index);
    void resizeDissolveRequested(int index, int length);
    void restyleDissolveRequested(int index, const QString &style);
    void setFadesRequested(int index, int fadeIn, int fadeOut);
    void toggleFadeEdgeRequested(bool connected, int index, bool outEdge, int length);
    void setVolumeRequested(int index, double db);
    void setConnectedVolumeRequested(int index, double db);
    void setPanRequested(int index, double pan);
    void setConnectedPanRequested(int index, double pan);
    void setEqRequested(int index, const QString &preset);
    void setConnectedEqRequested(int index, const QString &preset);
    void detachAudioRequested(int index);
    void setSpeedClipRequested(int index, double speed);
    void setCropRequested(int index, double l, double r, double t, double b, double feather, bool invert);
    void setConnectedCropRequested(int index, double l, double r, double t, double b, double feather, bool invert);
    void setConnectedRotationRequested(int index, double degrees);
    void setConnectedAnchorRequested(int index, double anchorX, double anchorY);
    void editPropertiesRequested(int track, int index, const QVariantMap &patch);
    void setFrameBlendRequested(int track, int index, int mode);
    void connectGeneratorRequested(const QString &resource, int position, int length, bool adjust);
    void setGeneratorColorRequested(int track, int index, const QString &color);
    void setConnectedColorRequested(int index, double brightness, double contrast,
                                    double saturation);
    void setColorRequested(int index, double brightness, double contrast,
                           double saturation);
    void trimRequested(int index, int in, int out, bool ripple);
    void moveRequested(int from, int to);
    void markerEditRequested(int request, const QVariantMap &target, int frame, const QString &type,
                             const QString &text, const QString &notes, bool remove, const QString &colour);
    void markerEditFinished(int request, bool changed, const QString &error);
    void retimeRequested(int request, const QVariantMap &target, double speed);
    void speedRampPrepareRequested(int request, const QVariantMap &target);
    void speedRampPrepared(int request, const QVariantMap &state);
    void speedRampRequested(int request, const QVariantMap &target, const QVariantMap &definition, bool normal);
    void retimeFinished(int request, bool changed, const QString &error);
    void audioEditRequested(int request, const QVariantMap &target, const QVariantMap &values, bool split);
    void audioEditFinished(int request, bool changed, const QString &error);
    void audioGestureRequested(int request, const QVariantMap &target, const QString &action,
                               const QVariantMap &changes, int sampleRate);
    void audioGestureFinished(int request, bool changed, const QString &error, const QVariantMap &selected);
    void clipboardPasteRequested(int request, const QVariantMap &target,
                                 const QVariantMap &clipboard, const QVariantMap &placement);
    void clipboardPasteFinished(int request, bool changed, const QString &error, const QVariantList &added);
    void toggleMarkerRequested(int frame);
    void removeMarkersInRequested(int first, int last);
    void setMarkerInfoRequested(int frame, const QString &text,
                                const QString &type, const QString &notes);
    void setMarkersRequested(const QVariantList &markers);
    void trackFlagRequested(int track, const QString &flag, bool on);
    void trackMixRequested(int track, const QString &key, const QVariant &value);
    void trackMixMapRequested(int track, const QVariantMap &mix);
    void masterMixRequested(const QString &key, const QVariant &value);
    void adrCuesRequested(const QVariantList &cues);
    void loopRequested(bool on, int in, int out);
    void scrubRequested(int frame);
    void resetLoudnessRequested();
    void setGradeRequested(int track, int index, const QVariantMap &grade);
    void applyColourSourceRequested(int track, int index, const QString &scope, const QVariantMap &fields);
    void setGradeVersionsRequested(int track, int index, const QVariantList &versions, int active);
    void editGradeVersionRequested(int track, int index, const QString &action, int version, const QString &name);
    void pasteColorRequested(int track, int index, const QString &kind, const QVariantMap &payload, int afterNode, const QString &parentPath = QString());
    void maskProviderChanged();
    void maskJobChanged();
    void prepareColorMaskRequested(int request,int track,int index,const QString &nodePath,int frame);
    void applyColorMaskRequested(const QVariantMap &target,const QString &path);
    void shareColorRequested(int track, int index, const QString &action, const QString &name, const QVariantList &addresses);
    void autoBalanceRequested(int track, int index, int clipFrame);
    void trackWindowRequested(int track, int index, int node, double x, double y, double w, double h);
    void trackWindowRangeRequested(int track, int index, int node, int window, int frame, int direction, bool singleFrame, const QString &nodePath = QString());
    void stopWindowTrackingRequested(bool keep);
    void tracksChanged();
    void destTrackChanged();
    void beatMapsChanged();
    void beatLinesChanged();
    // A mapping ended: with beats (`found`), without any, failed or cancelled.
    void beatMapFinished(const QString &path, bool found, const QString &message);
    void editTakesChanged();
    void rippleTrimChanged();
    void addTrackRequested(bool audio);
    void removeTrackRequested(int track);
    void swapTracksRequested(int a, int b);
    void placeOnTrackRequested(int track, int mode, const QString &path,
                               int position, int in, int out,
                               bool audioHint = false, int takes = 0);
    void fitToFillRequested(int track, const QString &path, int position, int frames,
                            int in, int out, bool audioHint);
    void tagNextPlacementRequested(const QVariantMap &tag);
    void cutToAngleRequested(int track, int index, int frame, int angle, bool keepAudio);
    void setMulticamSoundRequested(int track, int index, int source);
    void collapseMulticamRequested(const QString &sequenceId);
    void detachAudioOnRequested(int track, int index);
    void reattachAudioRequested(int track, int index);
    void setFlipRequested(int track, int index, bool h, bool v);
    void setAudioMapRequested(int track, int index, const QString &mode);
    void setNormalizeRequested(int track, int index, bool on);
    void freezeFrameRequested(int track, int index, int frame, int length);
    void syncByAudioRequested(const QStringList &paths, double syncFps, const QVariantMap &sources);
    void decodeAnglesRequested(const QStringList &paths, const QList<int> &frames, int seq);
    void multicamChanged();
    void angleFramesChanged();
    void syncingChanged();
    void slideRequested(int track, int index, int delta);
    void joinRequested(int track, int index);
    void moveClipToRequested(int track, int index, int newTrack, int newStart);
    void pasteRequested(int track, int position, const QVariantMap &entry,
                        bool insert);
    void nudgeRequested(int track, int index, int delta);
    void removeClipsRequested(const QVariantList &addrs, bool ripple);
    void nudgeClipsRequested(const QVariantList &addrs, int delta);
    void moveClipsRequested(const QVariantList &moves);
    void setClipsDisabledRequested(const QVariantList &addrs, bool disabled);
    void setClipsLinkedRequested(const QVariantList &addrs, bool link);
    void setLabelsRequested(const QVariantList &addrs, const QString &label);
    void nestSequenceRequested(int index, int position);
    void nestSequenceOnTrackRequested(int index, int track, int position, bool audio);
    void renderAndReplaceRequested(int track, int index);
    void restoreOriginalRequested(int track, int index);
    void renderJobChanged();
    void pasteAttributesRequested(const QVariantList &addrs, const QVariantMap &source,
                                  const QStringList &groups);
    void setBypassRequested(int track, int index, const QString &key, bool on);
    void setClipMarkersRequested(int track, int index, const QVariantList &markers);
    void editClipMarkerRequested(int track, int index, const QVariantMap &marker, bool remove);
    void importCaptionsRequested(const QVariantList &cues, const QVariantMap &style, const QVariantMap &language);
    void placeCaptionsRequested(const QVariantList &cues, const QVariantMap &style,
                                const QVariantList &remove, const QString &label);
    void captionStyleRequested(const QVariantMap &styles, bool everyCaption);
    void restyleCaptionRequested(int index, const QVariantMap &style, const QVariantList &place, const QString &label);
    void captionSpeakerRequested(const QVariantList &indices, const QString &speaker, const QVariantMap &settings);
    void captionSettingsRequested(const QVariantMap &settings, const QString &label);
    void captionTextsRequested(const QVariantList &changes, const QString &label);
    void retimeCaptionsRequested(const QVariantList &changes, const QString &label);
    void renameCaptionSpeakerRequested(const QString &from, const QString &to, const QVariantMap &settings);
    void duplicateCaptionTrackRequested(int track, const QVariantMap &language, const QString &name);
    void splitManyRequested(int track, const QVariantList &positions, const QString &label);
    void editsDetected(int count);
    void detectingEditsChanged();
    void setMixAlignRequested(int track, int index, const QString &align);
    void projectCreateRequested(int request, const QString &anchorId, const QVariantMap &settings);
    void projectCreateFinished(int request, bool created, const QString &error);
    void sequenceCreateRequested(int request, const QString &anchorId, const QString &name, const QVariantMap &settings);
    void timelineCreationRequested();
    void sequenceCreateFinished(int request, bool created, const QString &error);
    void addSequenceRequested(const QString &name);
    void addSyncedClipRequested(const QString &name, const QVariantList &items, bool silenceCameras,
                                const QVariantMap &format);
    void setCategoriesRequested(const QVariantList &addrs, const QString &category);
    void importSequenceRequested(const QString &name, const QVariantList &snapshot, const QStringList &trackNames, const QVariantList &markers, const QVariantMap &format, const QStringList &problems);
    void timelineImported(const QString &name, const QStringList &problems);
    void stackedSequenceChanged();
    void placeEntryRequested(int track, int position, const QVariantMap &entry, bool insert);
    void switchSequenceRequested(int index);
    void renameSequenceRequested(int index, const QString &name);
    void moveSequenceToBinRequested(int index, const QString &binId);
    void duplicateSequenceRequested(int index);
    void closeSequenceRequested(int index);
    void setSequencesRequested(const QVariantList &sequences, int active);
    void sequencesChanged();
    void timelineItemsChanged();
    void setBlendRequested(int track, int index, int mode);
    void setScalingRequested(int track, int index, const QString &scaling);
    void setLensRequested(int track, int index, double k1, double k2, double cx, double cy);
    void motionPresetRequested(int track, int index, const QString &preset, const QVariantMap &options);
    void findKeyBackdropRequested(int track, int index, const QString &uid, int clipFrame, double x, double y);
    void videoEffectRequested(int track, int index, const QString &action,
                               const QString &uid, const QVariantMap &values);
    void stabilizeRequested(int track, int index, int shakiness, int smoothing);
    void removeStabRequested(int track, int index);
    void stabJobChanged();
    void compareRequested(bool on);
    void compareModeChanged();
    void beforeFrameChanged();
    void nodePresetsChanged();
    void colorMatteChanged();
    void effectPreviewChanged();
    void colorAssetChosen(const QString &action, const QString &path);
    void colorMatteRequested(int request, int track, int index, const QString &nodeId, int frame);
    void effectPreviewRequested(int request, int track, int index, int frame, bool matte, const QSize &size);
    void twoUpEdgeChanged();
    void twoUpFramesChanged();
    void twoUpEdgeRequested(int edge);
    void pasteClipsRequested(const QVariantList &entries, int position);
    void setLabelRequested(int track, int index, const QString &label);
    void trackNameRequested(int track, const QString &name);
    void historyChanged();
    void undoRequested();
    void redoRequested();
    void seekRequested(int frame);
    void skimRequested(int frame);
    void audioPositionChanged();
    void audioPositionRequested(quint64 request, const QVariantMap &context, const QVariantMap &position, bool audition);
    void speedRequested(double speed);
    void setSpeedConnectedRequested(int index, double speed);
    void exportRequested(const QString &path, const QVariantMap &spec);
    void exportPauseRequested();
    void exportResumeRequested();
    void exportCancelRequested();
    void renderFolderChosen(const QString &path);
    void saveProjectRequested(const QString &path, const QVariantMap &libraryState);
    void saveRecoveryRequested(const QString &path, const QString &origin, const QVariantMap &libraryState, const QString &token);
    void prepareProjectArchiveRequested(const QVariantMap &libraryState, const QString &token);
    void safetyChanged();
    void archiveFolderChosen(const QString &folder);
    void loadProjectRequested(const QString &path);
    void newProjectRequested();

private:
    QString prepareClipAttributes(const QVariantList &indexes, const QVariantMap &attributes, QVariantList *proposed);
    void restoreClipAttributes(const QVariantMap &attributes);
    bool m_clipAttributesPending = false;
    int m_clipAttributesRequest = 0;
    CommandRegistry *m_commands = nullptr;
    WorkspaceLayouts *m_workspaceLayouts = nullptr;
    EffectShelf *m_effectShelf = nullptr;

    void setStatus(const QString &status);
    void sayRefused(const QString &why);
    void setBusy(bool busy);
    void setDirty(bool dirty, bool workerUpdate = false);
    void addRecent(const QString &path);
    QVariantMap libraryState() const;
    void refuseProjectSave();
    QStringList reservedRenderOutputs() const;
    void applySpeed(double speed);
    void setupAudioCursor();
    void cancelAudioSeek(bool clearPosition);
    void dispatchAudioSeek();
    void syncAudioPosition(int frame);
    int audioPictureFrame(const QVariantMap &position) const;
    QVariantMap m_audioPosition, m_requestedAudioPosition, m_pendingAudioSeek;
    quint64 m_audioSeekSerial = 0, m_audioSeekInFlight = 0;
    double m_speedAfterAudioSeek = 0.;
    void onImported(bool ok, const QString &path, const QVariantMap &meta,
                    const QImage &thumbnail);
    void onTimeline(const QVariantList &entries, int duration, double fps,
                    int width, int height, bool canUndo, bool canRedo, bool contentEdit = true, const QVariantMap &format = {},
                    const QVariantMap &audioEditContext = {});
    int requestAudioEdit(const QVariantMap &target, const QVariantMap &values, bool split);
    QVariantMap m_audioEditContext;
    int m_audioEditRequest = 0;
    void onWaveform(int index, int start, const QVariantMap &wave);
    void onConnectedWaveform(int index, int start, const QVariantMap &wave);
    void onConnectedFilmstrip(int index, int start, const QStringList &keys,
                              const QList<QImage> &tiles, const QVariantMap &layout);
    void setupAudio();
    void setupBeats();
    // The beat maps the library holds, by file, read again when it changes.
    // `firstBar`: which beat is the first of a bar, -1 for a map without the bar.
    struct BeatTimes { QVector<double> beats; double duration = 0, bpm = 0; int firstBar = -1, perBar = 4; };
    struct BeatAt { int frame = 0, level = 0; bool bar = false; };
    QHash<QString, BeatTimes> m_beatTimes;
    QString m_beatSignature;
    BeatMapper *m_beatMapper = nullptr;
    int m_beatMapRevision = 0;
    void readBeatMaps();
    void storeBeatMap(const QString &path, const QVariant &map);
    QVector<BeatAt> beatLinesOfEntry(const QVariantMap &entry) const;
    QVector<int> beatsOfEntry(const QVariantMap &entry) const;
    mutable QVariantList m_beatLines;
    mutable bool m_beatLinesValid = false;
    void setupColor();
    ColorMaskService *m_maskService=nullptr;
    QVariantMap m_maskProvider,m_maskJob,m_maskTarget,m_maskRequest;
    int m_maskSerial=0;
    void setupColorTools();
    QString m_colorAssetAction, m_colorAssetId;
    QVariantList m_nodePresets;
    void sendColorMatte();
    QImage m_colorMatte;
    QVariantMap m_pendingColorMatte;
    int m_colorMatteSerial = 0;
    bool m_colorMatteBusy = false;
    void sendEffectPreview();
    QImage m_effectPreview;
    QVariantMap m_pendingEffectPreview;
    int m_effectPreviewSerial = 0;
    bool m_effectPreviewBusy = false;
    void setupProject();
    void setupProjectSafety();
    void resetRecoverySession();
    void requestProjectSave(const QString &path);
    void onProjectSettings(const QVariantMap &settings, bool rateLocked);
    void refreshGradeClip();
    void refreshColorClips();
    void renderScopes();
    void cancelScopes(bool clear=false);
    void refreshStillThumbs();
    void onFilmstrip(int index, int start, const QStringList &keys,
                     const QList<QImage> &tiles, const QVariantMap &layout);
    void onSourceStrip(const QString &path, const QStringList &keys,
                       const QList<QImage> &tiles);
    void onLibraryStrip(const QString &path, const QStringList &keys,
                        const QList<QImage> &tiles);
    void onLibraryWave(const QString &path, const QVariantMap &wave);
    // One library entry's field, announced and dirtied. False when the index
    // is out of range.
    bool setLibraryField(int libraryIndex, const QString &key, const QVariant &value);
    bool libraryOffline(int libraryIndex) const;
    // The logging fields a project line carries for one entry (empty = none).
    static QVariantMap loggingOf(const QVariantMap &entry);
private slots:
    void onFrame(const QImage &image, int position);
private:
    void showFrame(const QImage &image, int position);
    void onFrameScheduled(const QImage &image, int position, qint64 dueNs);
    void onScheduleCleared(int settled);
    void presentScheduled();
    void onBeforeFrame(const QImage &image);
    void onStabProgress(int track, int index, int percent);
    void onStabApplied(int track, int index, double zoom, double steadied);
    void onSequences(const QVariantList &sequences, int active);
    void onPreviewOpened(bool ok, const QString &path, int duration, double fps);
    void onPreviewFrame(const QImage &image, int position, bool placed = false);
    void flushPendingSeek(bool fromSource);
    void setRangeMark(bool markIn);
    // Resolves an entry's marked range to a concrete (in, out), or (-1, -1).
    static QPair<int, int> rangeOf(const QVariantMap &entry);

    enum class ChooserMode { Import, ImportFolder, Export, SaveProject, OpenProject, LocateProject, ExportFrame,
                             ImportCaptions, ExportCaptions, RenderFolder, SoundFolder, LutFolder, LutFile, ColorAsset, ArchiveFolder,
                             ImportTimeline, ExportTimeline };
    QString m_exportTimelineFormat;
    // A timeline import waits for its media import task before placing.
    QString m_timelineImportTask; QVariantMap m_timelineImportDocument; QString m_timelineImportName;
    // What to say of the rate an EDL was read at, once it is in ("" otherwise).
    QString m_timelineImportRateNote;
    QPair<int, int> timelineRate() const;
    void placeImportedTimeline();
    // AAF goes through a Python helper that can take minutes on a long
    // timeline: it runs on one background thread, shown as a task that cancels.
    QThreadPool *m_interchangePool = nullptr;
    std::shared_ptr<std::atomic_bool> m_aafCancel;
    QString m_aafTaskId;
    void continueTimelineImport(const QVariantMap &document, const QString &fallbackName);
    void beginAafTask(const QString &name, const QString &file);
    void endAafTask(const QString &state, const QString &message);
    void setupLuts();
    void sendPendingLutPreview();

    void setupDeliver();
    QVariantList captionCuesForRender() const;
    int captionTrackIndex() const;
    QStringList missingMedia() const;
    QStringList missingServices() const;
    QVariantMap renderContext(const QVariantMap &spec, const QString &path) const;
    QVariantMap renderEstimateFrom(const QVariantMap &spec, const QVariantMap &context) const;
    // What the checks need of the timeline that no render setting changes:
    // the renderer's own check of every clip above all, which on a long edit
    // takes longer than a frame. Kept while the timeline, its tracks, the
    // project's settings and the size and rate are the ones it was made of
    // (lists the timeline still shares compare at once).
    struct RenderTimelineChecks {
        bool valid = false;
        QVariantList timeline, connected, tracks;
        QVariantMap project;
        QVariantList categories;
        int width = 0, height = 0;
        double fps = 0;
        bool engine = false;
        bool hasAudio = false;
        // missingMedia()'s look at the clips: their grades' and effects'
        // problems (reading each LUT), and each file the timeline plays once,
        // with its name and whether the worker found it missing. Whether the
        // files are there is looked at every time.
        QStringList clipProblems;
        QList<std::tuple<QString, QString, bool>> media;
        QVariantList audioTracks, categoriesInUse;
        QStringList missingServices;
        QVariantList eightBitEffects;
        bool managedOutput = false;
        QString linearError;
    };
    const RenderTimelineChecks &renderTimelineChecks() const;
    mutable RenderTimelineChecks m_renderTimelineChecks;
    RenderQueue *m_renderQueue = nullptr;
    RenderPresets *m_renderPresets = nullptr;
    ViewerProcessor *m_viewerProcessor = nullptr;
    ScopeProcessor *m_scopeProcessor = nullptr;
    HardwareSettings *m_hardware = nullptr;
    void setupHardware();
    bool m_quitWhenIdle = false;

    FileChooser *m_chooser;
    ThumbnailProvider *m_thumbs = nullptr;
    bool m_decorationWindowSet = false;
    double m_decorationFirst = 0, m_decorationLast = 0;
    int m_decorationPixels = 0;
    QSet<QString> m_decorationAddresses;
    void pruneTimelineDecor();
    bool wantsTimelineDecor(const QVariantMap &entry) const;
    void scheduleDecorChanged();
    bool m_decorNotifyPending = false;
    quint64 m_decorationRevision = 0;
    QStringList m_recentLibraryWaves;
    ChooserMode m_chooserMode = ChooserMode::Import;
    void setupPlayback();
    void resetProjectPlayback();
    void notePresentationFrame();
    void syncSourceLoopRange();
    void saveFrameImage(const QString &path, bool source);
    QVariantMap m_viewerSettings;
    bool m_projectLoadPending = false, m_exportFrameSource = false;
    QString m_locateRecentPath, m_replacementRecentPath;
    QElapsedTimer m_presentationClock;
    QTimer m_presentationTimer;
    quint64 m_presentationSerial = 0, m_lastPlaybackSerial = 0;
    quint64 m_presentedSerial = 0, m_presentedCount = 0;
    int m_droppedFrames = 0;
    qint64 m_playbackElapsedMillis = 0;
    // Timeline pictures the worker made ahead, shown when due (presentScheduled).
    struct ScheduledFrame { QImage image; int position = 0; qint64 dueNs = 0; };
    QList<ScheduledFrame> m_scheduledFrames;
    QTimer m_scheduledTimer;
    QString m_status;
    bool m_busy = false;
    bool m_exporting = false;
    int m_exportPercent = 0;
    QString m_appendAfterImport; // path queued for append once its import lands
    QString m_projectPath;
    // Ranges read from a project file, applied when each clip's import lands.
    QString m_previewRequestPath; // the source preview last asked for; older replies are ignored
    QString m_previewShownPath; // identifies the displayed source when linking viewer positions
    QHash<QString, QPair<int, int>> m_pendingRanges;
    struct ImportMark { bool dirty; QString undoText; };
    QHash<QString, ImportMark> m_importMarks; // dirty state before an import, restored if it fails
    // Logging fields read from a project file, merged when the import lands.
    QHash<QString, QVariantMap> m_pendingMeta;
    // Subclips read from a project file: copies of their source entry made
    // once that entry's import lands (a path imports only once).
    QMultiHash<QString, QVariantMap> m_pendingSubclips;
    Volumes *m_volumes = nullptr;
    bool m_mediaStatusPending = false;
    // The project file's library order, so entries that import asynchronously
    // land in their line's place beside the offline ones added at once.
    QStringList m_restoreOrder;
    // Generate Captions: clips still to transcribe (paths), and those already
    // tried this round, so a clip with no speech is not tried again.
    QStringList m_captionQueue, m_captionTried;
    std::unique_ptr<QProcess> m_detectProc;
    QVariantMap m_matchReference; // {track, index, frame, name, path, link}
    std::unique_ptr<QProcess> m_matchProc;
    std::unique_ptr<QProcess> m_reframeProc;
    // A tool's process that could not be started, or that has gone quiet,
    // must still come to an end: see watchToolProcess.
    void watchToolProcess(QProcess *process, const std::function<void()> &failedToStart);
    QVariantList m_reframeQueue;
    void reframeNext();
    // The entry whose sound plays for a clip: itself, or the linked sound
    // under a picture that has none of its own.
    QVariantMap soundEntryFor(const QVariantMap &entry) const;
    bool generateCaptionsNow(bool afterTranscribing);
    void continueCaptionQueue(bool ok);
    QVariantMap captionStyle() const; // the look new captions are made with
    static QVariantMap defaultCaptionStyle();
    // `track` when it is a caption track (-1: the first caption track), else -1.
    int captionTrackAt(int track) const;
    // The caption style keys of `input` checked and set on `style` (the
    // lengths Generate Captions keeps to only with `lengths`). False with the
    // reason in *why for an unknown key or a value out of range.
    bool applyCaptionStyleKeys(QVariantMap &style, const QVariantMap &input, bool lengths, QString *why) const;
    bool languageFor(const QString &code, const QString &name, QVariantMap *language);
    bool m_regeneratingCaptions = false;
    // The caption list: settings, rows and timing (backendcaptionlist.cpp).
    QVariantMap captionSettings() const { return m_captionSettings; }
    void setupCaptionList();
    // The captions as the checks take them: [{index, track, start, frames, text}].
    QVariantList captionRows() const;
    // The caption settings with `name` among the speakers (a colour of its own).
    QVariantMap captionSettingsWithSpeaker(const QString &name) const;
    // The Export page's warnings for the captions' problems.
    QStringList captionCheckWarnings() const;
    // The captions named (all when none are; -1 entries dropped), checked to
    // be captions: false when one is not.
    bool chosenCaptions(const QVariantList &connectedIndices, QList<int> *out) const;
    // Sends the new times [{index, start, frames}] as one undo step `label`;
    // the number that change.
    int retimeCaptionsTo(const QVariantList &changes, const QString &label, const QString &none);
    // Clips Generate Captions transcribes with each channel a speaker (paths).
    QSet<QString> m_twoChannelTranscripts;
    // Export Captions…'s choice while its file chooser is open.
    QString m_captionExportFormat = QStringLiteral("srt");
    int m_captionExportTrack = -1;
    QSet<QString> m_restorePending;
    QVariantList m_markers; // {frame,text,type} mirror of the worker's list
    QVariantList m_tracks;  // header state mirror, see Timeline::tracks()
    int m_destTrack = 0;
    int m_editTakes = 0; // EditTakes: both, picture, sound
    // What an edit of this library clip takes: the choice, or the whole clip
    // where the choice asks for a picture or a sound the clip does not have.
    int takesFor(int libraryIndex, const QString &asked = {}) const;
    static QString takenName(const QString &name, int takes);
    bool m_rippleTrim = false;
    QString m_exportPreset = QStringLiteral("h264-high"); // sticky per session
    QStringList m_recent; // most recent first, persisted via QSettings
    bool m_linearPlayback = false;
    QVariantMap m_mediaHealth;
    BackgroundTasks *m_backgroundTasks = nullptr;
    bool m_backgroundRenderEnabled = true;
    int m_backgroundRenderLimit = 20;
    QString m_backgroundRenderProcessing = "auto";
    QVariantMap m_backgroundRender{{"state", "idle"}, {"percent", -1}, {"ready", 0}, {"total", 0},
                                 {"ranges", QVariantList()}, {"readyRanges", QVariantList()}};
    QTimer m_feedbackSync;
    QHash<QString, QString> m_workerTaskIds;
    QString m_archiveTaskId;
    void setupFeedback();
    void syncFeedback();
    void refreshMediaHealth();
    void workerTaskProgress(const QString &kind, const QString &name, int percent);
    void sweepKeptMedia(bool remove);

    QThread m_thread;
    PlayerWorker *m_worker;
    QSize m_viewerDrawSize;
    QVariant m_viewerDisplay, m_playbackDisplay;
    QSize m_sourceDrawSize;      // what the worker was last told of the source viewer
    QVariant m_sourceDisplay;
    QVariant m_playbackScopes;  // what the worker was last told of the scopes (sendPlaybackDisplay)
    void sendPlaybackDisplay();
    static bool scopesReadBack();
    QVariantMap measuredScopeOptions() const;
    QImage m_frame;       // timeline (program) frame
    QImage m_beforeFrame; // comparison view: the frame with every attribute off
    int m_twoUpEdge = -1;
    QImage m_twoUpOutgoing, m_twoUpIncoming;
    bool m_compareMode = false;
    QVariantMap m_stabJob;
    bool m_timelineCreationPrompt = false;
    struct PendingTimelinePlacement {
        std::function<void(const QVariantMap &)> execute;
        std::function<void(const QString &)> cancel;
    };
    QList<PendingTimelinePlacement> m_pendingTimelinePlacements;
    QVariantMap m_nextTimelinePlacementTag;
    QString m_pendingTimelineSource;
    QString m_pendingTimelineAnchor;
    int m_pendingTimelineGeneration = -1;
    int m_pendingTimelineCreateRequest = 0;
    bool m_pendingTimelinePrompted = false;
    QHash<int,QVariantMap> m_sequenceCreateContexts;
    void dispatchTimelinePlacement(const QString &path, std::function<void()> placement,
                                   const QVariantMap &sourceAction = {});
    void queueTimelinePlacement(const QString &path, PendingTimelinePlacement placement);
    QVariantMap timelinePlacementTarget() const;
    QVariantList m_sequences; // the worker's full list: [{name, snapshot, position}]
    Transcriber *m_transcriber = nullptr;
    int transcriptClip(const QVariantMap &job) const;
    void storeTranscript(int index, const QVariantList &segments, const QString &language);
    void transcriptionFinished(bool ok, int index, const QString &error);
    int m_stackedSequence = -1;
    QString m_stackedId;      // survives reordering/renaming: matched back by id
    QVariantList m_presets;   // attribute presets, persisted as JSON in QSettings
    QVariantMap m_renderJob;  // {track, index, percent} while a Render & Replace runs
    int m_activeSequence = 0;
    double m_monitorVolume = 1.0;
    QVariantMap m_monitorSettings;
    QString m_recordCue;
    QTimer *m_adrStopTimer = nullptr;
    QImage m_sourceFrame; // previewed library clip frame
    QVariantList m_library;
    MediaCatalog *m_mediaCatalog = nullptr;
    MediaTasks *m_mediaTasks = nullptr;
    int m_mediaGeneration = 0;
    int m_markerEditRequest = 0;
    int m_retimeRequest = 0;
    int m_sequenceCreateRequest = 0, m_projectCreateRequest = 0, m_configuredProjectRequest = 0;
    void setupMedia();
    // The constructor's wiring, in the order it is made.
    void createParts();
    void wireFileChooser();
    void wireClipPlacement();
    void wireEdits();
    void wireTimelines();
    void wireViewer();
    void wireProjectFiles();
    void wirePictures();
    void onProjectLoaded(bool ok, const QString &path, const QStringList &library, const QVariantList &records);
    void syncMediaSources();
    void applyMediaCsv(const QString &id, const QVariantMap &result);
    QString m_mediaPlayback = QStringLiteral("preferred");
    bool m_photoshopSplitting = false;
    bool m_aafProbing = false, m_aafProbed = false; // the first look for the AAF module, off the GUI thread
    QVariantMap m_lastMediaSources;
    int m_mediaSourceSyncs = 0;
    // The table of media sources is built from the whole library, so it is
    // built only for a change that can alter it. A change to a clip's logging
    // or thumbnails announces the library without it; a setter that walks a
    // selection holds it and builds it once at the end.
    void announceLibraryKeepingSources();
    struct SourceSyncHold {
        explicit SourceSyncHold(Backend *backend) : m_backend(backend) { ++m_backend->m_sourceSyncHeld; }
        ~SourceSyncHold() { if (--m_backend->m_sourceSyncHeld == 0 && std::exchange(m_backend->m_sourceSyncWanted, false)) m_backend->syncMediaSources(); }
        Q_DISABLE_COPY_MOVE(SourceSyncHold)
    private:
        Backend *m_backend;
    };
    int m_sourceSyncHeld = 0;
    bool m_sourceSyncWanted = false, m_libraryChangeKeepsSources = false;
    QHash<QString, QVariantMap> m_relinkResults;
    QVariantMap m_mediaPreviews;
    QHash<QString, QVariantMap> m_pendingMediaPreview;
    QSet<QString> m_mediaPreviewInFlight;
    bool m_mediaGang = false;
    void sendMediaPreview(const QString &purpose, const QVariantMap &request);
    bool mediaTaskCurrent(const QString &id) const;
    // Multicam state (see the properties).
    bool m_multicamMode = false;
    bool m_multicamAudioFollows = true;
    QVariantMap m_multicamClip;
    QStringList m_angleFrames;
    QStringList m_angleKeys; // thumbnail keys in use, dropped on the next set but one
    QStringList m_angleKeysBefore; // those of the set before, which a cell may still be loading
    int m_angleSeq = 0;
    bool m_anglesPending = false;
    bool m_anglesDirty = false;
    bool m_syncing = false;
    QVariantMap m_pendingMulticam; // {name, indices} while a waveform sync runs
    double placementRate(const QVariantMap &source) const;
    QVariantMap placementSource(int libraryIndex); // resolves + tags multicam
    QVariantMap angleSource(const QVariantMap &entry, int angle) const;
    QString multicamJson(const QVariantMap &entry) const;
    QVariantList soundSourceRows(const QVariantMap &camera) const;
    void finishMulticam(const QVariantMap &pending, const QList<int> &offsets);
    void startSync(const QString &name, const QVariantList &libraryIndices, const QString &method,
                   const QVariantMap &synced, int soundFrom = -1);
    void finishSyncedClip(const QVariantMap &pending, const QList<int> &offsets);
    void onClipsSynced(const QStringList &paths, const QList<int> &offsets,
                       const QString &message);
    void onAnglesReady(const QList<QImage> &frames, int seq);
    void updateMulticamClip();
    void requestAngleFrames();
    void refreshMulticamThumbs();
    QVariantList m_timeline;  // storyline entries (lane 0)
    QVariantList m_connected; // connected-lane entries (lane 1)
    QVariantList m_timelineDecor;  // {strip, wave} per storyline entry
    QVariantList m_connectedDecor; // {wave} per connected entry
    int m_thumbKey = 0;
    int m_duration = 0;
    double m_fps = 0.0;
    int m_videoWidth = 0;
    int m_videoHeight = 0;
    int m_position = 0;
    int m_pastEndFrames = 0;
    double m_speed = 0.0;
    quint64 m_speedRequest = 0;
    quint64 m_editGestureSerial = 0, m_editGesture = 0;
    QString m_undoText, m_redoText;
    bool m_canUndo = false;
    bool m_canRedo = false;
    bool m_seekInFlight = false;
    // Seeks sent and not yet answered, oldest first, for the timeline and
    // the source: a picture for one a newer one has overtaken shows, but
    // neither moves the playhead nor stands for the newest.
    QList<int> m_sentSeeks[2];
    void sentSeek(bool source, int frame);
    bool overtaken(bool source, int position);
    // Skimming: the playhead's picture while another is shown, and one
    // skim picture asked for at a time (the newest waiting).
    bool m_skimActive = false, m_skimInFlight = false;
    int m_pendingSkim = -1;
    QImage m_skimHome;
    void onSkimFrame(const QImage &image, int frame);
    void onSkimSettled(const QImage &image, int frame);
    int m_skimShown = -1;  // the frame the skim's picture is of
    int m_pendingSeek = -1;
    // Which view each seek was aimed at (m_viewingSource at queue time) —
    // only a frame from that view may answer or flush it.
    bool m_seekInFlightSource = false;
    bool m_pendingSeekSource = false;
    bool m_dirty = false;
    quint64 m_projectRevision = 0, m_saveRevision = 0;
    bool m_savePending = false, m_autosavePending = false;
    QString m_recoveryPath, m_recoveryToken, m_recoveryLoadPath, m_lastRecoverySave, m_archiveToken;
    QStringList m_discardedRecoveryPaths;
    std::unique_ptr<QLockFile> m_recoveryLock;
    QVariantList m_recoveryCopies, m_projectBackups;
    int m_backupLimit = 20;
    ProjectArchive *m_projectArchive = nullptr;
    QVariantMap m_archiveRequest;
    bool m_restoringLibrary = false;
    // A sequence list this side just handed the worker (open, load) — its
    // echo is not an edit.
    bool m_sequencesEchoPending = false;
    // Offline library entries being re-imported because their file is
    // back: path → the row the fresh entry takes.
    QHash<QString, int> m_relinkAt;
    AudioMeters *m_audioMeters = nullptr;
    // Audio page.
    QVariantMap m_master;
    QVariantList m_adrCues;
    QVariantMap m_captionSettings; // the worker's Timeline::captionSettings
    SoundLibrary *m_soundLibrary = nullptr;
    Recorder *m_recorder = nullptr;
    bool m_loop = false;
    int m_loopIn = -1, m_loopOut = -1;
    LoudnessPass *m_loudnessPass = nullptr;
    QString m_auditionPath;
    int m_recordTrack = -1;
    int m_recordStart = 0;
    // Color page.
    QVariantMap m_gradeClip;
    QVariantMap m_copiedGrade;
    QVariantMap m_copiedNode;
    QVariantList m_colorClips;
    int m_scopeTick = 0;
    bool m_scopesActive = false;
    QVariantMap m_scopeOptions;
    int m_scopeRevision = 0;
    QVariantMap m_trackJob;
    StillsGallery *m_stills = nullptr;
    QVariantMap m_keyframeClipboard; // {editKeys: {property: [keys]}, opacityKf: [keys]}, first key at 0
    LutLibrary *m_lutLibrary = nullptr;
    QVariantMap m_lutPreview;
    QVariantMap m_pendingLutPreview;
    int m_lutPreviewSerial = 0;
    bool m_lutPreviewBusy = false;
    // Project settings + preferences.
    QVariantMap m_projectSettings;
    QVariantMap m_timelineFormat; // exact profile belonging to the announced frame coordinates
    bool m_projectRateLocked = false;
    QString m_audioOutput;
    int m_audioBufferFrames = 6;
    QString m_monitorMapping = QStringLiteral("stereo"), m_inputChannel = QStringLiteral("both");
    QVariantList m_plugins;
    int m_autosaveMinutes = 5;
    QString m_newTimelineStart = QStringLiteral("00:00:00:00");
    int m_defaultColorspace = 709;
    QTimer m_autosave;
    bool m_viewingSource = false;
    int m_sourcePosition = 0;
    int m_sourceDuration = 0;
    double m_sourceFps = 0.0;
    int m_previewLibraryIndex = -1;
    void libraryRowsMoved(int at, int count);
    // Match Frame: applied when the preview opens.
    int m_pendingSourceSeek = -1;
    int m_pendingSourceRangeIn = -1;
    int m_pendingSourceRangeOut = -1;
};
