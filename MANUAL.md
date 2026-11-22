# OmaEdit user guide

The reference for everything in the editor. For installing and a first cut, see the
[README](README.md). Press **?** in the app for every key, and rest the pointer on
any control to see what it does.

## The editor at a glance

- **Library** (left): imported media with filmstrips. Click a clip to view
  it in the source viewer; mark an in/out range with `I` / `O`.
- **Viewer** (top): program monitor, or source + program side by side
  (`Ctrl+3`).
- **Timeline** (bottom): as many video and audio tracks as you need, with a
  header per track. Video tracks stack upwards from `V1`; audio tracks hang
  below it. Every sound sits on an audio track under the pictures: a video
  clip's sound, on V1 or on a track above, goes onto an audio track linked
  to it and follows it when the picture moves or is trimmed. Audio tracks
  also hold audio-only clips (music, voice-over).

### Project settings and preferences

File and folder choices open **inside OmaEdit**: import media, open/save projects,
choose export locations, locate LUTs and manage presets without launching a
separate file manager. The browser supports folder navigation, multiple-file
selection, hidden files, common folders and mounted drives, and a filename field
when saving. The search box filters file and folder names in the current folder
(case-insensitive, with no subfolder scan). **Ctrl+F** focuses search; its clear
button restores the list. The type menu above the list offers video, audio,
images and individual formats such as MP4, MOV and WAV for media imports. Search
and type filters work together, and changing either clears the old selection.
Use **Ctrl+L** to enter a folder path,
**Alt+Up** for its parent and **Alt+Left** to go back. **Ctrl-click** or
**Shift-click** selects multiple files; **Ctrl+A** selects all displayed files
when the file list has focus. Each operation remembers its last accepted folder.
Cancel or Escape leaves the operation
unchanged; saving over an existing file asks for confirmation.

