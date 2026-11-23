# OmaEdit command-line interface

OmaEdit has a headless, JSON command interface for agents and scripts. It uses the
same project, timeline, colour, audio and export engines as the desktop editor.
No display server or open editor window is required. The interface version is 1.

## Agent skill installation

The skill travels with the application at `/usr/share/omaedit/skills/omaedit/`
for Arch packages, and alongside local test installs and source builds. Run as
the user who will use the agent:

```sh
omaedit skill          # JSON: path of the bundled SKILL.md, agent locations and link status
omaedit skill --link   # register it; safe to repeat
```

`skill` reports where the bundled `SKILL.md` lives, not its contents. Each
`places` entry says whether that location is `linked`; a location holding
something else shows `linked:false` until `--link` is used.

Following OmaShow's Omarchy arrangement, this registers links in `~/.agents/skills`
and `~/.claude/skills`, plus Codex, Pi and Hermes locations for installed agents
and each Hermes profile. Codex honours `CODEX_HOME` when set. The package itself
never writes to users' home directories. Start a new agent session after linking.
The links follow package updates and local test-build updates without relinking.
A build run from a source checkout has its own copy of the skill, so it reports
links made by an installed package as `linked:false` and treats them as
conflicts; run `--link` from the copy you want agents to use.

With `--link`, conflicting files, directories and broken links are preserved
and reported in that entry's `note`, with `skill_link_failed` and a nonzero exit
status; other locations can still be registered. Explicit
`omaedit skill --link --force` first moves conflicts to uniquely named backups,
reported in each affected `places` entry. `skill` does not load the editing engine,
open a window or access project recovery. The skill points agents to this built-in
manual and the installed operation catalogue rather than duplicating the API.

## Entry points

```sh
omaedit --help
omaedit --version
omaedit guide                       # complete built-in manual as JSON text
omaedit ops                         # operation catalogue and property types
omaedit ops Grade                   # filter the catalogue
omaedit defaults                    # project/timeline/grade/render/audio defaults
omaedit new edit.omaedit --timeline Main \
  --settings '{"custom":true,"width":1920,"height":1080,"fpsNum":25,"fpsDen":1}'
omaedit inspect edit.omaedit
omaedit apply edit.omaedit --ops edits.json --output revised.omaedit
omaedit apply edit.omaedit --ops edits.json --in-place
omaedit call edit.omaedit editor.sequenceEntries --args '{"index":0}'
omaedit export revised.omaedit --output film.mp4 \
  --spec '{"container":"mp4","vcodec":"h264","audio":true}' --progress
omaedit frame revised.omaedit --frame 25 --output frame.png
omaedit serve revised.omaedit        # persistent JSON-lines session on stdin/stdout
```

Options may come before or after the command word (`omaedit [options] command`).
A word that is neither a command nor an existing file is a `usage` error with a
nonzero exit status; `omaedit clip.mp4` or `omaedit edit.omaedit` opens the desktop
editor as usual.

`new` creates a project; `--timeline NAME` also creates its first timeline. Omit
that option to import and organise media before making a timeline; such a
media-only project lists no sequences in `inspect`. Timeline
settings include dimensions, rational frame rate (`fpsNum`/`fpsDen`), colour
space, audio sample rate/channels and starting frame (`startFrame`, counted at
the timeline's rate; give `startRate`, whole frames a second, when the count
was made at another rate or before the rate is known, and the same timecode is
kept; a start of 24 hours or more is refused, as timecode has no such hour).
With no `startFrame` given, `timeline.create` starts the timeline at the
user's "New timelines start at" preference, which `defaults` shows as the
timeline's `startFrame` and `startRate`. `custom:false` matches the first clip. Create a timeline before placing clips; CLI placement reports `timeline_required` when none exists. Use `defaults` to obtain complete editable settings.
Unknown keys in `--settings`, `timeline.create` settings or a render `spec` are
rejected with `invalid_argument` naming the key, so a misspelling never falls
back silently to a default. Like the desktop editor, a saved project keeps its
grade stills gallery in a `Stills` folder beside it.

`--settings`, `--spec` and `--args` accept an inline JSON object or `@file.json`.
`apply --ops FILE` reads a JSON array; `--ops -` reads it from stdin. Local media
and project paths are resolved from the process's working directory.

Every command emits one JSON result on stdout. Diagnostic messages and optional
render progress go to stderr. Exit status is zero for success and nonzero for
failure. Help is readable text; `--version` is JSON. All operations return the
request's `id` in a persistent session.

## A complete edit

Save this as `edits.json`, substituting your media path:

