# Third-party notices

OmaEdit's own code is under the [GNU General Public License, version 3 or
later](LICENSE). The package also carries the components below, each under its
own licence.

## FFmpeg and the libraries beside it

The package carries its own FFmpeg 9.1 for OmaEdit alone, in
`/usr/lib/omaedit/ffmpeg`, built by `bin/build-ffmpeg` under GPL v3 from a pinned
commit, with only what OmaEdit uses. Everything else on your computer keeps the
system's FFmpeg.

This FFmpeg is modified by one small patch of OmaEdit's own, kept in
`pkgbuild/ffmpeg`.

These travel beside it, as Arch Linux packaged them:

| Library | Licence |
| --- | --- |
| x264 | GPL-2.0-or-later |
| x265 | GPL-2.0-or-later |
| SVT-AV1 | BSD-3-Clause and AOM patent licence |
| libvpx | BSD-3-Clause |
| dav1d | BSD-2-Clause |

Their licences are installed in `/usr/share/licenses/omaedit/ffmpeg/`, in a
folder for each library. The file `SOURCE` there names FFmpeg's repository, exact
commit, patch and configuration, and each library's exact version with the
address of Arch Linux's recipe for building it.

## The libraries beside the program

These travel in `/usr/lib/omaedit/lib`, as Arch Linux packaged them, so that an
update of the system's own copies cannot stop OmaEdit starting:

| Library | Licence |
| --- | --- |
| OpenCV | Apache-2.0 |
| OpenColorIO | BSD-3-Clause |
| OpenEXR | BSD-3-Clause |
| Imath | BSD-3-Clause |
| OpenTimelineIO | Apache-2.0 |
| yaml-cpp | MIT |
| pystring | BSD-3-Clause |
| minizip-ng | Zlib |
| OpenJPH | BSD-2-Clause |
| Rubber Band | GPL-2.0-or-later |
| vid.stab | GPL-2.0-or-later |

OpenCV, as Arch Linux builds it, has code from other projects inside it, each
under its own licence. In the OpenCV libraries OmaEdit carries these are Intel's
Integrated Performance Primitives for OpenCV (Intel Simplified Software
License), ittnotify (BSD-3-Clause, or GPL v2 at your choice), SoftFloat (BSD),
the OpenCL headers (MIT), protobuf (BSD-3-Clause), flatbuffers (Apache-2.0) and
the Rubik typeface (SIL Open Font License 1.1). The licence texts of all the code
Arch's OpenCV holds are installed with OpenCV's.

Their licences are installed in `/usr/share/licenses/omaedit/carried/`, in a
folder for each library, with a file `SOURCE` that names each one's exact
version and the address of Arch Linux's recipe for building it.

## glslang

The code of [glslang](https://github.com/KhronosGroup/glslang), which compiles
the colour programs OpenColorIO writes into the engine's shaders while OmaEdit
runs, is linked into the program, unmodified, as Arch Linux packaged it. Its
licences (BSD-3-Clause and others) are installed as
`/usr/share/licenses/omaedit/glslang-LICENSE`.

## OpenAPV

APV exports are encoded by OpenAPV's command-line encoder, version 1.1.3.0
(Academy Software Foundation, BSD-3-Clause), built unmodified by
`bin/setup-openapv` and installed as `/usr/lib/omaedit/oapv_app_enc`. Its licence
is installed as `/usr/share/licenses/omaedit/OpenAPV-BSD-3-Clause`.

## Whisper's speech model

Transcription (Inspector ▸ Transcript) runs whisper.cpp over the clip's sound
with OpenAI's Whisper **base** model (multilingual), as the whisper.cpp project
converted it to ggml format and publishes it
([huggingface.co/ggerganov/whisper.cpp](https://huggingface.co/ggerganov/whisper.cpp),
`ggml-base.bin` at commit `5359861c739e955e79d9a303bcbc70fb988958b1`), unmodified.
It is installed as `/usr/share/omaedit/whisper/ggml-base.bin`. The model and the
conversion are under the MIT licence; both licences are installed as
`/usr/share/licenses/omaedit/Whisper-base-model-MIT`.

## Complete source

Every package on the [Releases](https://github.com/28allday/omaedit/releases)
page has its complete source beside it, at no charge, as
`omaedit-<version>-<release>-source.tar.xz`: OmaEdit, FFmpeg at the commit it was
built from, OpenAPV, Arch Linux's recipe and patches for every library above,
the source of each library beside FFmpeg and of Rubber Band and vid.stab, and
the scripts that build them.

## Other components

- The Vulkan headers in `shared/hardware/third_party/vulkan` are the Khronos
  Group's, unmodified, used under the MIT licence
  (`/usr/share/licenses/omaedit/Vulkan-Headers-MIT`).
- The icons are from [Lucide](https://lucide.dev) (ISC licence,
  `/usr/share/licenses/omaedit/Lucide-Icons-ISC`).
- **Remove Background (AI)** uses the weights of MODNet (Zhanghan Ke and others,
  Apache-2.0), converted for OmaEdit's own engine
  (`/usr/share/licenses/omaedit/LICENSE.MODNet`).
- The colour configurations in `colour/` are OmaEdit's own, written for
  OpenColorIO. Camera formats use OpenColorIO's built-in transforms where it has
  them, otherwise the makers' published curves and primaries.

Qt, SPIRV-Tools, the Vulkan loader and the other libraries OmaEdit links are
installed from the Arch Linux repositories as its dependencies, and are not part
of the package. So are whisper.cpp and ggml (MIT), whose `whisper-cli` OmaEdit
runs to transcribe.