**Keyboard controls:** **Tab / Shift+Tab** moves between controls with a visible
focus outline and scrolls the focused control into view. **F6 / Shift+F6** moves
between workspace panels; these bindings are editable in Keyboard Shortcuts.
Typing a number or timecode and pressing **Tab** keeps the change; **Escape**
cancels the draft ([Timecode](#timecode) says how one is typed). Arrow keys adjust mixer knobs and faders, **Shift** makes a
finer adjustment, **Home / End** selects a limit, and **Enter** resets. A focused
removable tag can be removed with **Delete**. Sidebar rows support **Space / Enter**.
Open dialogs retain keyboard focus. Arrow-key navigation scrolls long menus to
keep the selected action visible.

**Preferences ▸ General** also offers **High contrast** and **Reduce motion**.
They are saved between launches; the picture and grading surrounds stay neutral.
In Colour, focused layers support keyboard selection, movement and actions, and
curve points support arrow-key adjustments with finer steps using **Shift**.
Timecode and numeric displays use a monospaced fallback when needed.
Only English is currently included; the application can load reviewed Qt Linguist
catalogues for other languages.

**File ▸ New Project** (`Ctrl+N`) creates a named project containing your media
and timelines. It asks for the project's name first; when you choose **Create
Project** with unsaved changes in the current project, it then offers Save,
Discard or Cancel before replacing it. You can import and preview media before
creating a timeline; choose the project file location when you first save.

**File ▸ New Timeline**, the command palette, **+ New Timeline**, or a
right-click on empty space in the Edit page's media bin ▸ **New Timeline…**
opens a name and format dialog. Choose **Match first clip**, a standard size, or set
dimensions, frame rate, colour space, audio format and starting timecode (such
as 01:00:00:00; the ruler, viewer and exports count from it). A starting
timecode is typed as every timecode is (see [Timecode](#timecode)):
`09:59:50:00`, `9595000` or `095950.`; `10...` is ten hours and `10:00` ten
seconds. It stays the timecode that was typed when the frame rate is chosen
afterwards or comes from the first clip. The box opens with the start set in
**Preferences ▸ General ▸ New timelines start at**. An entry that cannot be
read (seventy seconds, a frame number the rate does not have, an hour past 23)
leaves the start as it was, with the field outlined and a line saying so, and
the box stays open. The standard
sizes are HD 720p and 1080p, QHD 1440p, UHD 4K and 8K, 2.39:1 widescreen in HD
and UHD, DCI 2K and 4K (full, flat 1.85:1 and scope 2.39:1), vertical 9:16 in
720p, HD and 4K, portrait 4:5 and square 1:1; Timeline Settings offers the
same list. Adding a clip before a
timeline exists opens this dialog and places the clip only after creation.
Cancel leaves the media in the library and the timeline uncreated.

Each project can hold multiple timelines with independent settings. Click a tab
to switch. **Duplicate Timeline** copies the current edit into another tab for an
alternate version; edits and copied grade groups are independent, while source
media is reused. Explicitly nested timeline clips still refer to their source
timeline. Names are checked for duplicates without case differences.

- **File ▸ Timeline Settings…** (`Ctrl+Alt+J`) changes the active timeline's
  dimensions, colour space, scaling, audio format and starting timecode.
  The starting timecode can be changed whether or not the timeline holds clips;
  the viewer, ruler, index, marker positions and the Inspector's "starts at"
  count from it at once.
  **Scaling** decides, automatically, how every clip of another size or shape
  takes the frame: **Fit** (the whole picture, with bars; the default),
  **Fill** (the whole frame, its edges cropped), **Stretch** (to the frame's
  corners) or **Original size** (pixel for pixel). Match first clip
  chooses its frame size and rate once. A timeline's frame rate is locked while
  it contains clips; other timelines can use different rates. Media import and
  proxy folder defaults remain project-wide. Saving, recovery and archives keep
  all timelines and their settings. Older projects inherit their existing format.
- **Preferences** (the gear in the title bar, `Ctrl+,`): density, autosave
  interval and saved-version retention, recent projects, the playback output
  device, hover preview and snapping,
  transcription command / model / language, the recording input, the
  default colour space for new projects and the LUT folder, the keyboard
  shortcut editor, and runtime hardware diagnostics. Hardware has a Graphics
  processor choice (Automatic, each GPU found, or CPU) which is yours alone and
  is never changed for you, on battery or otherwise; it takes effect on the next
  launch. Storage has a **Kept media** folder for Render in Place files and
  stabilisation analyses (with the application's data by default; a new folder
  is used from the next launch, when the files are moved to it), and **Remove
  Unused Kept Media…**, which asks before deleting the ones no project OmaEdit
  can check still uses. Hardware lists Vulkan GPUs,
  queues, memory, individual encoder tests and H.264 VA-API/NVDEC checks.
  **Refresh Hardware** reruns the probes; Copy Diagnostics keeps the report local. Unavailable options
  are identified in place.
- **Your desktop's accent**: selection, focus, drop targets and the other
  highlights take the accent colour of your current Omarchy theme, and follow
  it the moment you switch themes — no restart. Everything nearer the picture
  (the viewer surround, the panels, clip and meter colours) stays a fixed
  neutral on purpose: a surround that changes hue changes how you read the
  image and the grade in front of it. Off Omarchy, the app wears its own
  accent and looks exactly the same as it always has.

**Help ▸ About OmaEdit / Diagnostics** shows a selectable system report with the
actual Qt Quick renderer, timeline and viewer processing, memory and codec/device
results. **Preferences ▸ Hardware ▸ Viewer** offers **Auto — measured Vulkan**
(the default) and **CPU**. Auto compares supported OCIO viewer colour transforms
locally and uses Vulkan only when it consistently saves at least 20% and 0.5 ms
with matching pixels. Small previews, unsupported transforms and unavailable or
slower GPUs use the CPU; runtime GPU failures also fall back to the CPU.

Diagnostics reports the viewer's actual choice, device, reason and timings.
Copy System Information puts it on the clipboard; nothing is uploaded. Timeline
grading/compositing, snapshots and exports retain their CPU processing.
**Preferences ▸ Hardware ▸ Interface** selects Auto (prefer Vulkan), Vulkan with
software fallback, or Software. Changes apply after restarting. A bounded, isolated
render/readback check qualifies Vulkan before startup; failure retains software.
The viewer uses Qt Quick image nodes on either renderer.

**Preferences ▸ Hardware ▸ Decoder** offers **Auto — measured**, **Software**, **VA-API**
and **NVDEC**, with a device selector. Changes apply after restarting. Startup
checks compare actual pixels and three complete open/decode/download batches
against software. Auto requires a consistent 15%
benefit on the short H.264 sample; explicit hardware choices must still pass
pixel checks. Results cache for up to 24 hours and are invalidated by app/driver
changes. **Retest decoder on next launch** clears the cached check.

Hardware decoding covers eligible local timeline video and the native float
colour path. Unsupported streams and returned hardware errors use software;
interlaced timeline frames retain the existing CPU path. VA-API uses the selected
render node's default driver; NVIDIA uses NVDEC because its VA-API wrapper stalled
in seek testing. This changes no desktop driver settings. Audio and saved media
paths are preserved. Diagnostics count the frames decoded in hardware and the
ones replayed on the CPU.
The short startup sample does not predict performance for every format or size.
Advanced launch overrides are `OMAEDIT_VIDEO_DECODER=auto|software|vaapi|nvdec`
and `OMAEDIT_VIDEO_DECODER_DEVICE` (a VA-API render node or NVDEC index).
The reusable components compile into each app independently; no other suite app
or service is needed.

### Timecode

Timecode is hours, minutes, seconds and frames, `hh:mm:ss:ff`. Frames count to
the timeline's whole rate (24 at 23.976, 30 at 29.97, 60 at 59.94) without
dropping numbers, and the count starts again at `00:00:00:00` after
`23:59:59` and the last frame: ten seconds after `23:59:50:00` is midnight.

**Typing a timecode** is the same in every timecode field: the starting
timecode, a marker's position, In and Out, an ADR cue, the viewer's go-to.

| You type | It is read as |
|---|---|
| `10000000` | `10:00:00:00`: digits alone are read from the right, frames last |
| `1000` | `00:00:10:00`, ten seconds |
| `5` | five frames |
| `10...` | `10:00:00:00`: a full stop (or comma) at the end, or before another, is a pair of noughts |
| `230.` | `00:02:30:00` |
| `1.` | one second |
| `9:59:50:00`, `09.59.50.00`, `9;59;50;0` | `09:59:50:00`: between digits `:` `;` `.` and `,` separate the pairs, and a single digit after one is a pair (`1.2` is one second and two frames) |

Whatever you leave off on the left is filled in. In a **starting timecode** it
is nought: `10:00` is ten seconds. In a **position** (the viewer's go-to, a
marker, In and Out, an ADR cue) it is the hours, minutes and seconds the field
shows: with the playhead at `10:00:00:00`, `230.` goes to `10:02:30:00` and
`12` to frame 12 of that second. A full timecode always says exactly where.

An entry that is not a timecode (frames the rate does not have, sixty or more
seconds or minutes, an hour past 23, letters) changes nothing: the field is
outlined in the warning colour and says what it could not read, in the box's
own warning line where it has one and in a note beside the field elsewhere.
The note goes when you type again. Lengths (a clip's duration) are typed as
time, as before: `2.5`, `1:30`.

**Go to a timecode.** The timecode under the viewer is where you type one.
Click it, press `=` (Mark ▸ Go To ▸ Timecode…; the key can be changed in
Keyboard Shortcuts), or start typing on the number pad; **Enter** moves the
playhead there, **Esc** leaves it where it was. The timecode is the
timeline's own: on a timeline that starts at `09:59:50:00`, `10000000` goes
to the programme's first frame. A timecode before the start or past the end
goes to the nearer end, and the status line says so. With a source in the
viewer the same entry moves through the source, counted from its first frame.

**Move by an amount.** Start with `+` or `-` (the number pad's open the
entry by themselves): `+10` is ten frames on, `-5` five back. One or two
digits are frames, however many (`+30` at 25 frames a second is a second and
five frames); a longer amount is read as a timecode (`+100` is a second,
`+1612` sixteen seconds and twelve frames, `-3..` three minutes back). With
nothing selected the playhead moves. With clips selected they move by that
amount instead, exactly as Nudge moves them: into free space only, and not at
all on a locked track, where the entry says so and waits.

While the digits cut between multicam angles (the multicam viewer is open,
or the playhead is on a multicam clip) the number pad's `1` to `9` do that
too, as the main row's always do; `=` and the pad's `+`, `-` and `0` still
open the entry.

**The starting timecode** of a timeline is set in New Timeline and changed
in Timeline Settings at any time, with or without clips; nothing on the
timeline moves, and the viewer, ruler, index, markers and Inspector count
from it at once. **Preferences ▸ General ▸ New timelines start at** holds the
start each new timeline is offered (`00:00:00:00` until you set it; a UK
programme with a ten-second clock would use `09:59:50:00`). It is kept as a
timecode, so it is the same start at 24, 25 or any other rate.

**What files carry.** Every rendered file says where on the timeline's clock
it starts: the starting timecode, or for a range the timecode of its first
frame.

| File | Carries |
|---|---|
| QuickTime (`.mov`) and MP4 | A timecode track, and the same timecode on the picture stream |
| Matroska (`.mkv`) and WebM | A `timecode` tag |
| WAV (a sound-only render, and each stem) | A broadcast-wave time reference: the samples since midnight at its first sample |
| MP3, M4A, FLAC, image sequences | Nothing: these hold no timecode |

The Export page shows it as **Start Timecode in the File**. A file delivered
at another frame rate counts its frame number at its own rate (frame 12 at 25
is frame 24 at 50). A 50 or 59.94 file counts frames 00 to 49 or 00 to 59.
Timeline files (EDL and both XML forms) are written with the numbers on the
screen, non-drop at every rate; drop-frame files from elsewhere are still read
to the frame. An EDL names no frame rate, so it is read at the open timeline's
rate, or at 25 when no timeline with a rate is open, and the status line says
which.

### Closing projects and viewer controls

**File ▸ Close Project** (`Ctrl+Shift+W`) clears the current project and source
viewer while keeping the application open. Unsaved projects offer Save, Discard
and Cancel. A failed save or file chooser leaves the project open. Missing files
stay in **Open Recent**, marked **Missing**, with Locate and Remove from Recent.

Open **Viewer and Playback** from the application menu, command palette or bottom
**Viewer** button. All five workspaces share Fit/Fill, 25–200% presets, custom
1–800% zoom, pan, safe areas, guides/grid and transparency checkerboard. Pixel
aspect ratio is applied to the picture and editing overlays. These are viewing
preferences; they do not change the edit.

**Canvas colour** sets what the viewer shows around the picture: Black, Dark
grey (the default), Mid grey, Light grey, White or a custom colour. Inside the
picture's frame, anything no clip covers stays black, so the edges of a
vertical 9:16, square or scope frame always show. Choose it in **Viewer and
Playback** or from the viewer's **Guides** button. It never appears in exports.

Full/Half/Quarter preview resolution reduces the displayed frame before its
monitor conversion. Sections already rendered in the background play at the
same reduced size. Where drawing a frame again is quicker than reading the
rendered one (light colour work, mostly), playback draws it. Remove Background (AI) looks for people at half or a
quarter of its size too while playing, which is most of what it costs; paused
pictures and exports use its full quality. Decode and compositing still produce full-resolution working
frames for scopes, snapshots and exports. The separate Original / Proxy / Proxy
preferred setting uses the existing media pipeline; managed OCIO processing still
uses originals where required.

**Preferences ▸ Hardware ▸ Playback cache** offers **Auto — measured Vulkan**
and **CPU**, taking effect immediately. Auto measures supported float input-colour
conversions on the detected Vulkan device, including upload, download and output
ownership, and compares pixels with the CPU reference before choosing a backend.
It requires a consistent saving of at least 20% and 0.5 ms. Unavailable drivers or
shader tools, unsupported transforms, accuracy differences and slower processing
retain the CPU. Repeated slow GPU conversions also return to the CPU. Hardware
preferences and copied diagnostics show actual conversion counts and the reason
for the latest selection. Decode acceleration follows the existing Decoder
preference; effects, compositing and LZ4 compression remain on the CPU. Cached
pictures retain float precision, alpha and colour/pixel-aspect metadata.

**Background rendering** is on by default. After editing pauses for about a
second, OmaEdit renders timeline spans with video effects, grades, titles or
compositing into a lossless playback cache. It starts near the playhead and
pauses for playback and export. A yellow line above the timeline shows the
spans being prepared; green marks pictures ready for playback. The status bar
and **Background Tasks** show progress, with Pause, Resume, Cancel and Retry.

Effects stay editable. Changing the edit, undoing, switching sequences or
changing source/proxy settings invalidates the current cache and schedules a
fresh render. Missing cache files fall back to live processing. Audio stays
live through the mixer; exports and project files use the original edit.
Managed colour retains full float precision and HDR values.

**Preferences ▸ Playback** has the automatic-render switch, a disk limit
(20 GiB per session by default), **Clear** and **Render**. Cache pictures live
under the configured cache folder and are discarded when the session closes;
files left by an interrupted session are cleaned up on the next render.
Reaching the limit or low disk space stops rendering while keeping completed
pictures usable. Large edits may therefore be only partly cached.

Monitor volume, mute, dim and mono affect listening. **Loop In/Out** (`Ctrl+L`)
uses independent source and timeline marks, or the whole clip/timeline when clear.
Displayed fps and dropped display frames measure frames painted by the viewer and
frames replaced before painting; they are not a decoder benchmark. Counters reset
on playback start and can also be reset manually.

**Export Current Frame** saves a full-resolution PNG, or a float OpenEXR when
colour management is on (the viewer's colour engine set to OpenColorIO);
without it, choosing .exr explains this and nothing is written. PNG uses the
standard sRGB output conversion; viewer zoom,
guides and monitor looks are excluded. Dedicated external video-output devices
remain unavailable; detachable/full-screen desktop viewers are supported.

### Commands and keyboard shortcuts

Every control explains itself: rest the pointer on a button, field, slider or
dropdown and a tooltip says what it changes, with the assigned key where there
is one. The same wording is what a screen reader announces.

Press **Ctrl+Shift+P** for the **Command Palette**, then search by action name,
category or assigned key. Use the arrow keys and Enter, or click an action.
Unavailable actions remain visible with their current status. The application
menu also opens the palette.

Press **?**, choose **Help ▸ Keyboard Shortcuts**, or open **Preferences ▸ Keyboard**
to edit assignments. Search for an action, enter one shortcut per line or use
**Record Key**, then **Assign**. Up to four alternatives are supported; commas
separate a sequence of keys, such as `Ctrl+K, Ctrl+C`. Clear the field and assign
to remove a binding. Conflicts include prefixes of longer sequences; **Reassign
Keys** explicitly takes those keys away from the listed actions. Edit and Colour
can use the same keys because they operate in different workspaces. Normal text
entry and dialog navigation retain their keys.

Changes save immediately and update menu hints and shared button tooltips.
Menus show the primary binding; the shortcut editor lists every alternative.
Save named shortcut presets to switch between keyboard maps; **Reset Action**
restores an action's defaults and **Reset All** restores the whole default map
while retaining saved presets. Shortcuts are application preferences, separate
from projects. The shortcut tables below describe the defaults.

Menus, shared toolbar actions, the palette and keyboard bindings use the same
command handlers. Playback follows the active workspace, including detached
viewers. Undo and Redo show the action at the top of their history, such as
**Undo Blade Clip**.

### Background tasks and feedback

Open **Background Tasks** from the application menu, the bottom **Tasks** button
or **Ctrl+Alt+T**. Search tasks by name, type, project or status, filter active and
finished work, and select a row to inspect its report or failure log. The window
stays open while you edit. Import, proxy, verification, transfer, relink, metadata,
thumbnail, waveform, render, Render in Place, archive and tracking/stabilisation
activity use the shared list. Existing task controls in Media and Export remain
available.

Pause/resume, cancel and retry are enabled where the owning engine supports them.
The render queue can be started or resumed from this window. Import retries
refresh current catalogue information; jobs from a previous project cannot be
retried into the current one. Automatic thumbnail jobs can be cancelled; later
edits or preview requests can schedule fresh work.

The bottom bar shows actual proxy availability, offline originals, cache activity,
rendering and free project-volume space. Proxies play on colour-managed timelines as on any
other; **Originals · OCIO** shows only for a managed picture the engine cannot
draw, which is made from original media. Missing proxies fall back to available
originals. Cache progress covers the current thumbnail/waveform/cache batch;
**Cache idle** means no work is queued. Progress is left unknown when a decoder
cannot report it. Speed is percentage points per second, with approximate remaining
time once enough progress has arrived; queue and playback waits do not inflate
processing speed estimates.

Success, warning and error messages appear in the small **Info** area in the
bottom bar, without popups or desktop notifications. Messages stay until dismissed
or replaced, and never take keyboard focus. Click **Info** to read the session
history. Up to 200 finished tasks and 100 messages are retained in the shared view.

### Workspace layouts and panels

Drag the dividers to resize major panels in **Media, Edit, Colour, Audio and
Export**. Each panel's title bar has **Hide** and **Maximise/Restore** controls;
double-click its title bar to maximise or restore it. **View ▸ Workspace Layout…**
(`Ctrl+Alt+W`) shows hidden panels, manages named layouts for each workspace,
and offers **Reset Workspace** to return to the default arrangement.
**Restore All Panels** shows and docks every panel while retaining splitter sizes.
Panel visibility, splitter sizes and detached-window placement save automatically
for each workspace and return on the next launch. Layout changes preserve the edit,
playhead and unsaved indicator.

Press **F11** for full-screen application mode, or **Ctrl+F11** for the active
workspace's viewer. **Escape** restores the maximised panel, exits full screen,
or docks a normal detached panel, depending on which window has focus.

Viewer panels, Colour scopes and the Audio mixer can **Detach** into independent
windows. Move them normally or use the **Monitor** selector in Workspace Layout.
Closing a detached window docks its panel. Detached windows follow their workspace
and return when you switch back; saved windows are moved onto an available display
if their previous monitor is disconnected. Playback keys and `Ctrl+S` remain
available from detached windows, with typing taking priority over transport keys.

### Recovery, backups and portable archives

Autosave keeps a **separate recovery copy**, including untitled projects and
library-only work. It runs at the interval in Preferences ▸ General (five
minutes by default), while the project is dirty and playback, export, loading
and manual saving are stopped. It preserves the saved project and its unsaved
indicator. After an interrupted session, the next launch offers recovery.
**File ▸ Recovery and Backups…** also opens the recovery list. **Open Copy**
restores the edit as Untitled; use Save As to keep it. Explicitly discarding
changes removes that session's recovery copy.

Each manual save preserves the previous file as a timestamped backup, then
atomically replaces the project. The **Backups** tab lists versions for the
current project. Preferences keeps 20 versions per project by default,
adjustable from 1 to 100. Recovery and backups live in Qt's application data
folder, normally `~/.local/share/omaedit/omaedit/project-safety/` on Linux.
These copies contain the edit and nested sequence snapshots; their media still
needs to be available.

**File ▸ Archive Project…** creates a new folder containing `Project.omaedit`,
`Media/` and an `Archive.json` manifest. It includes media used by every sequence,
nested sequence snapshots, LUTs, external mattes and other referenced files. A
multicam clip in use brings its multicam item and every angle with it.
Enable **Include unused library media** to collect the whole library. The
project uses relative media paths, so move the complete folder and open
`Project.omaedit` from its new location. Copying runs in the background and can be
cancelled; incomplete archives are removed and existing folders are preserved.

Whole sources are copied unchanged by default. Optional **handles** retain extra
frames before the earliest use and after the latest use of each eligible source.
Trimming creates lossless FFV1 video/PCM audio intermediates, which may be larger
than compressed originals. Retimed, frozen, stabilised, proxy-backed, nested or
incompatible sources stay whole. The manifest records which files were trimmed.

### Freeze frames, flips, reattaching audio

`Alt+F` (Edit menu ▸ Freeze Frame, or timeline toolbar ▸ Video) blades the
clip under the playhead and inserts a two-second still of that frame —
video only, so nothing hums — rippling the tracks the way an insert does.
The clip's linked sound is cut at the same frame: the part before the still and
the part after it each stay linked to their own sound, and the still plays in
silence. The still wears a `❄ freeze` badge and trims like any clip.

The Inspector's **Flip** section mirrors a clip horizontally or vertically;
**Uniform scale** in Transform keeps W and H together while you type.
Dragging the viewer's transform or crop handles updates the picture live, as do
the Inspector's Position, Size, Rotation, Anchor, Crop and Opacity controls.
Each drag is one undo step. Colour-wheel, mixer-fader and pan-knob drags also stay
one undo step when you pause with the mouse held down and then continue.

**Detach Audio** (`Alt+Shift+S`) moves the sound of a clip that still
carries it onto an audio track. Clips placed now already have their sound
on an audio track, so it matters only for a clip from an older project
before its first edit. **Reattach Audio** on the same menu brings it back as long as
the audio clip still sits exactly under its picture.

Blocks wider than a couple of hundred pixels show their source in-point
timecode bottom-right. Clicking a block's `fx` badge opens the Inspector's
Effects tab for it, where every applied item has a remove.

The Inspector's **Audio** tab maps a clip's channels (stereo as recorded,
mono fold, swap left/right, left or right on both) and can **normalise
loudness** to −23 LUFS on the fly. A selected title drags and resizes in
the program monitor like any other clip.

### Multicam

Right-click library clips (Edit page library or the Media page, where a
multi-selection counts) and choose **New Multicam Clip…**. Tick the angles
in the order you want them, name the clip, and pick how to line them up:

| Sync by | What it uses |
|---|---|
| Audio waveform | The angles' sound, first three minutes (the cameras heard the same room) |
| Timecode / creation time | The container's timecode track when every angle has one, else the files' creation times |
| First marker | Each angle's first clip marker (Media page ▸ Clip Log) is the same moment |
| Clip start | All angles start together |

The multicam clip lands in the library with a `MULTICAM ×N` badge. Append,
insert, overwrite or drop it like any clip: angle 1 goes on the timeline,
and the block reads `⧉ <angle>`.

`Ctrl+Shift+M` (or the grid button on the program monitor) opens the
**multicam viewer** in the source monitor's place: every angle at the
playhead's moment, the one on air outlined. While playing, click an angle
or press `1`–`9` to **cut** to it at the playhead; while stopped the same
click or key **switches** the segment under the playhead. `Shift+click`
always cuts at the playhead; `Shift+1`–`9` and `Alt+click` always switch the
segment.
Every cut is one undo step. Right-click a multicam block ▸ **Switch Angle**
addresses a clip directly.

**Audio follows video** (the speaker button in the viewer, or Clip menu)
is on by default: a cut takes the new angle's sound. Turn it off and the
sound playing at the first cut stays, detached onto an audio track, while
later cuts change the picture only.

**Sound from** in the New Multicam Clip dialog decides this for the clip
itself. *Follows the picture* is the behaviour above. Choose one of the
ticked clips instead, a camera or a sound recording such as a studio mix,
and that clip's sound plays under every angle: cuts and switches change the
picture only, whatever the Audio follows video button says (it is greyed
out while such a clip is under the playhead, and the viewer says where the
sound is from). A sound recording chosen here is lined up with the cameras
like any angle but is not one: it has no cell in the viewer and no number
key. The sound block on the timeline is there only where the source has
sound; before it starts and after it ends the track is empty and silent.

The choice can be changed after the clip is made. Right-click a multicam
block on the timeline ▸ **Sound From** lists *Follows the picture* and every
source that was lined up when the clip was made and has sound, with the one
in use marked; choosing one changes the sound of that whole placed clip,
across all its angle cuts, as one undo step, and leaves the picture cuts
alone. The pieces it reaches are those of the same multicam clip on that
track that still lie where the placed clip put them, or touch such a piece;
a copy, a second placement, or a piece moved away is a clip of its own. A
sound that was deleted or unlinked is not made again. The same **Sound
From** on the clip in the media panel changes it for placements from then
on; clips already on a timeline keep their sound.

When the cutting is done, right-click a timeline's card in the media panel
(or its tab) ▸ **Collapse Multicam**: every multicam piece on that timeline,
open or not, becomes an ordinary clip of the angle it shows, as one undo
step. The cuts, the pictures and the sound stay exactly as they were; a
stretch with no footage stays black. A sound that is its picture's own stays
linked to it. The sound of a clip with a sound source of its own, or a sound
kept on another angle, is left where it is as sound clips of their own,
unlinked, since linked sound is always its picture's own. The multicam clip
in the library, and other timelines that use it, are not changed. The entry
is greyed out for a timeline with no multicam clip, and nothing is collapsed
while a track that holds a piece (or a sound to be unlinked) is locked.

### Tracks

Each track header carries:

| Toggle | Meaning |
|---|---|
| `●` | Destination track for `E` / `W` / `D` edits |
| `L` | Lock — the track refuses every edit and never moves |
| Link | Sync lock — the track rides ripples from V1 |
| `V` / `M` | Disable a video track's picture / mute an audio track |
| `S` | Solo an audio track |
| Microphone | Arm an audio track for recording |

Right-click a header to **add** a video or audio track, **move** the track
**up or down** past its neighbours (video tracks climb away from `V1`, audio
tracks descend; a higher video track composites on top; undoable), set the
**track height**, or **delete** an empty one. Shift+wheel over a header or
dragging its lower edge sizes that track independently; heights save with the
project. Every track starts the same height, `V1` included. `Ctrl+Shift+=` /
`Ctrl+Shift+-` size them all together, bringing back any track sized on its own.
When there are more tracks than fit, the wheel over the headers scrolls them up
and down (so does Shift+wheel over the tracks, or the scroll bar at their right),
and the ruler and playhead stay at the top. An audio header menu also starts/stops recording on that track. Drag a clip up or down to move it
to another track of the same kind; drag past the last row to create a new
track.

### Editing

Every length reads as time: a clip's duration, fades, dissolves, handles,
pre-roll and keyframe spacing show as `6.12 s` or `1:23.40`, never as a count
of frames. Type seconds (`2.5`), minutes and seconds (`1:30`), a full timecode
(`00:00:04:00`) or, when you want it, a frame count (`12f`); the length lands on
the nearest whole frame. Positions such as the playhead, In and Out stay timecode.

The toolbar above the timeline holds **Trim**, **Speed**, **Video** and **Audio**,
followed by marker, label and title controls. The tool picker
shows the active Select, Track Select, Trim, Blade, Slip, Slide, Hand or Zoom tool;
keyboard shortcuts still select tools directly. Controls stay in the same place
and disable when they do not apply to the selection. Zoom and view controls move
to a second row when the window is narrow.

Right-click a timeline clip for **Copy**, **Paste at Playhead**, **Duplicate**,
**Paste Attributes**, **Source**, **Disable Clip**, **Delete (Leave Gap)** and
**Ripple Delete**. Every track uses the same ordering. Titles put **Edit Title**
first; multicam clips add **Switch Angle**. An unrelated multi-selection offers
actions that operate on the selection, while a linked picture and sound remain
one clip target.

Detailed transform, audio and transition commands reveal the relevant Inspector
tab, including when the panel is hidden. The existing speed, picture-in-picture,
pan and EQ presets are in the Modify menu. The Media workspace groups
clip commands into **Add to Timeline**, **Organise**, **Create** and **Proxies**;
Input Settings and Clip Attributes stay directly accessible.

Edits ripple only across sync-locked tracks. Empty space on a
track is just that — a gap you can drop into, trim into, or ripple away.

- `E` append at end, `W` insert at the playhead, `D` overwrite at the
  playhead — all onto the destination track (`●`).
- `Q` places the source clip on the track above the playhead; `Shift+Q`
  places its audio only.
- **Drag from the source viewer**: press on the source viewer's picture and
  drag it to a track, and the range marked there with `I` / `O` lands where
  you let go (no In: from the start; no Out: to the end), just as the clip's
  tile dragged from the library would. A click on the picture does nothing,
  and the strip under it still scrubs.
- **What an edit uses**: the three-way switch in the source viewer's
  transport row (and **Edit ▸ Replace ▸ Edits Use**) sets whether append,
  insert, overwrite, replace and a drag from the bin use the clip's
  **Picture and Sound**, its **Picture Only** or its **Sound Only**. A picture
  alone brings no sound and leaves the sound under it; a sound alone goes to
  the first audio track with room for it (an insert parts the first audio
  track and ripples) and leaves the pictures. An audio row always uses the
  sound; a clip with only one of the two is used as it is. The choice is
  kept between runs and starts as Picture and Sound.
- `Backspace` deletes a clip and leaves a gap; `Delete` ripple-deletes it
  (the gap closes and sync-locked tracks follow).
- **Selecting several clips**: `Ctrl`-click adds a clip to the selection
  (or takes it out), `Shift`-click selects every clip between the first
  selected clip and that one on its track, dragging on empty timeline
  space sweeps a marquee, and `Ctrl+A` selects everything (`Ctrl+Shift+A`
  nothing). The Track Select tool in the toolbar takes a clip and
  everything after it on its track (`Ctrl`-click: every track,
  `Alt`-click: everything before it). Delete, Backspace, nudging, `V`,
  labels, copy and paste, and dragging a track clip all act on the whole
  selection as one undoable step. The Inspector shows the first selected
  clip.
- **Linked selection** (the link button in the toolbar, `Ctrl+Shift+L`): a clip and the
  audio detached from it select, move and delete together. `Alt`-click a
  clip to pick just that half for one edit. Deleting a V1 picture without
  its sound leaves a gap in its place, so later clips stay with their
  sound. While you drag a picture — moving it, trimming an edge or
  dragging it to a new place in the order — its linked sound moves with it
  on screen, not only after the drop. Dragged on its own (linked selection
  off, or `Alt`-click first), a picture or sound moves alone: its partner
  stays where it is and the two are no longer linked.
- **Clip ▸ Unlink Picture and Sound** (also on a clip's right-click menu)
  parts the selected clips from their linked partners for good: each moves,
  trims and deletes on its own.
  **Clip ▸ Link Picture and Sound** joins a picture and sound cut from the
  picture's own file (select both) into one pair again and puts the sound
  back under its picture.
- Grading lives on the Colour page. The Inspector's old brightness, contrast
  and saturation sliders show only on a clip from an older project that
  already uses them, so it can be changed or reset.
- `Ctrl+B` blades at the playhead; the Blade tool (`B`) cuts wherever you click a clip. `Alt+W` inserts empty space.
- `Ctrl+Shift+B` **Blade All at Playhead** cuts every unlocked picture/sound
  track in one undo step. Linked members cut together; if one cannot be cut,
  the pair stays intact. Gaps, transitions, edit points and caption tracks are
  skipped. Hidden, muted and sync-unlocked tracks still participate.
- **Clip ▸ Detect Edits** finds the cuts inside the selected clip, such as a
  programme recorded off air or a camera reel, only within the part the
  timeline shows. **Blade at Each Edit** cuts the clip into its shots in one
  undo step; **Add Markers at Each Edit** leaves it whole and marks each new
  shot. It runs in the background (Stop Detecting ends it) and needs ffmpeg;
  retimed clips are left alone.
- Drag a clip edge to trim. **Normal trim** (default) moves the edge and
  opens or closes a gap; press `T` for the **Trim tool**, where trims ripple
  everything downstream. The `TRIM` badge by the timecode shows the mode.
- **Trim & Arrange** offers **Trim Start to Playhead** (`Alt+[`) and
  **Trim End to Playhead** (`Alt+]`) in the Edit workspace. Both retain the
  picture under the playhead and follow the current normal/ripple trim mode.
  Available source handles can extend the chosen edge. These commands act on
  one selected clip and require its linked tracks to be unlocked.
- **Trim & Arrange → Roll Edit** moves the shared cut while preserving the
  pair's duration. Use `Alt+,` / `Alt+.` for one frame, add `Shift` for ten,
  or `Ctrl+Shift+X` to move the cut to the playhead. Select an edge, a clip
  before the cut, or open the Trim Editor. The two-up view follows the cut.
  Picture rolls carry linked sound edges while preserving independent J/L trims;
  rolling on a sound track changes only sound. A cut that carries a transition
  rolls with it: the transition stays across the cut with its length, sides and
  kind, and the roll stops where either clip runs out of the spare frames its
  half needs. Locked linked tracks, insufficient handles, gaps, a transition on
  the far cut of either clip and collisions refuse the complete edit.
  With the mouse, drag the cut itself: with the ordinary pointer (or the Trim
  tool) the pointer turns into a roll within a few pixels of a cut where two
  clips butt together, on any track, a multicam clip's angle cuts included.
  Both clips and their linked sound follow the cut as you drag, it stops where
  either clip runs out of spare frames, and nothing after them moves. Just
  beside the cut, a selected clip's own edge still trims that clip alone.
- Video clips show a filmstrip of frames spaced across their timeline length.
  Thumbnails adapt to the available width and zoom level; scrolling loads
  frames for the visible section, and trimming updates the sampled range.
- `Ctrl+C` / `Ctrl+V` copy the selected clip and paste it at the playhead
  (overwrite, onto the destination track); `Ctrl+D` duplicates a clip right
  after itself. `Shift+X` extends the selected edge to the playhead;
  `Alt+←` / `Alt+→` nudge a clip by a frame (`Shift` for ten). On V1 a
  nudge moves the clip and everything after it, so a nudge left needs an
  empty gap before the clip to move into; on other tracks the clip moves
  alone, into free space. When a nudge can't happen the status line says
  why. `Shift+R`
  replaces the selected clip with the selected library clip's material.
- `Ctrl+T` cross-dissolves a clip into the next clip on its track;
  `Ctrl+[` / `Ctrl+]` fade in / out. Both work on any track. Drag the dots
  at the ends of the outlined fade ramps to set their lengths. Applied
  fades and dissolves stay visible without selecting the clip.
- A new transition (`Ctrl+T`, or a tile dropped from the Transitions list)
  sits across the cut, half on each side, about a second long. It is made
  of the frames each clip has spare beyond the cut, so the edit keeps its
  length and nothing after it moves. If one clip has no spare frames on its
  side, the transition shifts to the other side; only where neither clip
  has any do the two clips overlap, and the edit gets shorter by that much.
  Its box is drawn over the end of one clip and the start of the next, with
  the cut still showing inside it at its own frame.
- Drag the vertical grip on either edge of the box. The left grip moves
  where the transition starts and the right grip where it ends; the other
  edge and the cut stay put, and the edit keeps its length. Each side goes
  as far as that clip has spare frames. The tooltip shows frames and
  seconds, and each drag is one undo step. Drag an edge to the cut and the
  transition sits on the other side only; drag both to the cut and it is
  removed, as `Ctrl+T` again does.
- The Inspector's **Alignment** says where a transition sits: centred on
  the cut, starting at it, ending at it, or **Custom** when you have set
  the two sides by hand. Choose **Overlap** to have the two clips overlap
  instead (the edit gets shorter); an overlap has one grip, on its leading
  edge. Changing the length in the Inspector keeps the cut where it is.
- Delete, lift or move a clip that is in a transition and the clip next
  to it gives back the frames the transition had borrowed: it ends (or
  starts) at the cut again, as if you had removed the transition first.
  One undo brings back the clip and the transition.
- A copy of a clip that is in a transition (copy and paste, duplicate) is
  the clip as it is drawn, from its cut to its cut, without the frames the
  transition borrows; a duplicate goes in at the cut. Copy both clips of a
  transition together (or cut them, duplicate them, or drag them with `Alt`
  held) and the pasted pair has the transition too, with its length, sides
  and kind.
- Replace (`Shift+R`, from the source's end, or Fit to Fill on a selected
  clip) covers the clip as drawn and keeps the transitions on its cuts. A
  side of a transition needs spare frames of the new clip beyond the cut:
  where the new clip has fewer, that side is shortened to what it has, a
  transition with nothing left on either side is removed, and the status
  line says which. One undo brings back the old clip and its transitions.
- A clip in a transition trims at its other edge, on any track, and
  changing its speed or reversing it keeps the transition across the same
  cut. The edge under a transition is the cut itself, in the middle of the
  transition's box. With the Trim tool (`T`, trims ripple) drag it as you
  would any cut: within a few pixels of the cut the pointer shows a roll,
  and on either clip's edge beside it a trim of that clip. The cut moves
  (or the incoming clip starts later or earlier in its source), what
  follows moves with it, and the transition stays across the cut with its
  length, sides and kind. Each clip must still have the spare frames its
  half of the transition needs and keep a frame of its own, and the drag
  stops there. Trim Start and Trim End to Playhead, Extend Edit, the edge
  nudges (`,` and `.`), Change Duration and the Trim Editor do the same at
  that edge. The grips that change the transition's own length stay where
  they were, at the two ends of its box.
- With the Select tool, whose trims leave the following clips in place, the
  edge under a transition has no trim handle and a trim asked for there is
  refused. That edge does not slip or slide either, a blade or a gap does
  not go through a transition's frames, and the edge takes no fade: the
  status line says so each time.
- An insert, overwrite or move that lands inside a dissolve on an upper
  track removes that dissolve first and says so in the status line; one
  undo brings back both. On the main track the same goes for a transition
  that sits across its cut: insert at the cut, overwrite across it or
  delete a range through it, and the transition is removed first. (An
  **Overlap** transition on the main track still refuses those edits;
  remove it first.)

- Right-click a clip ▸ **Label** gives it a colour; double-click a track's
  badge to rename the track.
- The Slide tool (`U`) drags a clip between its neighbours, which give and
  take frames to keep the total length. `Ctrl+J` joins two clips that are
  one continuous piece of the same source, healing a blade cut; the
  linked sound's cut heals with it.
- The media panel's **Effects** and **Titles** tabs are tile grids:
  double-click applies to the selected clip (or opens the title dialog with
  that preset), or drag a tile onto a clip. Transitions are on the Effects tab
  (choose **Transitions** in its category list). Drag one onto a clip and it
  takes the nearer cut, or the clip's other cut if that end has no neighbour.
  `Ctrl+T` still adds a cross dissolve. A title drops at the frame under the
  pointer.
- A media tile dragged over a track lands where you let it go: it starts
  under the pointer and, with snapping on, catches the playhead, cuts and
  markers. It shows there for its length while you drag; hold it against
  either edge of the timeline to scroll on.
- A clip dragged along a track snaps as it moves. Where it meets another clip
  on that track it catches and holds for a moment, so the two can be butted
  together; keep pushing and it slides over. `N` turns snapping off.
- While a tile is dragged, an empty row opens above the video rows and below
  the audio rows. The clip shows on it under the pointer, as on a track; drop
  it there to create a new track for it.
  A selection holding both picture and sound makes one new track of each kind.
- **Titles** (`Alt+T`, or double-click a title on the timeline) take a
  font, preset or custom pixel size, colour, Regular / Bold / Italic,
  alignment, tracking, line spacing, background and outline. Text accepts
  multiple lines. Drag the title in the monitor to place it, or use
  **Edit title here** to change text directly in the viewer.
- **Captions**: File ▸ Import Captions… reads a `.srt` or `.vtt` file
  onto dedicated caption tracks as editable lower-third titles; overlaps
  use separate tracks. The **CC** role survives renaming and prevents ordinary
  media placements. Toggle **Burn captions into video** in each header menu;
  The Export page’s subtitle mode controls the overall export. File ▸ Export
  Captions… writes all caption tracks back out as `.srt`.
  **Edit ▸ Captions**: **Add Caption** (Alt+C) puts a two-second caption at
  the playhead; double-click it to write its words. **Split Caption** turns a
  caption of several lines into one caption per line, each timed by its length.
  **Join Captions** makes the selected neighbouring captions on one track into
  one. **Generate Captions** writes captions from the timeline's speech using
  each clip's transcript (Inspector ▸ Transcript); clips without one are
  transcribed first, one after another, and the captions follow. Retimed clips
  are left out. A caption never lands on another: it goes to the next caption
  track instead. Each is one undo step.
- **Timeline interchange**: File ▸ Import Timeline… reads an OpenTimelineIO
  (`.otio`), CMX 3600 EDL (`.edl`), XML timeline (`.fcpxml`), legacy XML
  timeline (`.xml`) or AAF (`.aaf`) file as a new sequence in the current
  project, importing any media the library lacks. File ▸ Export As ▸ writes
  the active sequence in the same five formats. The Export page's
  Export Timeline button offers the same five. Tracks, cuts, dissolves,
  speed changes, clip names, source timecode, markers and enabled state
  travel; other effects, grades and audio mixes stay in the project file.
  Each format has limits. An EDL carries only the V1 picture edit, with
  other video tracks written as comments that most tools ignore. An
  `.fcpxml` file keeps dissolves between pictures but not crossfades
  between sound clips. OTIO and AAF files write a clip's linked sound as
  part of its V1 clip rather than as a separate A1 clip.
  XML import preserves linear time maps with holds, reverse playback and
  frame-sampling choices. Unsupported smooth curves, retimed markers/anchors,
  pitch/rate-conform settings or timing that falls between project frames return an error.
  Speed-segment export still requires Render in Place or a native project.
  AAF needs the optional `python-aaf2` package (Preferences ▸ Plug-ins
  reports it). AAF work runs in the background: it appears in Background Tasks,
  can be cancelled, and a cancelled or failed export leaves no partial file.
- Nested clips (`Alt+G` / `Alt+Shift+G`), the trim editor
  (double-click an edit point), retiming, audio EQ and
  pan, picture-in-picture, titles (`Alt+T` / `Alt+Shift+T`), markers (`M`)
  and volume / opacity keyframes live in the menus across the title bar, the
  media panel's **Effects** tab (`Ctrl+5`
  flips the panel to it and back), and the toolbar above the timeline.
- The menu bar runs File, Edit, Trim, Mark, Clip, Modify, View, Window, Help.
  **Modify** holds what changes a clip: stabilise, auto balance, volume, fades,
  pan, EQ, category, rotate, retime, reverse, frame sampling and render. **Window** shows
  panels, switches workspaces, and records a voiceover. **Mark** rates the
  source clip (Favourite, Reject, Unrate), adds to-do and chapter markers, and
  deletes markers in a range. `Ctrl+X` cuts, `Ctrl+F` searches the timeline,
  `Alt+M` adds a marker and opens it, and `Ctrl+M` deletes the marker at the
  playhead. `M` marks the selected clip when the playhead is on it and the
  timeline otherwise; `Alt+Shift+M` (**Mark ▸ Markers ▸ Add/Remove Timeline
  Marker**) always marks the timeline. A bar along the bottom of the timeline
  scrolls through the edit without moving the playhead. The Audio page's
  timeline has the same bar, and the wheel works there as it does here: a
  plain turn or a sideways swipe moves along the time, `Ctrl` zooms (or the
  other way round, per Preferences ▸ Input), and `Shift` scrolls the tracks
  when there are more than fit.
- **Clip ▸ Sync Clips…** lines up pictures and separately recorded sound, by
  their sound, timecode, first marker or start, and puts one new clip in the
  library marked SYNCED. The originals are not changed. Drag it onto a timeline
  to use it; double-click it to open it and adjust the sync by hand.
- **Categories** say what each clip is for: Dialogue, Music, Effects, Video,
  Titles and any you add under **Modify ▸ Edit Categories…**. A clip's picture
  and its sound have one each, shown as the coloured strip along its foot. Set
  them from **Modify ▸ Category**, or `Ctrl+Alt+D`, `Ctrl+Alt+M` and `Ctrl+Alt+E`
  for the three sound ones. The **Categories** tab of the timeline list switches a
  whole category on or off, and the Export page can make one WAV stem per category.
- `Ctrl+Shift+2` opens the timeline list — a searchable list of every
  clip, title and marker.
- The tracks run on past the last clip at any zoom, so the end of the edit
  scrolls well into view with room to place clips beyond it. Fit lays the
  whole edit across the view, with the empty tracks after it to the right.
- Drag the playhead along the ruler and it carries on past the last clip.
  Hold it at either edge of the view and the timeline scrolls along with
  it. Past the end, the viewer keeps the last picture.
- **Timelines**: the scrolling tab row holds the project's timelines. Use
  **+ New Timeline** to create one; click to switch, double-click to rename,
  or right-click to duplicate, nest, stack or delete. They also show in Project
  Media ahead of the clips: double-click one to open it, right-click for the same
  menu, or drag it onto a track to use it as a clip. **Move to Bin…** on that
  menu, or dragging a tile onto a bin, files it there. The Library always shows
  every timeline, and the Media page lists them above its clips the same way.
  **Delete Timeline** asks
  for confirmation and removes the edit from the project; source media remains.
  Export renders the active timeline. Switching starts a fresh undo history.
- **Markers** (`M`) go on the timeline — or, when a clip is selected and
  the playhead is over it, on that clip, where they ride every trim and
  move and show as small flags along its top edge (click one to jump to
  it). `Shift+M` opens the marker editor for its name, type, colour,
  multiline notes and position. Each type (Standard, Chapter, To Do,
  Completed) has its own colour; **Colour** gives a marker red, orange,
  yellow, green, cyan, blue, purple, pink or white instead (the **A** swatch
  goes back to the type's colour). The colour shows on the ruler, on clip
  flags and in the timeline index. Enter a frame number or `HH:MM:SS:FF`; duplicate or
  out-of-range positions are rejected. **Delete Marker** acts on the marker
  shown in the dialog even if the playhead moves. Save/delete use one undo step;
  locked tracks and changed project/clip targets are protected. Clip positions
  use retimed source frames; timeline positions start at sequence frame zero.
  `Ctrl+;` / `Ctrl+'` walk every marker, and drags snap to them.
- The monitor's **Overlay** button switches between the transform rect
  (drag the body or a corner of a track clip) and a **crop box** with an
  edge handle on each side; the Inspector's Crop section has the numbers.
- The Inspector's **Composite** section gives a track clip a blend mode
  (Multiply, Screen, Overlay and the rest). Opacity sits under Transform,
  with its keyframe diamond, and Transform's reset puts it back to 100% in
  the same undo step (an effect layer, which has no Transform, keeps its
  opacity under Composite). The Inspector's header and source line give
  the clip's length in seconds, as the media cards and timeline cards do;
  a timeline card gives the size the timeline plays at; **Lens
  Correction** straightens barrel or pincushion distortion; **Stabilise**
  analyses the clip's motion in the background, then smooths it — change
  the smoothing any time without analysing again.
- The Inspector's **Effects** tab lists everything applied to the clip.
  The eye on a row **bypasses** it — the value stays, its effect goes,
  and the project remembers — and the × removes it. The monitor's
  **Before / after** button wipes the edit against the same frame with
  every attribute switched off; drag the divider.

### Nested clips

Select adjacent clips on **V1** and press `Alt+G` to group them into one
nested clip, or mark an In/Out range to group that span. Linked sound stays
on its audio track. The new nested clip is selected; `Alt+Shift+G` breaks it
apart, respecting any trims. Each action is one undo step.

V1 must be unlocked. Remove dissolves touching the span and ungroup
the nested clips inside it first. If grouping is refused, the status bar
explains why and your selection and range remain available to adjust.

### Transform, crop and speed, the finer points

- **Scaling** (Inspector ▸ Video ▸ Transform) is on every picture clip. On a
  clip that is already the timeline's size and shape every choice looks the
  same, and the Inspector says so. It follows the timeline's Scaling until you
  choose Fit, Fill, Stretch or Original size for that clip alone; Position,
  Size and Crop then work on the scaled picture. One undo step.
- **Anchor** (Inspector ▸ Transform) is the point a track clip rotates
  about, as a fraction of the clip: 50 / 50 is its centre, 0 / 0 its
  top-left corner. Position and size stay what you typed.
- **Crop** trims the clip's own picture, before Position and Size place it:
  25% off each side of a half-size picture in picture keeps its middle half,
  and the crop box sits inside the clip's rect on the monitor.
- **Feather** (Inspector ▸ Crop) softens the crop's edge by a fraction of
  the picture; **Invert** keeps the outside and cuts the box away. Both
  work on any clip, together with the monitor's crop handles, and only the
  clip itself is cut — the tracks below show through.
- **Change Duration…** (Modify menu, or the command palette; give it a key
  in the shortcut editor) sets how long the selected clip is without changing
  its speed. Type the length in seconds (`2.5`) or minutes and seconds
  (`1:30`); a full timecode such as `00:00:04:00` works too. Its end
  moves; on the storyline the clips after it follow, while a connected clip
  keeps its place. A media clip can get shorter, or longer only as far as its
  media goes, and the box says how far that is. Stills, titles and colour
  clips can be up to 10 minutes long at 25 fps. One undo step.
- **Speed / Duration…** (timeline toolbar ▸ Speed, Edit ▸ Retime, the command
  palette or Inspector ▸ Speed) previews the resulting length before you apply.
  Enter a percentage from 5–2000%, or a duration in seconds (`2.5`, `1:30`), and
  optionally reverse playback. Unrepresentable durations are rejected instead
  of silently rounding to another length. Linked video/audio is one target;
  changes preserve audio pitch, existing frame blending, clip markers and
  animation keys. Later clips follow the existing ripple settings. Apply is one
  undo step; stale clips, changed projects/sequences and locked tracks are guarded.
- **One-click retimes** (Modify ▸ Retime) on the selected clip: **Hold** (Shift+H)
  freezes the playhead's picture for two seconds; **Blade Speed** (Shift+B) splits
  the clip's speed at the playhead so each side can take its own speed; **Instant
  Replay** plays the clip twice; **Rewind ×1, ×2, ×4** plays it, runs back to the
  start at that speed and plays it again. The clip grows to fit and later clips
  follow. Each is one undo step, and each opens up in Speed Segments for changes.
- **Speed Segments…** (Edit ▸ Retime, command palette, or Speed / Duration) edits
  variable speed on a graph. Select a segment, set its starting and ending rates,
  choose Linear or Smooth, or blade its timing at the cursor. Rates are signed
  percentages: 100 is normal, negative reverses, and zero holds. The duration
  preview updates before Apply; all changes form one undo step. Presets include
  replay, rewind and play, reverse, hold, and Normal speed, which removes the
  mapping and plays its source range forwards once. Reset changes restores the
  draft from when the editor opened. Linked audio follows picture edits; animation
  and clip-marker positions scale with the total duration. When the speed
  changes across a segment, the clip's sound follows that speed sample by sample,
  so its pitch rises and falls with it, and a cut or seek inside the clip keeps
  every sample in place; clips whose segments are all constant keep their pitch.
  Holds and absolute rates below 10% or above 1000% are silent. Speed segments support Floor,
  Nearest frame, Frame blending and Optical flow in the viewer and export, including colour-managed
  footage. They require native project storage; timeline export asks you to
  render them in place first.
  **Paste Attributes** and saved presets include speed patterns. Applying one
  fits it to the destination's source range; segment durations scale with that
  range, with small rate adjustments where whole frames require rounding.
  A source frame-rate change is refused if it would put existing speed segments
  beyond the available footage, including nested timelines and undo history. Reset or
  render those clips in place, then save and reopen before changing that rate.
- **Sampling** (Inspector ▸ Speed) chooses how to sample pictures between
  source frames. *Floor* chooses the picture at or before the source position;
  *Nearest frame* chooses the closest picture. Both apply in ordinary,
  interpreted and speed-segment clips, including colour-managed output.
  New media placements use *Floor*. *Original sampling* keeps older clips'
  saved behaviour; reopening or pasting an older clip does not change its mode.
  For speed segments, *Frame blend* mixes the two adjacent
  pictures by their fractional position, including reverse playback and holds.
  Sampling changes appear in the viewer immediately and survive blade edits,
  undo and saved projects. Colour-managed clips blend in linear light.
  *Optical flow* follows motion between pictures. It switches at scene cuts
  and blends pictures when the motion estimate is unreliable.
  For constant-speed clips, *Frame blending* and *Optical flow* still bake on
  **Render in Place** and **export**, with cached intermediates. Their viewer
  plays the nearest picture; colour-managed export requires Floor or Nearest frame
  or conversion to speed segments with Frame blend or Optical flow.

### Picture animation and motion presets

The diamonds beside **Position**, **Size**, **Rotation**, **Anchor**, **Crop**,
**Colour** and **Opacity** add or remove a keyframe at the playhead. A highlighted
diamond marks a key at this frame. Editing an animated value updates that frame;
moving a picture in the viewer follows the same rule. Existing easing is retained,
and a drag makes one undo step. Removing the last key keeps the current value.

**Inspector ▸ Video ▸ Animation** provides previous/next navigation, editable
keyframe times and values, a curve display, copy/paste and Linear, Hold or Smooth
interpolation for transform, crop, colour and opacity; its paste puts one
property's keys at the playhead. **Edit ▸ Keyframes ▸ Cut, Copy, Paste** does the
same for a whole clip's animation: every animated property and the opacity. Paste
puts the first copied key at the playhead on the selected clip, which can be a
different clip. Its own keys within the pasted stretch give way, those before
and after it stay, and keys past the clip's end are left off. Cut empties the
clip's animation. Each is one undo step. The timeline opacity curve
shows the same easing and keeps it when you drag a key. Trims retain control points
outside the visible clip so extending it restores the original curve. Splits,
retiming, native project saves and both render pipelines preserve opacity easing.

**Motion Presets** provides Zoom In/Out and Pan Left/Right/Up/Down. Set the amount
and easing, then **Apply Motion**. Presets replace position and size animation
across the clip using its saved framing, so applying a different preset does not
keep enlarging the picture. **Start** and **End** seek to the endpoints; refine
their framing by dragging in the viewer or editing the Inspector.
**Paste Attributes** and saved presets retain the selected groups' animation.

Titles also offer Fade, Slide Left/Right, Rise and Scale entrances/exits. Their
In / out duration is measured in seconds and shortened for short clips. These
presets replace position, size and opacity animation, with one undo step. Motion
uses ordinary clip keyframes and survives native project saves, splits and trims.

### Effects shelf, generators and effect layers

- **Video effects:** blur, sharpen, glow, vignette, keying, spill suppression,
  corner pin, fisheye correction, grain, pixelation and stylised looks appear
  in the Effects browser. Filter by category or search, then double-click a
  tile or drag it onto a picture clip. Unavailable plug-ins have dimmed tiles.
  **Inspector ▸ Effects ▸ Video Effects** shows the clip's ordered stack:
  expand a card to edit its controls, use the eye to bypass, arrows to reorder,
  reset to restore parameters, or remove. Numeric controls accept typed values
  and preview changes during a drag, coalescing rapid updates on the render
  worker. Each drag is one undo step. Repeated instances
  of an effect have independent settings; **Paste Attributes ▸ Video Effects**
  and saved presets carry the stack.
- **Keying:** **Chroma Key** takes out a green, blue or magenta screen, finding
  the screen's colour by itself when added. **Remove Background (AI)** finds the
  people in the picture with no screen at all (Fast, Balanced or Best; or blur the
  background instead of taking it away). It knows people only, and works best on
  one or two people from the waist up. **Colour Key** takes out one colour.
  **Light Wrap**, placed after a key, lets the light of what is behind the clip
  spill over its edges. A key first in the stack works on the clip as filmed and
  at its own size, so neither the timeline's size nor the clip's grade changes
  its edges. **Effects window…** in the Inspector opens the clip's effects beside
  a preview of the clip alone, its matte (**Show matte**) or the timeline
  (**In the timeline**), and a click on that preview picks the backdrop.
- **What filters see:** filters that work on brightness or colour values, and blend
  modes, work on the timeline's codes, what the viewer shows. So Threshold at 0.5
  splits the picture at the viewer's mid-point, and Multiply is exact in those codes.
  By default the stack runs after colour and before crop/transform. Effects
  persist in projects and are included in playback and export. These filters
  currently require the standard colour pipeline; the managed linear renderer
  asks you to bypass them or turn colour management off. Missing active plug-ins
  are reported before export, with settings retained for recovery.

- **Applied Effects** in the Inspector has bypass, earlier/later arrows and
  reset/remove controls. Order changes rendering and saves with the clip.
  Right-click a timeline **fx** badge for direct removal. Right-click a
  dissolve or a fade on the timeline (or click the **↔** badge) to remove it.

- The **Effects** tab has a shelf above the tiles: **All**, **★
  Favourites**, **Recent** (the last eight you applied, latest first;
  right-click ▸ clear) and your **bins**. The star on a tile keeps it as
  a favourite; right-click a tile to add it to a bin, or make a new bin
  with it; right-click a bin to rename or delete it. Presets you save
  from the Inspector sit here like any effect.
- The **Titles** tab's second half is **Generators**: Solid Colour
  (pick the colour in the Inspector), Colour Bars, Noise, Counting
  Leader and **Effect Layer**. Double-click one to place it at the
  playhead, or drag it onto the timeline; each is 4 s and trims like any
  clip. On an empty timeline a generator becomes the first clip on V1;
  an Effect Layer is refused there, as it needs a picture under it. A
  counting leader counts its own seconds down to 1. An effect layer's
  Colour, Lens Correction, Flip, Crop and Video Effects
  process the video stack below its track for its length. Higher tracks
  composite afterwards. Disabled effect layers/tracks have no effect.

### Stacked timelines

Right-click a sequence tab ▸ **Stack Above Timeline** and that sequence
draws compactly above the timeline, a row per track. Drag a block down
onto a track to place that clip (attributes and all) at the frame, or
double-click it to place it at the playhead on the destination track.
The × on the strip, or switching to that sequence, takes it away.

### Transcript

The Inspector's **Transcript** tab transcribes the selected clip's speech
— **Transcribe with Whisper (AI)** runs whisper.cpp on this machine over
the clip's audio and writes the lines, with their times, onto the library
clip (the project keeps them; the media and the timeline are never
changed; **Clear** removes them). Search narrows the lines; click one to
set the I/O range over those frames on the timeline, or to range the
source viewer when the clip is only in the browser. **Make Captions** turns
the transcripts of the timeline's clips into a caption layer, as **Edit ▸
Captions ▸ Generate Captions** does, transcribing first any clip that has
none.

It needs no setup: the package brings whisper.cpp and its multilingual base
model (`/usr/share/omaedit/whisper/ggml-base.bin`), which finds the language
by itself. It runs on every core of the processor, or on the graphics card
the editor draws with when there is one (the processor takes over if the
card's run fails). **Preferences ▸
Media ▸ Transcription** can name another whisper.cpp-compatible command, a
model of your own (while that file is there it is used; otherwise the
packaged one is) or a language (`auto` finds it); they are the
`transcriptCommand`, `transcriptModel` and `transcriptLanguage` settings.

### Beat map

**Map Beats** (the Clip menu, a timeline clip's right-click menu, and a
clip's menu in the browser) finds the beats in a recording's music: its
tempo, the time of every beat, and the bar (four beats, counted from the
beat the music leans on). It runs in the background (it is in
Background Tasks, where it can be cancelled) on the file's own sound, a
picture clip's included, and writes the result onto the library clip; the
project keeps it, and the media and the timeline are never changed.
**Clear Beat Map** removes it. Mapping is not an undo step.

The beats are in the recording's own time, so every timeline clip made
from it has the ones it plays, through trims, slips, moves, cuts and
changes of speed. On the Edit timeline and on the Audio page they are
lines down every track, over the clips, so a cut on any track can be laid
against a beat: solid on the music's own clip and lighter across the
others, the first beat of each bar wider and capped at the top of the
music's clip. Zoomed out, where the beats would run together, only the
bars are drawn, then every fourth bar, then every sixteenth, so the lines
are never closer than eight pixels and never gone. **View ▸ Show Beat
Map**, or the button beside Snapping in the timeline's toolbar, hides and
shows them (the choice is kept); while they are hidden nothing snaps to
the beats. A map made before the bar was found has beats and no bars:
**Map Beats** again gives it them. The Inspector gives the tempo beside the clip's length (as
the clip plays it: half at half speed), and the Media page lists it with
the clip's audio facts. With snapping on, the playhead, a moved or
trimmed edge and the blade snap to beats as they do to markers;
**Previous Beat** and **Next Beat** (command palette; no keys of their
own) walk them. Speech, effects or a held tone have no steady beat: the
status line says so and nothing is stored. A recording shorter than
eight seconds is not mapped.

### Playback

`Space` play / pause, `J` `K` `L` shuttle, `←` `→` step a frame, `↑` `↓`
jump between edit points, hover the ruler to preview (`S` toggles hover preview),
`N` toggles snapping.

## The Media workspace

The **Media** tab brings footage into the project, organises and logs it,
and prepares playback before editing.

- **Storage** lists mounted local, removable and network drives with free
  space, and, dimmed, drives that are plugged in but not mounted: click one
  to mount it (the browser then shows it), or right-click a removable drive
  to mount, unmount or eject it. A drive the project still uses stays
  mounted, and an encrypted drive must be unlocked from the desktop first.
  Add, rename and reorder favourite folders; create nested bins;
  move clip references between bins; keep custom searches as Saved Searches.
  The project's timelines sit above the clips: double-click one to edit it.
  Projects can be saved even before anything is placed on the timeline.
- **Import Media** accepts files, recursive folders and file-manager drops.
  Video and audio go through FFmpeg; stills include PNG, JPEG, TIFF, WebP,
  OpenEXR and PSD (the picture as saved; see below for its layers). Every
  still arrives at its own size as a 15,000-frame clip (ten minutes at
  25 fps); set In/Out or trim it to the length you want.
  **File > Split Photoshop Layers**, with a PSD selected in Project Media,
  brings in each pixel layer as its own still: the size of the whole
  document so stacked layers line up, with its transparency, mask and
  opacity. The stills are PNG files in a folder named after the document
  beside it (`poster layers/01 Back.png`, …, bottom layer first), plus
  the whole picture with exact soft edges. Splitting the document again
  writes a new folder (`poster layers 2`) and leaves the first as it was.
  Hidden layers are included.
  Blend modes, layer effects, adjustment layers and smart filters are not
  rendered, and text comes in as a picture; a message says how many
  layers that touches. 8-bit and 16-bit RGB, greyscale and CMYK documents
  are read.
  Animated WebP uses FFmpeg 9, retains transparency and frame timing, and
  plays one animation cycle; seeking and retiming rebuild its frame canvas.
  Static WebP keeps the normal still-image behaviour.
  Choose leave in place, copy, or move after verified copy, with hierarchy,
  filename patterns (`{name}`, `{index}`, `{date}`), tags, production metadata
  and an optional input LUT/space. Duplicate checks use path, size or SHA-256.
  Equal size alone does not establish identical content.
  Importing from a removable drive (**Import from This Drive…** on its
  right-click menu, or files on it added or dropped) starts on copying to
  this computer: into the folder the last copying import used, else the
  saved project's `Media` folder, else `OmaEdit Media` in your Videos
  folder. The folder is made if it is not there, a folder on the drive
  itself is refused, and the clips are the verified copies, so the drive
  can be ejected straight after.
- **Clone** copies a folder/card to one or two destinations and verifies
  each file with SHA-256. Existing different files are never overwritten.
  Common camera-card folders are recognised. **Watch Folders** import in
  place after a file's size and modification time stay unchanged across two
  scans, five seconds apart; each watch belongs to the project.
- **Tasks** shows background import, clone, verification, relink search,
  proxy and CSV work, with progress, cancellation, retry and exportable
  reports. Reports last for the session; watch folders keep their latest scan.
- **Browser** offers grid, list and filmstrip views, multi-selection,
  sorting including scene/number, and searches across logging, custom fields
  and available transcripts. **… ▸ Filter Media** adds resolution, frame-rate,
  proxy, offline and date rules. **Save Search…** keeps rules as a Saved Search.
  Hover preview and optional sound are toggled in **…**; dragging a filmstrip
  sets its source range. Right-click to duplicate a reference, name a subclip,
  stack related references, reveal, relink, insert or remove. Auto Stack groups
  numbered filenames from the same folder; stacks organise references rather
  than turn numbered stills into a new playable sequence.
  Cards and list rows give video as 1080p or 4K, and a still or an upright
  clip at its full size (1080×1920); a sound file's card shows its waveform.
  Frame rates read as written (29.97) and file sizes in kB, MB and GB as the
  Export page gives them. A filmstrip keeps each picture at the clip's shape.
- **Source Preview / Clip Log** supports timecode In/Out, named subclips,
  editable coloured markers with notes and previous/next navigation, keywords,
  production fields and stereo waveform seeking. Source marks stay in recorded
  picture units; placement converts them to the timeline rate, including clips
  with an interpretation rate. Filmstrips, hover preview and source timecode use
  recorded pictures; browser duration labels and sorting use interpreted playback time,
  and a subclip reads as long as its range.
  Edit-page drag previews show the placed duration at the project's rate.
  **Gang Source and Timeline**
  links positions with their current offset. The viewer offers Fit/Fill and
  25–200% zoom; its **…** menu saves PNG snapshots, adjusts monitor volume/mute
  and opens full-screen viewing (Esc to exit). **Compare Source Clips** opens
  two source views, with optional linked positions in recorded time and audio from A.
- **Metadata** edits compatible fields across the selection and displays
  available camera/exposure, codec, pixel-aspect and audio facts. Audio bit
  depth is the depth the file stores (a 24-bit WAV reads 24 bit); compressed
  sound such as AAC or MP3 stores none. A sound file has no Video section. Input `.cube`
  LUT and Rec.601/709/2020 interpretation overrides apply to preview, timeline
  and export. Custom text/number/boolean fields belong to the project.
  **… ▸ Import / Export Metadata CSV** round-trips logging and custom fields.
  Import matches `libraryId`, or a unique `path`; invalid rows are reported
  without changing that row. Custom columns use `custom.key`.
  The inspector's **All Metadata** group lists every tag the file's container
  and streams carry, exactly as written. CSV export includes them as
  `source.tag` columns for reference; they are never written back on import.
- **ProRes RAW** uses FFmpeg 9's 16-bit sensor frames and camera colour
  metadata. Bilinear debayering applies sensor black/white levels, As Shot
  white balance, the camera-to-XYZ matrix and scene exposure gain. **Input
  Settings** adds RAW exposure (−6 to +6 stops), red/blue white-balance
  multipliers (0.25–4, relative to As Shot), and **As Shot** reset. Settings
  follow source references and persist in the project. Changing RAW settings
  switches back to the original; regenerate a proxy to bake the new settings.
  The ordinary viewer and proxies develop to Rec.709. Enable OpenColorIO for
  floating-point scene-linear grading and highlight values above display white;
  RAW camera colour is already linear and bypasses the project's input gamma.
  RAW decoding/debayering currently uses the CPU. Missing camera colour
  metadata is rejected rather than interpreted as ordinary RGB video.
  Support also depends on FFmpeg's decoder: FFmpeg 9.0.1 cannot decode
  ProRes RAW files with Bayer pattern 3.
- **Reframe 360° Video…** in the clip menu creates and imports a ProRes 422 HQ
  copy with a fixed view direction. Choose equirectangular or dual-fisheye
  input, 1080p/2160p output, yaw, pitch, roll and horizontal field of view.
  Auto uses Vulkan and retries on the CPU if unavailable; CPU is also selectable.
  Timing and audio are retained, and an existing destination is never overwritten.
  Develop RAW footage to an ordinary ProRes clip before using this operation.
- **Create Proxies** provides H.264 540p/720p and ProRes Proxy 1080p presets,
  destination and naming controls. Right-click to attach, detach, replace or
  reveal a proxy. **Original / Proxy / Preferred** controls playback with
  fallback when media is unavailable. A proxy already in the folder under
  the same name is used as it is when it fits its clip (so the same footage in
  another project needs no second encode). Proxies must match source frame count
  and frame rate. Export resolves originals, including nested sequences.
  ProRes proxies offer Auto (Vulkan encoding with CPU fallback) or CPU.
  Reports record the encoder actually used. ProRes RAW proxies develop the
  camera colour and current RAW input settings before encoding; they are
  Rec.709 viewing copies, while export continues to use the RAW originals.
- **APV** (Advanced Professional Video) footage, such as phone recordings in
  MP4 or MOV and APV in MKV, imports like any other clip: 4:2:2 and 4:4:4 at
  10 or 12 bits, with its embedded sound. FFmpeg 9 decodes it on the CPU, so
  4K APV benefits from proxies on slower machines. A raw `.apv` stream has no
  frame rate or sound; wrap it in MOV or MKV first.
- **Relink Media** lists missing sources with their last known paths. Browse
  for one replacement to search nearby automatically, or choose a search folder.
  Combine filename, relative path, file size, frame count, timecode and SHA256
  matching; every selected rule must agree. Relative paths use the original and
  replacement base folders. Timecode and checksum need recorded source facts.
  Skip sources, select individual candidates or **Select All Unique Matches**,
  then apply the reviewed selection. Ambiguous matches require an explicit choice.
  Search and validation support cancellation and keep separate result summaries.
  Applying rechecks the chosen rules and requires matching frame count/rate;
  failed validation leaves project references unchanged. Relinking preserves
  logging, timeline ranges, nested clips, sequences and undo/redo references.
  Missing footage reopens as offline placeholders; export reports missing
  originals instead of rendering proxies/placeholders.
- **Sync** opens multicam creation with waveform, timecode, marker or clip-start
  alignment (see the multicam section above).

The **Cloud Storage** row registers an already-mounted cloud/network folder,
including through a JSON provider manifest. Account sign-in and mounting use
external storage tools. Version 1 supports the `mounted` adapter:

```json
{"version":1,"id":"team-rushes","name":"Team Rushes","adapter":"mounted","kind":"network","root":"/mnt/team/rushes"}
```

Bins, logging, watches, stacks, custom schemas and source/proxy preferences
are stored with the project. Favourite locations and registered providers are
application preferences. Source media, LUTs and proxies remain external files.

## The Colour workspace

`Ctrl+Shift+C`, or the Colour tab, grades **the clip under the playhead**.
The clip strip and previous/next buttons move between shots. Grade edits
support Undo/Redo and save in the project.

The page is for the colour work an edit needs: balancing a shot, setting its
contrast and saturation, shaping it with curves, a look from a LUT, and a
correction kept to part of the picture with a shape or a picked colour. A
finishing grade belongs in a grading application: **File ▸ Export Timeline**
hands the edit over as OTIO, EDL, XML or AAF.

- **Viewer and colour management:** the **Colour** button on the viewer says what the
  clip is set to and opens the settings. A line at the top spells out the whole path,
  for example Sony S-Log3 → graded in ACEScct → ACES 2.0 → computer monitor. **Standard
  video** suits phones, most cameras and screen recordings; **Log camera footage** sets
  ACEScct and ACES 2.0, and leaves you to say what the camera was set to.
  **Shot as** is what the clip was recorded as: standard video, a camera format (Apple
  Log, ARRI LogC3 and LogC4, Blackmagic Film Gen 5, Canon Log, Log 2 and Log 3, DJI
  D-Log, Fujifilm F-Log and F-Log2, Panasonic V-Log, RED Log3G10 and LogFilm, Sony
  S-Log3), ACES, or other primaries and curves. **Grade in** picks the space the clip's
  layers work in: linear Rec.709, ACEScct, ACEScc, ACEScg or Rec.709 Gamma 2.4. **Use on**
  gives both to every clip from the file or on the timeline, in one undo. **Screen** is
  what you are looking at. **Tone map** (None, ACES 1.3, ACES 2.0) brings bright light
  down to the screen; with None, what it cannot show clips. The Export page's Tone mapping
  follows the viewer unless you choose otherwise, so the file matches what you graded.
  Layers, effects and working storage stay in linear Rec.709 RGBA32F. Export applies
  the output encoding once after composition. The viewer's display transform and
  RGB/R/G/B/Y isolation leave stored pixels and exports unchanged. Managed stills use
  FLOAT OpenEXR; explicit PNG exports use sRGB. Highlight shows the selected layer's
  matte. There is no automatic recognition of camera formats or calibrated monitor
  surface.
- **Layers:** a clip's grade is a stack of layers beside the viewer, the last
  one applied at the top. Each layer has its own wheels, curves, masks,
  colour pick and LUT: click a layer and the tools below edit it. `+` or a
  right-click adds a layer above the selected one; the eye hides or shows
  it; drag a layer up or down to change the order; double-click or `F2`
  renames it. A layer is named by its place in the stack until you name it.
  Grades made earlier with parallel or layer mixers, channel splits or
  groups keep rendering as they did, and those corrections show in the
  stack with a note of what they are. A grade holds up to 128 layers.
- **Copy/paste and presets:** `Ctrl+C/V` copies/inserts an independent layer;
  `Ctrl+Alt+C/P` copies/replaces the clip's whole grade. Layers menu ▸ Presets
  saves a layer for reuse; `.omanode` files import/export presets.
- **Primaries:** Lift/Gamma/Gain/Offset Y/R/G/B wheels, temperature, tint,
  exposure, contrast/pivot, saturation, hue, highlights/shadows/whites/blacks
  and positive/negative Mid Detail. Highlights scales the whole picture like
  Gain and Shadows raises it like Lift. Whites and Blacks move the white and
  black points: raising Whites brightens the picture towards clipped white,
  raising Blacks darkens it towards crushed black, and negative values pull
  the ends back in. Tint balances green against magenta across the whole
  picture. Numeric entry, dragging and reset are available. **Auto Balance** estimates neutral gains from this shot's frame
  and adds an editable correction in one undo step; review intentionally
  coloured scenes before keeping its estimate.
- **Reframe to Fit** (Modify): refits clips shot in another shape to the
  timeline's, such as 16:9 footage in a 9:16, 4:5 or square timeline. Each clip
  is scaled to fill the frame and moved to follow what stands out in it, the
  subject against its background: position keyframes once a second, smoothed so
  the framing glides, and never far enough to show an edge. A subject that hardly
  moves gets one still framing. It works on the selected clips, or with none
  selected on every clip whose shape differs; it runs in the background (Stop
  Reframe to Fit ends it), one undo step per clip, and needs ffmpeg. Adjust the
  keys afterwards like any others; retimed clips are left alone.
- **Match** (Modify ▸ Match): select a clip, put the playhead on the frame you
  like and choose **Set Match Reference**. Then select another clip and choose
  **Match Colour** to add a "Match Colour" correction that brings its colour
  balance and brightness to the reference frame's (the frame used is the one
  under the playhead, or the clip's middle), or **Match Audio** to move its
  volume so it sounds as loud as the reference (EBU R128 loudness of the parts
  each clip shows, as their volumes stand; volume keyframes move together).
  Each is one undo step. Match Audio needs ffmpeg.
- **Precision:** every correction — primaries, curves, the hue curves,
  masks and their soft edges, and layer LUTs — is worked
  in floating point on a 16-bit picture, so a graded clip in a 10- or 12-bit
  delivery (ProRes, APV, H.265 10-bit, DNxHR HQX, FFV1) keeps the gradation
  of its source. The viewer grades from the same 16-bit picture during
  playback, so it shows exactly what the export holds, rounded to 8 bits.
  Custom RGB curves are smooth curves that never turn back: points that
  rise give a curve that rises, and it never overshoots a point.
- **Bypass and comparison:** Bypass in the Primaries tab turns this clip's
  grade off while you look; the eye above the viewer (or Layers menu ▸ Bypass
  All Grades) turns off every grade and shows GRADES BYPASSED. Split Screen
  and the Comparison list show the graded and ungraded picture side by side,
  or as a before/after wipe.
- **Curves:** custom Master/R/G/B, Hue vs Hue/Sat/Luma, Luma vs Luma and
  Sat vs Sat. The hue curves leave greys alone, including the grain on grey
  footage. Playback and export use the same pipeline. On a wide panel the
  curves and the masks share one page, **Curves and Masks**; on a narrower
  one each has its own tab. Every tab fits a 1280-wide window; a tab still
  bigger than its panel shows its scroll bars, and the mouse wheel scrolls it
  (sideways with Shift, or when only the width runs out). With colour
  management on, saturation changes match the unmanaged result closely, but a
  hue shift on a strongly saturated colour can land a little differently (a
  red pushed towards orange may come out slightly darker), because the managed
  engine turns colours in linear light.
- **Colour pick and shape masks:** The colour pick sits on the Primaries page: beside the
  wheels from 1920 pixels wide, below them on a narrower panel, where its arrow
  hides it and the wheels take the room back. HSL/RGB Pick, Add and Subtract buttons sample the
  viewer (Ctrl adds, Shift subtracts). Ranges, softness, invert, blur and
  grow/shrink refine the matte. Add multiple circle, rectangle, polygon,
  diamond or gradient windows and combine them with Add/Intersect/Subtract.
  Polygon vertices can be added/removed/numerically edited or dragged in the
  viewer. On a layer with both, the correction lands only where the shape and
  the colour pick agree.
- **LUTs:** beside Auto Balance, search `.cube`/`.3dl` files, folders and
  favourites. Current/Preview isolates this clip, including its grade, trims
  and retiming. Intensity blends continuously from 0–100%. Apply changes the
  selected layer in one step; Cancel discards the preview. Folder, Refresh and
  Locate LUT controls handle discovery and replacement. Export refuses missing
  or unreadable active LUTs/mattes. These assets remain external files.
- **Scopes:** waveform, parade, vectorscope and histogram in single/dual/quad
  layouts, with scale, channels, intensity, graticule, range and pause. A background
  CPU worker measures every pixel of the delivered preview, with up to 25 updates
  per second. Fixed density gain prevents brightness pumping between frames;
  histogram height shows a fixed logarithmic fraction of the frame. Float and
  16-bit input retain their precision and highlight/shadow excursions. Optional
  8/10/12-bit quantisation is in Scope Settings; scale units alone do not quantise.
  Scopes use the project output signal and its Rec.601/709/2020 colour matrix,
  before the monitor/view transform. IRE is normalised black-to-white (0–100);
  integer scales are **full-range RGB equivalents**, not encoded YUV code values.
  The range switch selects 0–100% or −10–110%; values beyond the plotted range
  appear in amber. Pause holds the last reading. These measure the current preview
  resolution, including any active proxy, rather than inspecting encoded file data.
  Rapid changes keep the latest pending frame; final paused-frame and scope-setting
  updates complete automatically. Hiding the panel or leaving Colour stops scope
  work. Pop out the scopes to a movable window
  for a second monitor; closing it docks them. Layout and geometry persist.
- **On the Edit page:** **View ▸ Viewer ▸ Video Scopes** (Ctrl+7) shows the same
  scopes beside the viewer, starting with a waveform. Their layout and which
  scopes they show are their own; scale, channels, intensity and range are shared
  with Colour, which brings back its own settings when you open it. Scopes are
  measured while either page shows them and not paused.

## The Audio workspace

The **Audio** tab (Ctrl+Shift+U) is the mix room: every audio track in
the project, grouped by role, with a channel strip, real inserts, meters
and an EBU R128 loudness meter. Everything on it runs through the same
engine that plays and renders the timeline — a fader or an EQ you set
here is what the export carries.

- **Sound**: the page works on the sound you have imported into the
  project. Import music, effects and recordings on the Media or Edit page
  and place them on the timeline there; they then appear here on their
  audio tracks. The app does not search your music folders.
- **Viewer** (left): the programme picture with a transport, timecode,
  a loop button for the In/Out range and the master's stereo meter. Mute,
  −20 dB Dim and Mono change monitoring without changing the render.
- **Mixer**: a strip for every track, named bus and Master. Track outputs and
  multiple pre/post-fader sends really route through the bus insert chains;
  feedback routes are refused. Peak/RMS meters show the decoded output;
  compressor gain reduction comes from its envelope. Faders, pan, mute,
  solo, arm, reset, insert bypass/reorder and compact/expanded banks work
  on the same mix used in exports.
- **Audio tools** (toolbar settings icon): edit volume/pan automation points
  with linear, smooth or hold curves. Tracks support Read, Touch, Latch,
  Write and Off; Touch returns to the old curve when released, Latch holds
  until stopped, and Write records throughout playback. Bus/master curves
  support Read/Off. The Routing tab manages buses and sends; Clip provides
  duplicate, pitch-preserved speed/reverse, fade lengths and curve shape.
- **Effects** tab: the selected strip's insert chain — add an Equaliser,
  Compressor, Gate / Expander, Limiter, De-Esser, Noise Reduction,
  Hum Removal, High-pass or Low-pass; reorder, bypass, reset or remove each; every
  parameter is a slider in dB / ms / Hz; every insert has wet/dry Mix. Presets hold whole chains (Dialogue Clean, Music Glue,
  Ambience Bed, Master Limiter, and your own saved ones).
- **ADR** tab: a cue list — character, line, notes, In / Out, pre- and
  post-roll, the track it records onto, a status dot. Record rolls
  the transport from the pre-roll, records the chosen input and stops
  after the post-roll; recordings list under the cue with a rating and a
  Review button; Promote puts the chosen recording on the timeline at the
  cue's In, trims off pre/post-roll and marks it approved. Use a recording for
  the In/Out selection to comp several recordings into a cue. Promotion/comping
  and their cue metadata undo together.
- **Loudness** tab: integrated, short-term, momentary LUFS, true peak
  (4× oversampled) and loudness range, measured live from the master
  while it plays; choose broadcast, web, music, podcast or dialogue reference
  targets and compare measured LUFS/peak/range against them. This is a
  programme loudness meter; the dialogue reference does not perform dialogue
  gating. Analyse Timeline or Selection works offline with cancellation.
  Normalise applies the gain in one undoable edit (only the measured range
  when selected), up to the master fader's +24 dB; if a very quiet mix
  needs more, it says how far short it stays. Normalise does not limit
  peaks: it tells you when the raised true peak will pass the target's
  ceiling (−1 dBTP when a script gives none), so you can add a master
  limiter. Analyse again to check the
  resulting loudness and peak.
- **Inspector** (far right): the selected strip's sections — **Voice
  Cleanup** (a high-pass, noise reduction, de-esser and gentle compressor
  in one violet switch, labelled DSP with a Before / After listen), **Equaliser** (five
  bands on an interactive curve — drag a handle for frequency and gain —
  with gain / frequency / Q fields), **Dynamics** (compressor with a
  gain-reduction meter, plus gate and limiter), **De-Esser**, **Noise
  Reduction** with a live spectrum showing the noise floor, and the
  master's **Loudness** readings.
  Noise Reduction pulls steady background noise down by its **Reduction**
  (12 dB when added) and leaves louder sound alone. It keeps working while
  dialogue or music plays: the noise is still pulled down around the
  louder sound, and the sound itself passes through unchanged, with no
  clicks. **Sensitivity** is
  the noise level it expects (−50 dB). With **Track the noise floor** on,
  as it is by default, it learns the real background within about a
  second, anywhere from 10 dB below to 10 dB above Sensitivity. For
  noisier recordings, raise Sensitivity to the floor drawn on the
  spectrum.
- **Timeline** (bottom): tracks grouped under Dialogue / Ambience / Music
  / Effects / Foley / Room Tone headers (click a header to fold the
  group). Each control block has the badge (double-click renames), M / S,
  record arm, input monitoring with device passthrough, a
  mini fader with its dB, and a context menu for role, colour, order,
  lock and removal. Clips draw their waveform in the role's colour with
  fade ramps and the gain band: drag the flat line for clip gain, double-
  click it for a keyframe, drag points, Alt-click to remove one. Select,
  Trim, Blade, Slip, Range, Hand and Zoom tools; snapping to clip edges,
  markers, the playhead and the range; the ruler seeks on click and
  scrubs (with sound) on drag. The **Ruler** choice sets how fine sound
  moves: Timecode (the sequence's own timecode, as on the Edit page) and
  Seconds move it a whole picture frame at a time, Samples and
  Milliseconds move it finer. Ctrl+wheel zooms, Shift+wheel scrolls; the
  Master row at the foot carries its own mute, level and meter. **Samples**
  zooms to individual signed stereo samples; visible regions are decoded at
  the source sample rate on background workers with bounded memory.
- Recording lands in `Recordings/` beside the project (or `~/Music/OmaEdit
  Recordings` for an unsaved one); the input device comes from the strip.
  Track mix settings, automation, buses, master and ADR cues save with the project.
- `Backend.audioServices` is the versioned capability boundary for cleanup
  and future external audio providers. The built-in cleanup is DSP; no
  learned model or third-party plug-in host is bundled. The Effects tab
  reports the plug-in host placeholder as unavailable.

## The Export workspace

The **Export** tab turns the cut into files. Everything on it is real:
the presets encode with the settings they show, the pre-flight checks
read the disk and the timeline, and the queue renders through the same
engine that plays the timeline.

- **Render Presets** (left): five built-in groups — Master (ProRes 422 HQ,
  ProRes 4444 with alpha, APV 4:2:2 10-bit, APV 4:4:4:4 12-bit with alpha,
  H.265 10-bit, DNxHR HQX, lossless FFV1 at 10-bit 4:2:2), Web
  (YouTube 2160p / 1080p, YouTube Shorts, Vimeo, H.265, WebM VP9, AV1),
  Broadcast (ProRes 422 HQ 1080i and 1080p at the timeline's rate, MPEG-2
  50 Mb/s 4:2:2 1080i, DNxHR HQ, HDR10 H.265 4K — this
  one needs colour management, see colour space below), Social (Instagram
  Reels, Feed 4:5 and 3:4, square and Stories; TikTok; Facebook Reels, Feed
  4:5 and 1080p; X landscape and vertical; LinkedIn 1080p and square) and
  Audio Only (AAC, MP3, WAV 24-bit, FLAC).
  The YouTube, Vimeo and Social presets are made the way each site asks:
  H.264 High in an MP4 that starts playing before it has downloaded,
  standard video colour (Rec.709), the edit's own frame rate up to 60
  (a faster edit is delivered at half or a third of its rate), AAC sound
  at 48 kHz stereo, and each site's own bitrate and size caps. They also
  **level the sound** to −14 LUFS with true peaks under −1 dBTP, the
  level streaming and social sites play at: the render is measured first,
  and its sound checked again as the finished file will play it (the
  sound codec rebuilds sharp peaks a little louder than it was given), so
  the file comes out right whatever the mix, and the project's own mix
  is never changed. Turn **Level sound** off in Render Settings to keep
  the mix exactly as it is. With one of these presets chosen, the
  pre-flight checks hold the edit to that site: its length and file size
  limits, the frame rates it takes and HDR.
  When the delivery's shape differs from the edit's (a 16:9 edit to a
  9:16 preset, or any size you type), the checks ask how the picture
  fills the frame, with a choice on that line:
  **Make a 9:16 copy and reframe it** copies the timeline at the
  delivery's size, makes the copy the one you are delivering and runs
  **Modify ▸ Reframe to Fit** on its clips, so each shot fills the frame
  and follows its subject (wait for it to finish before rendering);
  **Blurred backdrop** shows the whole picture over a blurred, dimmed copy
  of itself that fills the frame; **Fill and crop** fills the frame from
  the middle of the picture; **Black bars** shows the whole picture with
  bars. Until you choose, the picture is fitted with black bars. The
  choice belongs to that preset: another preset starts with its own, and
  coming back to the first shows what you chose there (until you quit).
  Built-ins are read-only; **New Preset** saves the current settings as a
  custom preset, and a preset's ⋮ menu favourites, duplicates, renames,
  updates, exports (a JSON file) or deletes it. **Import…** reads exported
  presets back. Search matches names; the funnel narrows to favourites,
  video, audio-only or custom.
- **Viewer** (centre): the programme picture with transport, and an **A/B
  range bar** — drag the handles (or press I / O on the Edit page) to
  render only part of the timeline; × clears it. Range Duration shows the
  span that will render.
- **Render Settings** (right): file name with tokens (`{project}`,
  `{sequence}`, `{preset}`, `{resolution}`, `{fps}`, `{date}`, `{time}`,
  `{version}` — the version steps past files that exist; the default is
  `{project} - {preset}`, and every name is made tidy: characters a file
  system refuses (and `/`) become `-`, `×` becomes `x`, and runs of spaces,
  underscores and hyphens close up, so "X / Twitter 720×1280" gives
  `Untitled - X - Twitter 720x1280.mp4`), save location
  (with a folder picker), format (MP4, MOV, MKV, WebM, or M4A / WAV / MP3 /
  FLAC for audio), codec (H.264, H.265, ProRes, DNxHR, VP9, AV1, MPEG-2,
  FFV1, APV — only those the format can hold), resolution, frame rate, quality
  (Best / High / Medium / Low, or a bitrate), colour space (Rec.709,
  Rec.601, sRGB, Rec.2020, HDR PQ, HDR HLG — all but Rec.709 and Rec.601
  need colour management: switch the colour engine to OpenColorIO in the
  Colour workspace first), audio codec / bitrate /
  sample rate / channels, subtitles (burn in, embed as a track, or a
  sidecar .srt), metadata (title, artist, copyright…) and hardware
  acceleration (NVIDIA NVENC through the engine, VAAPI through ffmpeg;
  each H.264/HEVC/AV1 encoder is tested independently). VA-API can choose a
  tested device for the current codec automatically, or use an explicit render
  node. A tiny probe does not guarantee all formats and resolutions. Optional software
  fallback retries a hardware failure once and keeps the reason in the log. The ⚙ beside
  Quality opens the advanced encoder settings: rate control and
  bitrates, profile and level, keyframe interval, bit depth, scan type,
  pixel aspect, data levels, alpha, and HDR mastering metadata.
  **Vulkan ProRes** is available for progressive Proxy, LT, 422 and HQ exports,
  using a lossless intermediate followed by GPU encoding. 4444, alpha and
  interlaced ProRes use software. Hardware probing tests the ProRes encoder,
  and the same optional software fallback applies.
  **APV** is a mastering codec in MOV, MP4 or MKV. Its profile (in the ⚙
  settings) picks 4:2:2, 4:4:4 or 4:4:4:4 with alpha, at 10 or 12 bits;
  quality or a bitrate sets the size. APV is encoded in software by the
  OpenAPV encoder that ships with OmaEdit. It is not a web upload format:
  use H.264, H.265 or ProRes for that.
- **Outputs → Tracks, stems and file splitting…** selects included tracks,
  maps them to output channels, splits a range into timed parts and adds
  separate 24-bit WAV stems. Each part/stem is a separate queue job and the
  size estimate includes the batch.
- **Stems** is one choice in that box: **None**, **One per track** or **One
  per category**. By category, every sound category in use on the timeline
  gets a WAV of its own beside the main file: `Name_Dialogue.wav`,
  `Name_Music.wav`, `Name_Effects.wav` and one for each category of your
  own (set a clip's under **Modify ▸ Category**). A sound with no category
  goes to a stem of its own, `Name_Uncategorised.wav`, made only when there
  is such a sound. A stem is as long as the render, silent where its category is,
  and starts at the same timecode as the main file. It always has its clips',
  tracks' and buses' processing; master processing is a switch. Without it,
  the stems added together are the main mix made without master processing.
  A category switched off in the timeline list's **Categories** tab makes no
  stem; nor do picture categories. The box says which stems will be made and
  what the files will be called. From the command line: a render `spec` with
  `"stems": true, "stemsBy": "category"`.
- **Render** picks what to render: the full timeline, the In/Out range,
  the selected clips as one span, or each selected clip as its own file
  (with handles). Video and audio can be switched off individually; alpha
  is offered where the codec carries it.
- **Validation Summary**: green ticks for what passed, amber warnings the
  job may go ahead with, red errors that block it — the save location and
  its write access, offline media, effects whose plugin is missing,
  format/codec combinations that cannot work, and the estimated file size
  against the free space. A 10- or 12-bit delivery also warns about each
  clip with a video effect that works in 8 bits (all but the Gaussian, box
  and directional blurs and Strobe, which keep 16 bits): grades keep the
  delivery's precision, but that clip does not.
- **Render Queue**: **Add to Queue** turns the settings into a job;
  **Render All** runs the queue in order. Every row shows status, progress,
  its render time (elapsed, and what is left while it runs) and the
  destination; the ⋮ menu pauses,
  resumes, cancels, retries, duplicates, reorders, removes, opens the
  output, reveals it in the file manager, or shows the encoder log (kept
  for failed jobs). Pausing a render keeps what is written and picks up
  from the next frame. The picture pieces are joined losslessly in the
  delivery's own format, and the sound is rendered once more in one pass
  before the join, so there is no gap or click where it paused. The queue
  saves the queued project/mix and nested-sequence state, so later edits
  cannot change a waiting job. Snapshots and unfinished jobs survive a
  restart; source media must remain available. The header menu clears finished jobs and can
  suspend or shut down the computer when the queue finishes.
- Closing the window while a render runs asks: keep rendering with the
  window hidden (it returns when the queue drains), cancel and quit, or
  stay. Finished and failed jobs appear in the Info area and message history.
- **File ▸ Export Movie…** (Ctrl+E) still works from the Edit page — it
  queues one job with the last quick preset and runs it. A different frame
  rate re-times through ffmpeg after the render; everything else encodes
  in one pass.

## Hotkeys

Press `?` in the app for the complete list. The essentials:

| Key | Action |
|---|---|
| `Ctrl+I` / `Ctrl+O` / `Ctrl+S` | Import media / open project / save project |
| `Ctrl+Alt+J` / `Ctrl+,` | Timeline settings / preferences |
| `=`, or a number-pad digit, `+` or `-` | Type a timecode to go to, or `+`/`-` and an amount to move by (Enter goes, Esc leaves) |
| `Ctrl+E` / `Ctrl+Shift+E` | Export movie with the quick preset / open the Export workspace |
| `Ctrl+Shift+U` | Open the Audio workspace |
| `E` `W` `D` | Append / insert / overwrite onto the destination track |
| `Q` / `Shift+Q` | Place on top / place audio on top |
| `Ctrl+B` | Blade at the playhead |
| `Ctrl+Shift+B` | Blade all unlocked picture/sound tracks at the playhead |
| `A` `B` `Y` `U` `H` `Z` | Tools: select, blade, slip, slide, hand, zoom (`Esc` back to select) |
| `T` | Trim tool on / off |
| `Ctrl+J` | Join clips (heal a blade cut) |
| Wheel over the track headers | Scroll the track stack up and down when it overflows |
| `Shift`+wheel | Scroll the track stack when it overflows; over a track header, make that track taller or shorter |
| `Backspace` / `Delete` or `Shift+Backspace` | Delete leaving a gap / ripple delete (the whole selection) |
| `Ctrl`-click / `Shift`-click / drag on empty space | Add a clip / a run of clips / a marquee to the selection |
| `Ctrl+A` / `Ctrl+Shift+A` | Select every clip / none |
| `Ctrl+Shift+L` | Linked selection on / off (`Alt`-click picks one half) |
| `Ctrl+C` / `Ctrl+V` / `Ctrl+D` | Copy / paste at playhead / duplicate |
| `Shift+X` | Extend edit to playhead |
| `Alt+[` / `Alt+]` | Trim start / end to playhead (Edit workspace) |
| `Alt+,` / `Alt+.` | Roll the edit left / right one frame (`Shift` for ten) |
| `Ctrl+Shift+X` | Roll the edit to the playhead |
| `Alt+←` / `Alt+→` | Nudge a frame (`Shift` for ten) |
| `Shift+R` | Replace with the selected library clip |
| `Ctrl+T` | Cross dissolve |
| `Ctrl+[` / `Ctrl+]` | Fade in / fade out |
| `Alt+W` | Insert empty space |
| `Ctrl+Z` / `Ctrl+Shift+Z` or `Ctrl+Y` | Undo / redo |
| `Ctrl+Q` | Quit |

Projects are saved as `.omaedit` files: the edit as OmaEdit holds it (tracks,
clips, grades, mix and media settings). Projects from earlier versions open
when they carry that record; MLT documents from other tools do not open.
Export renders H.264 + AAC, ProRes 422, or audio only.

