---
name: omaedit
description: >
  Create, inspect, edit and export video projects through OmaEdit's headless CLI.
  Use for importing footage, arranging or duplicating timelines, trimming clips,
  grading colour, mixing audio, adding titles or captions, archiving projects and
  rendering video in OmaEdit or an existing .omaedit project. Not for developing
  the app's source code.
---

# Editing with OmaEdit

Use `omaedit` directly; no window or display server is needed. Commands return
JSON on stdout and a nonzero exit code on failure. Diagnostics and optional
progress go to stderr. The CLI uses the same editing engine as the desktop app.

## Discover before editing

- `omaedit guide` returns the complete installed manual in its JSON `result`.
  Read it for command syntax, file operations and asynchronous workflows.
- `omaedit ops` lists the installed operations, named arguments, overloads and
  properties. Filter with `omaedit ops Grade` (a positional, case-insensitive
  filter). Look up methods and argument names rather than guessing them.
  Numeric colour and audio parameters list a `range` with their `neutral`
  value: `setClipColor` brightness is an offset (neutral 0), contrast and
  saturation are multipliers (neutral 1).
- `omaedit defaults` provides timeline, grade, render and audio-effect data shapes.
  Its `videoEffects` catalogue provides video effect IDs, availability and typed
  parameter ranges. `editor.editVideoEffect` edits an ordered per-clip stack;
  inspect `videoEffects` on the clip for the instance `uid` before updating,
  bypassing, resetting, moving or removing an effect. These effects currently
  require the standard colour pipeline; managed linear export refuses active
  video effects instead of omitting them.
- `editor.applyMotionPreset` applies pan/zoom (`zoom-in`, `zoom-out`, `pan-left`,
  `pan-right`, `pan-up`, `pan-down`) with `options: {"amount":15,"interpolation":"smooth"}`.
  Title clips also accept `title-fade`, `title-slide-left`, `title-slide-right`,
  `title-rise` and `title-scale` with `options: {"duration":0.5}` (seconds per
  entrance/exit). Address the clip by track/index. Presets replace position/size
  animation; title presets also replace opacity. Inspect the resulting keys.
  Opacity curves use `opacityKf: [{"frame":0,"alpha":0,"interpolation":"smooth"},
  {"frame":49,"alpha":1}]` with `editor.setEditProperties`. Easing can be `linear`,
  `hold` or `smooth`; keep out-of-range control points left by trims. Clear keys
  explicitly with `opacityKf: []` when setting a static opacity.
- Use `speed.inspect` with `track`/`index` before editing speed segments.
  `speed.edit` accepts `action`, optional `segment`, and `options`: `set` takes
  `speed`, `endSpeed` (multipliers, 1 = normal), `frames`, `easing` (`linear` or
  `smooth`) and `keepSource`; `split` takes a clip-relative `frame`; `hold` takes
  `frame` and added `frames`. Whole-clip `reverse`, `replay`, `rewind`, `normal`
  take empty options. `normal` clears the map. `speed.apply` applies an edited
  inspected `definition` in one undo step. Linked audio follows. The `speed`
  attribute/preset group fits a path-free `speedPattern` to the destination's
  source range. `editor.setFrameBlend` takes `track`, `index` and `mode`:
  0 retains original sampling, 1 blends frames, 2 uses optical flow, 3 is Floor,
  and 4 is Nearest frame. Floor/Nearest select recorded pictures for ordinary,
  interpreted and mapped clips in preview/export, including colour management.
  New media placements default to Floor; reopening or pasting old clips without
  a saved mode retains Original.
  Mapped Frame blending and Optical flow also work live; constant-rate blend
  and flow still use the separate bake path.
  Optical flow switches at scene cuts and blends when motion is unreliable.
  Use native storage; render
  in place before timeline export. XML import supports linear time maps;
  unsupported curves or timing combinations return an error. Holds and rates below 0.1× or above 10× in
  either direction are silent. Read `guide` for definition coordinates/options.
- `omaedit inspect project.omaedit` returns media, timelines, clip/track indices,
  markers, grades and mixer state. Inspect before addressing existing objects and
  after structural edits, which can change indices. Use sequence IDs or names
  with `--sequence` to select the intended timeline.

## A normal editing task

Create a project with an explicit first timeline and the requested format:

```sh
omaedit new edit.omaedit --timeline Main \
  --settings '{"custom":true,"width":1920,"height":1080,"fpsNum":25,"fpsDen":1}'
```

Use the user's requested format; the values above are an example. A project can
hold several timelines. `new` without `--timeline` creates a media-only project;
create a timeline before placing clips. `timeline.create` and
`editor.*Sequence*` operations manage timelines, including duplicates for alternate
cuts. Find their arguments in `ops`.