```json
[
  {"op":"media.import","args":{"paths":["/media/interview.mp4"]}},
  {"op":"editor.setLibraryRange","args":{"libraryIndex":0,"in":25,"out":124}},
  {"op":"editor.appendClipAt","args":{"libraryIndex":0}},
  {"op":"editor.setClipColor","args":{"timelineIndex":0,"brightness":0.05,"contrast":1.0,"saturation":0.95}},
  {"op":"editor.setMasterMix","args":{"key":"volume","value":-3}},
  {"op":"marker.set","args":{"frame":50,"text":"Review this shot"}},
  {"op":"editor.duplicateSequence","args":{"index":0}},
  {"op":"editor.renameSequence","args":{"index":1,"name":"Alternate cut"}},
  {"op":"editor.switchSequence","args":{"index":0}}
]
```

`brightness` is an offset with neutral 0 (−1…1); `contrast` and `saturation` are
multipliers with neutral 1. `marker.set` takes an optional `type` of `standard`
(the default), `chapter`, `todo` or `done`; a marker of any other type, as
older versions wrote, opens as `standard`. `editor.setLibraryRange` returns the
range actually applied and says so in `messages` when a point was limited to the
clip's recorded picture count. Library ranges and source markers use recorded
source frames, including after `setClipAttributes` changes the interpretation
rate. Placement converts both range boundaries and markers to timeline frames.
For example, source frames 10–29 at 50 fps occupy 10 frames in a 25 fps timeline.
Duration is quantized to whole timeline frames; inspect the placed clip's `in`,
`out` and `frames` when rates do not align exactly. Marked clips also retain a
`sourceOrigin` in physical source seconds to preserve the selected first picture.
Keep this field when transporting entries; zero is meaningful.
`editor.libraryPlacementRange` (`libraryIndex`) previews that conversion without
editing: `in`, `out`, `frames` and `duration` are timeline-frame values;
`sourceIn`, `sourceOut` and `sourceFrames` remain recorded-picture values.
It also reports `sourceFps`, `effectiveFps` and `projectFps`. The Edit browser
uses this same query for its drag preview.

Clipboard entries returned by inspection include `timelineFps`. Pass complete
entries to `editor.pasteClip` or `editor.pasteClips` to preserve source timing,
attributes and, for groups, linked sound. Copying ordinary media or speed ramps
to a different timeline rate converts durations, source anchors and animation
keys to that clock. Group paste is one undo step; a refused clip rolls back the
whole paste. Older external entries without clock metadata use destination-frame
units. Cross-rate multicam/transition copies, nested-clip pastes and speed maps that
cannot retain every segment at the destination rate are currently refused.

An apply batch stops at the first failure and publishes the project only after
all operations succeed. With `--sequence`, the selected timeline is the one
active when the project is saved. The input is preserved by default: choose `--output` or
explicitly choose `--in-place`. Project open/new/save operations are excluded
inside a batch. Use separate commands or `serve` for those operations.

This protects the **project file**. External effects explicitly requested by a
batch—rendered files, archives, recordings, copied media and preset changes—are
not rolled back if a later operation fails. A failed batch reports its failing
operation index and the results already obtained.

## Discovering the full operation surface

`ops` lists every exposed operation, its named arguments, valid overloads and
return type. Use argument names exactly as listed. Default C++ arguments appear
as shorter overloads. Numeric colour and audio parameters carry a `range` with
`min`, `max` and the `neutral` value that leaves the clip unchanged (the mixer's
`value` gives one per key, such as `volume` and `pan`). A value outside its
range is an `invalid_argument` error naming the range; nothing is clamped
silently. Unknown fields, wrong
primitive types and invalid common clip addresses are errors. Editing operations
can validly make no change (for example, setting an existing value); inspect the
resulting state to verify intent.

A reply's `ok` reflects each operation's own outcome. Operations that start
background work or create something (media tasks such as CSV export/import,
relinking and proxy attachment, presets, stills, bins) return its ID and fail
when it is refused. A media task that finishes within the request's wait
decides its reply: a failed or cancelled task fails the request with the reasons
from its report, such as CSV rows that matched nothing or a relink replacement
of a different length, and a partial one lists them in `messages`. With
`wait:false`, or past the timeout, `ok` means only that the task was accepted;
read its outcome with `mediaTasks.record` and the returned ID. Queue operations
on a job ID that is not in the queue fail with `not_found`.
Operations that return a problem description fail with that text. Informational
results, such as Auto Balance or a pasted grade, appear in `messages`. `status`
holds the status line set by that request, or is empty when it set none.

| Object / operation | Available work |
| --- | --- |
| `project.new`, `project.open`, `project.save` | Project lifecycle, explicit saves, conflict detection |
| `timeline.create`, `editor.*Sequence*` | Independent formats, rename, duplicate, switch, nest, close |
| `media.import`, `media.*`, `editor.*Media*` | Import, bins/saved searches, metadata, ranges, relinking, proxies |
| `editor.*Clip*`, `editor.*Retime*`, `editor.*Title*` | Placement, insert/overwrite, trims, ripple, blade, transforms, keyframes, titles, transitions, multicam and nested clips |
| `editor.createMulticam` | A multicam clip from library rows (`libraryIndices`, `method`: `audio`, `timecode`, `markers`, `start`). Optional `soundFrom`: the row among them its sound comes from under every angle (a sound recording chosen so is lined up but is no angle); left out, the sound follows the picture |
| `editor.setMulticamSound` | The sound source of a placed multicam clip, chosen after it is made (`track`, `index`: any piece of it; `source`: `-1` the sound follows the picture, `0`… that angle's own sound, after the angles a recording lined up with the clip that is no angle). Every piece of that placed clip changes, across its angle cuts, as one undo step; the picture cuts stay, and the library item is not changed |
| `editor.setLibraryMulticamSound` | The same choice on the multicam clip in the library (`libraryIndex`, `source`), for placements from then on; clips already placed keep their sound |
| `editor.collapseMulticam` | Collapse Multicam on the timeline at `index` of `sequences`, open or not: every multicam piece on it becomes an ordinary clip of the angle it shows, with the cuts, the pictures and the sound as they were, as one undo step. A sound that is not its picture's own (a clip's own sound source, a sound kept on another angle) is left as sound clips of their own, unlinked. Fails, changing nothing, when the timeline has no multicam clip or a track in the way is locked. `editor.multicamPiecesIn` (`index`) says how many pieces there are to collapse |
| `editor.bladeAllAtPlayhead`, `editor.trimToPlayhead` | One-step all-track blade; trim a clip's explicit start/end to the timeline playhead |
| `editor.rollEditOn` | Roll a touching pair on any track; preserve linked sound offsets, duration and one-step undo |
| `editor.setGrade`, grade/version/LUT/window operations | All supported grade data, nodes, masks, tracking, versions, shared grades, LUTs and presets |
| `editor.editVideoEffect` | Add, adjust, bypass, reset, reorder or remove a clip's video effects |
| `editor.applyMotionPreset` | Pan/zoom or title entrance/exit animation as one undoable edit |
| `speed.inspect`, `speed.edit`, `speed.apply` | Inspect and edit speed segments, ramps, reverse, replay and holds |
| `editor.setTrackMix`, `editor.setMasterMix`, `loudness.*` | Mixer routing, sends, effect chains, automation, analysis and normalisation |
| `editor.startRecording`, `editor.stopRecording`, ADR operations | Physical audio capture and recording management in a persistent session |
| `editor.captionStyleSettings`, `editor.setCaptionStyle`, `editor.generateCaptions`, `editor.regenerateCaptions` | The timeline's one caption style (font, size, colour, box, outline, `place` top/centre/bottom, `margin` and `width` in percent) set on every caption in one undo step (`everyCaption: true` also restyles captions with their own look), and the lengths Generate Captions keeps to (`maxChars`, `maxLines`, `minSeconds`) |
| `editor.trackCaptionStyle`, `editor.setTrackCaptionStyle` | One caption track's style (`track`; `-1` = the first caption track to read, every caption track to set, each keeping the rest of its own), with the fuller look: `shadow`, `shadowColor` (`#rrggbb` or `#aarrggbb`), `shadowOffset` (-50 to 50 px), `shadowSoftness` (0-50 px), `outlineColor`, `outlineWidth` (0-20 px, 2 = the usual edge), `boxOpacity` (0-100, the alpha of `bg`), `boxPadding` (-1 automatic, or 0-200 px), `boxRadius` (0-200 px); pixels are the timeline frame's. One undo step |
| `editor.captionLook`, `editor.setCaptionLook`, `editor.useTrackCaptionStyle`, `editor.setCaptionSpeaker` | One caption (`connectedIndex`): its whole look with `x`/`y` (offset from where the style places it, -1 to 1 of the frame, the clip's `rectX`/`rectY`), `speaker`, `track` and `ownStyle`; set style keys, `x`, `y` on it alone (it then keeps its own look, entry key `ownStyle`); `on: true` puts it back in its track's style and place, `false` keeps its look out of later track restyles; a speaker for `connectedIndices` (`""` = none). Each one undo step |
| `editor.captionTrackLanguage`, `editor.setCaptionTrackLanguage`, `editor.captionLanguages`, `editor.duplicateCaptionTrack` | A caption track's language: `code` (BCP 47 such as `en`, `fr`, `pt-BR`; `und` = not set) and `name` (empty takes the known name), kept as the track's mix `language`; the known languages; a copy of a caption track's captions and style in another language, named "Captions (French)", not burnt in. Each one undo step |
| `editor.captionList` | Every caption of the timeline in time order (`filter`: `track`, `language` code, `speaker` (`""` = none), `search`, `problems: true`; `{}` = all): `index` (flat connected), `track`, `start`, `end` (the frame after its last), `frames`, `seconds`, `language` `{code, shown}`, `speaker`, `colour`, `text`, `problems` `[{kind, reason}]` (kinds `speed`, `line`, `lines`, `short`, `long`, `gap`, `overlap`) |
| `editor.captionRules`, `editor.setCaptionRules` | The caption checks' limits, kept with the timeline (project key `captions.rules`, only those changed): `charsPerSecond` (17), `lineLetters` (42), `lines` (2), `shortestSeconds` (0.83), `longestSeconds` (7), `gapFrames` (2; a gap of none is fine), `noOverlap` (true); and `twoSpeakers` (false: Generate Captions names the speaker of each channel of a stereo clip, `leftSpeaker`/`rightSpeaker`, with whisper's `--diarize`; it needs a microphone per person, each on its own channel). Set keys given; one undo step. The Export page warns of what the checks find when captions go out (`subtitles` not `none`) |
| `editor.captionSpeakers`, `editor.setCaptionSpeakerColour`, `editor.renameCaptionSpeaker`, `editor.captionSpeakerColours` | The timeline's speakers `[{name, colour, captions}]` (project key `captions.speakers`, and any name a caption has); add one or set its colour (`#rrggbb`, `""` = the next free); rename on every caption (`newName: ""` removes); the colours offered. `editor.setCaptionSpeaker` adds a new name to the list in the same step. Each one undo step |
| `editor.setCaptionText`, `editor.replaceCaptionText` | A caption's words (`\n` between lines); every `find` replaced in `connectedIndices` (`[]` = every caption; `matchCase`), returning how many captions changed. One undo step |
| `editor.snapCaptionsToCuts`, `editor.extendCaptionsToNext`, `editor.closeCaptionGaps`, `editor.shiftCaptions` | Caption timing on `connectedIndices` (`[]` = every caption): edges onto a picture cut within `frames`; each to the next caption on its track less `gapFrames`; gaps under `frames` closed; moved by `frames` (negative = earlier). Return how many will move (-1 refused); a caption that would cover or pass another stays. Each one undo step |
| Caption and timeline interchange operations | Caption files, OTIO (`otio`), EDL (`edl`), FCPXML (`fcpxml`), legacy XML (`xmeml`, written as `.xml`) and optional AAF (`aaf`) |
| `render`, `render.enqueue`, `queue.*`, `presets.*` | Export settings, stems/splits, hardware choice, immutable job snapshots, pause/resume/cancel and presets |
| `editor.exportCaptionFiles`, `editor.exportCaptions`, `editor.importCaptions`, `editor.captionTrackCues`, `editor.captionFormats`, `editor.captionExportTracks` | Caption files in `format` `srt` (SubRip), `vtt` (WebVTT, with its `Language:` and the track style's top/centre place and left/right alignment), `itt` (timed-text XML for video stores: `xml:lang`, SMPTE frame times at the timeline's rate, `dropNTSC` at 29.97, top/bottom regions) or `scc` (CEA-608 pop-on written by OmaEdit: up to 4 rows of 32 letters, lines wrapped at spaces, programme timecode at the timeline's rate, drop-frame `;` at 29.97 and 59.94; a caption that cannot fit, or a character 608 has no code for, refuses the export and names the caption). `track: -1` writes every caption track beside `url`, named by its language (`film.en.srt`, `film.fr.srt`; `und` when not set; just `url` when there is one caption track with no language); a track index writes that track to `url`. Returns the paths written. `exportCaptions` takes the format from the name's suffix. `importCaptions` reads all four (and `.ttml`, `.dfxp`, `.xml` timed text; SCC pop-on, paint-on and roll-up of the first channel); a language in the file (`xml:lang`, WebVTT `Language:`) or its name (`film.fr.srt`) puts the captions on a caption track of that language, made "Captions (French)" when there is none. `captionTrackCues` lists each caption track's `track`, `code`, `name`, `place`, `align` and `cues` |
| Render `spec`: `subtitles`, `sidecarFormat` | `subtitles: "embed"` puts every caption track into the file as a stream of its own, tagged with its language as ISO 639-2 (`eng`, `fra` in MP4/MOV, `fre` in Matroska/WebM; one track with no language is `eng`, as before) and named by the language (title; in MP4/MOV also the track's handler): `mov_text` in MP4/MOV, SubRip in Matroska, WebVTT in WebM. `subtitles: "sidecar"` writes a file for each caption track beside the delivery in `sidecarFormat` (`srt` default, `vtt`, `itt`, `scc`), named as `exportCaptionFiles` names them; an SCC file's timecode is the delivered file's |
| `editor.archiveProject` | Portable archive, copied media and optional safe trimming |
| `frame`, `stills.import`, `stills.export`, `stills.*`, `color.manage` | PNG/EXR frame output and the grade stills gallery |
| `sounds.*`, `mediaTasks.*`, `tasks.*` | Sound collections, background media jobs, status and cancellation |
| `hardware.*`, `transcriber.*`, `luts.*`, `effects.*`, `volumes.*`, `meters.*` | Encoder, decoder and graphics processor choices (`hardware.engineGpus`, `hardware.setEngineGpu`: the editor's choice, never changed for them), speech-to-text settings, the LUT list, the effects shelf, drives and free space, playback meters |
| `get`, `set` | Read/write exposed properties, including input/output device and processing preferences |
| `editor.saveStorageSettings` | Storage folders and limits from the next launch, as `editor.storageSettings` lists them; `keptMedia` is where Render in Place files and stabilisation analyses are kept (empty for the default), and they are moved there at that launch |
| `inspect`, `defaults`, `ops`, `wait` | State, data templates, operation discovery and completion |

Use `editor.seek` to position the timeline playhead before these editing commands.
`editor.trimToPlayhead` takes `track`, `index` and `end` (`false` for the start,
`true` for the end), keeps the playhead picture and follows `editor.rippleTrim`.
It requires the clip and its linked tracks to be unlocked. Blade All cuts unlocked
picture/sound tracks as one undo step, keeps linked groups together, and skips
gaps, transitions, edit points and caption tracks. Playback visibility, mute and
sync lock do not exclude tracks. Both operations require the timeline viewer.

`editor.rollEditOn({track,index,delta})` addresses the outgoing clip at the cut.
A positive delta moves the cut later, a negative delta earlier. Picture edits
move linked sound edges by the same delta; sound-track edits remain independent.
Locked mates, insufficient handles, gaps, transitions or a collision refuse the
whole roll and report the reason. `editor.rollEdit({timelineIndex,delta})` is the
primary-track equivalent. Duration and downstream positions remain unchanged.

Video effect definitions are in `defaults.videoEffects` (or
`get` with `property: "videoEffectCatalogue"`): each definition includes its ID,
availability, parameter keys, types, defaults and allowed ranges. For example:

```json
{"op":"editor.editVideoEffect","args":{"track":0,"index":0,"action":"add","uid":"","values":{"id":"gaussian-blur","params":{"av.sigma":7}}}}
```

Inspect the clip's `videoEffects` list to obtain the new instance's `uid`.
Use `action: "update"` with `values: {"params": {...}}` to change specific
parameters, or `values: {"bypass": true}` to bypass it. `reset` restores defaults,
`remove` deletes that instance, and `move` takes `values: {"direction": -1}`
(earlier) or `1` (later). Other actions take an empty `values` object. All changes
are undoable. Invalid values, stale instance IDs, locked tracks and audio-only
targets are refused. Video effects currently require standard colour rendering;
active effects in a managed linear timeline must be bypassed before export.

`editor.applyMotionPreset` takes `track`, `index`, `preset` and `options`:

```json
{"op":"editor.applyMotionPreset","args":{"track":0,"index":0,"preset":"zoom-in","options":{"amount":15,"interpolation":"smooth"}}}
```

Pan/zoom IDs are `zoom-in`, `zoom-out`, `pan-left`, `pan-right`, `pan-up` and
`pan-down`. Their options are `amount` (1–100%, default 15) and `interpolation`
(`smooth` or `linear`, default smooth). They replace position/size keys across
the clip using its saved static framing. Title-only IDs are `title-fade`,
`title-slide-left`, `title-slide-right`, `title-rise` and `title-scale`; they take
`duration` (seconds per entrance/exit, 0.04–10, default 0.5), shortened to fit.
Title presets also replace opacity keys. Pan/zoom needs at least two frames;
title motion needs three. Locked tracks, audio and effect layers are refused.
Inspect `editKeys` and `opacityKf` afterwards; `editor.setEditProperties` can
refine the resulting keys.

Opacity uses `opacityKf: [{"frame":0,"alpha":0,"interpolation":"smooth"},
{"frame":49,"alpha":1}]` in an `editor.setEditProperties` patch. `alpha` is 0–1;
`interpolation` is `linear` (the default), `hold` or `smooth`, and describes the
segment leaving that key. Frame positions are clip-relative integers. Trimming
can leave control points outside the visible range; retain those when editing
an existing curve. Use `{"opacity":1,"opacityKf":[]}` to reset it. Title motion
presets use smooth opacity as well as smooth transform motion.

Use `speed.inspect` with integer `track` and `index` to read a media clip's
visible speed draft. It returns `definition`, segment rows, duration in `frames`,
and the first and final source boundaries. `speed.edit` takes the same address,
an `action`, an optional zero-based `segment` (default 0), and `options`:

```json
{"op":"speed.edit","args":{"track":0,"index":0,"action":"split","options":{"frame":50}}}
{"op":"speed.edit","args":{"track":0,"index":0,"action":"set","segment":0,"options":{"speed":0.5,"endSpeed":1.5,"easing":"smooth"}}}
```

Rates are multipliers: `1` is normal, negative reverses, and `0` holds.
`set` accepts `speed`, `endSpeed`, `frames`, `easing` (`linear` or `smooth`),
and `keepSource` (default true). With a rate/easing change and no explicit
duration, `keepSource` adjusts the duration to keep the current source range as
closely as whole frames allow. `split` takes a clip-relative `frame`; `join`
joins `segment` to its predecessor at a continuous source boundary. `hold`
inserts `frames` held frames at the clip-relative `frame`. Whole-clip actions
`reverse`, `replay` (play twice), `rewind` (play, reverse, play) and `normal`
take empty options. `normal` clears the mapping and plays its source range once
at 1×. Each successful call is one undoable edit and updates linked audio.

For several changes in one edit, start from the `definition` returned by
`speed.inspect`, modify it, and pass it to `speed.apply` with `track` and `index`.
Definitions use `version: 1`, `sourceIn`, `sourceLength`, and `segments` containing
`frames`, `speed`, `endSpeed`, `easing`, `curveStart` and `curveEnd`. Source
coordinates use frame boundaries at the timeline rate, after source frame-rate
interpretation. Optional segment `source` values represent jumps; `holdBefore`
retains the picture immediately before a boundary when holding reverse footage.
Keep the inspected source length and curve phases unless deliberately changing
them. `normal: true` is only accepted for a canonical forward 1× definition.
Invalid settings, stale targets and locked tracks are refused without an edit.

The `speed` group in `editor.pasteAttributes` and `editor.savePreset` carries a
reusable `speedPattern`. It fits the destination's current source range, scaling
segment durations and adjusting rates slightly for whole-frame rounding. Saved
patterns contain no media paths. Set `editor.setFrameBlend` with `track`, `index`
and `mode`: 0 preserves original sampling, 1 blends frames, 2 uses optical flow,
3 selects Floor and 4 selects Nearest frame. Floor and Nearest select recorded
pictures in ordinary, interpreted and mapped clips, including colour-managed
preview and export. New media placements default to Floor; saved clips without
a mode retain Original when reopened or pasted.
Mapped clips support live Frame blending and Optical flow; constant-rate blend
and flow retain their separate bake path.
Optical flow switches at scene cuts and blends pictures when motion is unreliable.
Invalid modes, missing clips and locked tracks
return an error without saving changes; selecting the current mode succeeds.
Use native projects; render in place before timeline export. XML timeline
import can preserve linear time maps with holds, reverse and supported sampling
modes. Unsupported curves or timing combinations return an error.
Holds and absolute rates below 0.1× or above 10× are silent.
`editor.setClipAttributes` validates a source frame-rate change before committing
any library attributes, and records one project-wide undo step. By default existing
edits keep their trim-frame numbers and durations. If a shorter source cannot retain
an existing trim, the entire operation is refused without changing the library,
timelines or undo history. Shorten those trims or enable `preserveSelectedFootage`
to adjust durations. A rate that shortens the source
beyond an existing speed map is refused, including maps in stored timelines and
undo/redo history. Reset or render those clips in place, then save and reopen
before changing that rate.
Include `preserveSelectedFootage: true` in the attributes
map to retain selected source footage and adjust edit durations and ripple positions.
The operation prepares all timelines before publishing their source settings;
undo restores the library attributes and affected timelines together. Affected
nested sequences, nested clips and multicam clips currently report a support limit. Linked clips
with independent trims retain their selected footage and synchronisation when all
members have the same playback-duration change and their scaled offsets fall on
whole timeline frames. This also applies to linked sound-only groups. Different
source-rate changes within a group, fractional linked offsets and groups spanning
multiple primary-track clips are refused before publishing any changes. Independently
trimmed speed maps also require their selected segment durations to scale to whole
frames, so rounding cannot introduce different timing in picture and sound.
The desktop dialog applies this asynchronously; CLI scripts can use the synchronous
`editor.setClipAttributes` operation.

The `editor` operations are the existing application methods, not a second edit
implementation. Dialogs, opening external desktop programs, window layout and
other GUI-only actions are excluded. Path-taking operations replace file choosers.
The CLI cannot supply features absent from the engine or missing optional
providers; inspect plug-in availability and returned errors.

An OpenEXR frame holds scene-linear light, which only a colour-managed grade
produces. `color.manage` with a clip's `track` and `index` turns on OpenColorIO
colour management for that clip with the bundled configuration, as the Colour
Management dialog does (`enabled:false` turns it off). `defaults` shows the
bundled settings as `colourManagement`.

`inspect` includes media, active timeline clips, connected tracks, markers,
grades, mixer state and related objects. Its `timeline` list holds the primary V1
track only; clips on every other track (A1, V2 and up, audio tracks) are
in `connected`, each with its `track` and `index`. Its `sequences` list every timeline
with its `index`, `id`, `name`, whether it is `active`, its `settings` and its
`entries`.
`--sequence NAME_OR_ID` selects a timeline for inspect, apply, call, frame or export without changing the source file. The session equivalent is `timeline.select` with a `sequence` string.

`editor.undo` and `editor.redo` follow one chronological
history across timelines. Selecting another timeline retains that history; undo
selects the timeline that owns the edit. The `undoText` and `redoText` properties
include its name when the next edit belongs elsewhere. Creating, importing,
duplicating, renaming and closing timelines also record undo steps, retaining
their stable IDs when restored. Source-attribute changes restore all affected
timelines and library clocks together. Later project preferences are retained.
History lasts for the current editing session, up to 100 steps; opening a project
starts a fresh history. Use a `serve` session or one `apply` operation list to undo
earlier operations in the same process.

`image.save` accepts a `source` image URL/key returned by inspection (for example a library thumbnail), or `program`, `source`, `matte`, `before`, `trim.incoming` or `trim.outgoing` and a PNG `path`. These images belong to the current session; save them before closing it.

`get` can retrieve one property without the full report:

```json
{"id":1,"op":"get","args":{"target":"editor","property":"projectSettings"}}
{"id":2,"op":"get","args":{"target":"presets","property":"presets"}}
{"id":3,"op":"set","args":{"property":"rippleTrim","value":true}}
```

Positions/durations use **frames at the active timeline rate**. In/out points
are inclusive; `[25,49]` contains 25 frames. Indices are zero based. Track/index
identifies a clip within its track; `timelineIndex` addresses the primary
timeline and `connectedIndex` addresses the connected list. Structural edits
can change indices: inspect again before subsequent edits. Sequence IDs remain
available for resolving a sequence index through `editor.sequenceIndexForId`.
Nested grade and effect maps use the same shapes returned by `defaults`,
`inspect`, `editor.defaultGradeNode`, and the audio effect catalogue. Keep unknown
existing fields when modifying those maps.

## Persistent sessions and background work

Send one JSON request per line to `omaedit serve`. Replies have this shape:

```json
{"id":1,"ok":true,"op":"get","result":{},"dirty":false,"busy":false,"messages":[]}
```

Failures contain `ok:false` and `error:{code,message}`. A bad request leaves the
session usable. Save explicitly before closing stdin; closing with unsaved edits
reports `unsaved_changes` and a nonzero exit code. Active recording, unfinished work and renders the session queued but never ran are also reported on session exit as `unfinished_jobs`; stop recording and finish, run or cancel jobs before closing stdin. `project.open` and `project.new`
refuse to discard edited work unless their arguments include `discard:true`.

Operations normally wait for their engine work and asynchronous replies.
For precise sound edits, obtain `editor.audioEditTarget` with the clip's `track`
and `index`, then pass the returned target to `editor.setAudioTiming` or
`editor.splitAudio` in the same persistent session. Targets expire after a project
reload, timeline change or a change to that cut; capture a fresh target before
the next edit. A precise cut's ID follows index and track changes.
`setAudioTiming` takes `timing:{start,in,duration}`; each time is rational seconds
as `{ticks:"1",rate:48000}` for one sample at 48 kHz. `splitAudio` takes a
timeline `position` in the same format. Convert an ordinary audio cut with
`setAudioTiming` before splitting it between video frames. Unsupported processing
and invalid or stale targets return an error without applying the requested edit.

For a long render or background operation, set **`wait:false`** so the session
can accept cancellation or status requests:

```json
{"id":1,"op":"render","wait":false,"args":{"path":"/exports/film.mp4","spec":{"vcodec":"h264"}}}
{"id":2,"op":"get","args":{"target":"queue","property":"jobs"}}
{"id":3,"op":"queue.pauseAll"}
{"id":4,"op":"queue.resumeAll","wait":false}
{"id":5,"op":"wait"}
{"id":6,"op":"project.save"}
```

Pause/resume/cancel operations return without waiting for the whole render by
default. Use job IDs from results, not their row positions. A paused job remains
visible and can be resumed. `render.enqueue` creates frozen jobs without starting
them; `queue.renderAll` starts every queued job. `render` (and the one-shot
`export`) runs exactly the jobs it creates, listed in its result's `jobIds`.
Older jobs are never run implicitly and never block it: jobs queued earlier in the
session, and jobs from an ended session (including a render cut off when its
session closed, which comes back queued), wait in the CLI queue until
`queue.renderAll`, `queue.cancelJob` or `queue.removeJob`. A one-shot
`call PROJECT render.enqueue` deliberately leaves its job queued this way for a
later command.

Every rendered file says where on the timeline's clock it starts: the
timeline's starting timecode (`startFrame`), or for a `rangeIn` the timecode of
that frame. A QuickTime or MP4 file has a timecode track, a Matroska or WebM
file a `timecode` tag, and a WAV (stems too) a broadcast-wave time reference
(samples since midnight). Read it back with
`ffprobe -show_entries stream_tags=timecode:format_tags=timecode,time_reference FILE`.
Nothing in a render spec turns it off or changes it. Timeline files (`edl`,
`fcpxml`, `xmeml`) are written non-drop at every rate, as the editor counts;
`editor.importTimeline` reads an EDL, which names no frame rate, at the open
timeline's rate, or at 25 when no timeline with a rate is open.

`wait` accepts `milliseconds` to keep a session alive for a timed capture or
playback interval, then waits for other work. Recording/playback themselves are
continuous and do not block every request. Physical capture needs an available
input device; third-party tools/providers retain their normal prerequisites.
Sound-library scanning is stopped at CLI startup to avoid an unsolicited scan
of the user's Music folder; request `sounds.rescan` explicitly when needed.

`--timeout` sets the per-operation wait limit in milliseconds, from 100 to
86400000 (default 600000).
A timeout does not claim an operation was cancelled. In a persistent session,
inspect jobs and cancel or wait as appropriate. Set `progress:true` on a session
request, or `--progress` on a command, to receive render progress JSON on stderr.

## Files, conflicts and desktop isolation

- Existing output files require `--overwrite` or `overwrite:true` on the built-in
  save/render/frame/still-export operations; otherwise they report
  `output_exists`. In-place saving of the loaded project is explicit
  authorization to replace that project revision.
- Project saves stage complete output, keep the previous version in the backup
  ring (`get` `projectBackups` lists it), and publish with atomic replacement. A
  changed input/destination is refused. CLI saves also take a per-project lock:
  a second CLI process saving the same project at that moment is refused with
  `project_locked` rather than queued; retry it.
- The CLI edits an independent project session. It does **not** control an already
  open desktop window. Save/close the desktop project before editing that same
  file from the CLI; otherwise reopen it to see agent changes. Fingerprint checks
  detect changes since the CLI opened it; the desktop does not share the CLI lock.
- CLI preferences, queues, caches and recovery use separate storage under
  `$XDG_DATA_HOME/omaedit/cli` (normally `~/.local/share/omaedit/cli`). Override with
  `--state-dir PATH` or `OMAEDIT_CLI_STATE_DIR`. Desktop queues and recovery copies
  are untouched. Media/project files explicitly supplied to commands are shared.
  So are your fonts. Transcription uses the speech model that comes with the
  package (`/usr/share/omaedit/whisper/ggml-base.bin`, multilingual, the
  language found by itself) unless the CLI's own `model` preference (target
  `transcriber`) names another file.
- Raw engine operations that write external files retain their application
  behaviour. Read the relevant operation before supplying a destination; the
  built-in overwrite guards do not wrap arbitrary `editor.*` export operations.
- A one-shot mutating `call` requires `--output` or `--in-place` to retain its edits.
  For a sequence of edits, use `apply` or `serve`.

## RAW, 360° and APV media

`editor.setMediaRaw` takes `indexes`, `exposure` (−6 to +6 stops), `red` and
`blue` (0.25–4, relative to the camera white balance). Set 0/1/1 for As Shot.
The operation applies to ProRes RAW sources and switches changed sources to
original playback. RAW input controls are saved with library/source metadata.

`editor.startMediaTask` supports `kind: "reframe360"`. Options are `destination`,
`pattern` (default `{name}_reframed`), `projection` (`e` or `dfisheye`), `width`,
`height` (default 1920×1080), `yaw`, `pitch`, `roll` (degrees), `fov` (horizontal,
10–170 degrees), and `acceleration` (`auto` or `software`). Output is an imported
ProRes 422 HQ copy with PCM audio. A fixed view is baked into the new clip.

Proxy tasks accept `acceleration: "auto"` (default) or `"software"` for ProRes.
Render specs accept `vcodec: "apv"` in MOV, MP4 or MKV. `vprofile` is `422-10`
(default), `422-12`, `444-10`, `444-12`, `4444-10` or `4444-12`; the 4444
profiles carry alpha and need `alpha: true` to keep it. `quality` (0–51, default
18) sets the encoder QP as quality + 12; `rateMode` `cbr` or `vbr` with `bitrate`
(kb/s) uses the encoder's rate control instead. APV is progressive only.
`level` passes an APV level (`1`…`7.1`) through. Colour space and data levels are
written into the APV bitstream, so every container reports them. The OpenAPV
encoder is found through `OMAEDIT_APV_ENCODER`, then `lib/omaedit` beside the
binary, a `bin/setup-openapv` build, then `PATH`; without it, APV renders fail
validation. APV media imports through FFmpeg 9's decoder.
Render specs accept `hw: "vulkan"` for progressive ProRes Proxy/LT/422/HQ;
`softwareFallback` retains its existing meaning. RAW decoding requires FFmpeg
9 camera colour metadata; RAW encoding is not offered.