Save editing requests as a **JSON array**, with **named arguments**:

```json
[
  {"op":"media.import","args":{"paths":["/media/interview.mp4"]}},
  {"op":"editor.setLibraryRange","args":{"libraryIndex":0,"in":25,"out":124}},
  {"op":"editor.appendClipAt","args":{"libraryIndex":0}},
  {"op":"marker.set","args":{"frame":50,"text":"Review this shot"}}
]
```

The index in this example assumes a new empty project's first import. Inspect an
existing project's library to resolve the right index. `editor.appendClipAt`,
`insertClipAt`, `overwriteClipAt`, `overwriteClipBacktimed`, `dropClipAt` and
`replaceClip` accept `"takes":"picture"` or `"sound"` to place the clip's
picture or its sound alone (default both; sound alone lands on an audio track;
`dropClipAt` needs `audio` given before it).
`beats.map` with `libraryIndex` finds the beats of that clip's music and returns
`{found, bpm, beats}` (beats in seconds of the file's sound; `found:false` for
sound with no steady beat); the project keeps them as the clip's `beatMap`.
`beats.clear` removes it. `editor.clipBeats` (`track`, `index`) gives the beats a
timeline clip plays as frames from its start.

```sh
omaedit apply edit.omaedit --ops edits.json --output revised.omaedit
omaedit inspect revised.omaedit
omaedit frame revised.omaedit --frame 50 --output review.png
omaedit export revised.omaedit --output film.mp4 --progress
```

Options may come before or after the command word; a mistyped command is a
`usage` error. Unknown keys in settings or a render spec are rejected, and
`marker.set` types are `standard` (default), `chapter`, `todo` or `done`.
`apply --in-place` explicitly saves back to the source. A single operation can
use `call PROJECT OP --args JSON`, with `--output` or `--in-place` for edits.
For colour/audio/effect maps, start from inspected or default data and preserve
unrelated fields. Use the actual catalogue for grades, masks, mixer routing,
effects, captions, interchange and archive operations.

## Values and verification

- Positions and durations are frames at the active timeline's frame rate;
  in/out points are inclusive: `[25,49]` is 25 frames.
- Indices are zero based. `timelineIndex` addresses the main storyline
  (`timeline` lists V1 only); `connectedIndex` the connected list (every
  other track's clips, a new V2's included); `track` plus `index` addresses a clip
  within its track. Re-inspect after moves, deletions or timeline switches.
- Read `ok`, `error`, `messages`, `dirty` and `projectChanged` in responses. An
  accepted operation can make no change. Check the resulting state against the
  requested edit, and inspect exported frames/video where visual results matter.
- Numeric values outside the `range` listed in `ops` are refused. A media task
  (CSV import, relink, proxy) that fails within the wait fails the request;
  with `wait:false` check `mediaTasks.record`. `frame` to `.exr` needs
  `color.manage` on the clip first.
- `image.save` saves session images such as a matte or thumbnail. Use `serve`
  when generating an image and saving it in subsequent requests.

## Persistent work

`omaedit serve PROJECT` accepts one JSON request per line and echoes its `id`.
For long jobs, set `wait:false`, then inspect job status, cancel or send `wait`.
Use the guide for render queue and recording workflows. `render` and `export`
run only the jobs they create; older queued jobs wait for `queue.renderAll`.
Send `project.save` before closing stdin after editing; unsaved work is
reported as an error. Stop recording, and finish, run or cancel active or
queued work before ending the session.
A timeout reports unfinished work; it does not cancel it.

## File handling

- CLI sessions are independent of open editor windows. Save/close the desktop
  project before editing the same file from CLI, then reopen it to see changes.
- `apply` publishes the project only when all operations succeed. External
  exports, copied media, archives and preset changes requested in the batch are
  not rolled back if a later operation fails.
- Built-in saves, renders and image exports refuse existing output unless
  `--overwrite` (or `overwrite:true`) is explicit. Raw `editor.*` file operations
  retain their native overwrite behaviour; check the guide for the chosen path.
- Separate CLI settings, cache, queue and recovery storage leave desktop state
  independent. `--state-dir PATH` selects an isolated automation environment.
- Optional codecs/providers and physical recording retain their normal device
  requirements. Read returned availability and errors rather than assuming them.

## Installation

The skill ships with OmaEdit. `omaedit skill` reports its location and links;
`omaedit skill --link` registers it with this user's agents. Start a new agent
session to discover it. Links follow application updates. If `omaedit` is missing,
install OmaEdit on Arch/Omarchy with `./bin/install` from its repository; an
existing source build can be used as `./build/omaedit`.
