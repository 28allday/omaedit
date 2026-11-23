include($$PWD/ffmpeg.pri)
QT += core gui qml quick quickcontrols2 dbus multimedia

CONFIG += c++20 release link_pkgconfig
PKGCONFIG += libavfilter libavutil
TARGET = omaedit
TEMPLATE = app
include(colour.pri)
include(playbackdecode.pri)
include(previewcache.pri)
include(keyboardnavigation.pri)

HEADERS += \
    src/backend.h \
    src/backend_p.h \
    src/captionformats.h \
    src/exportspec.h \
    src/filepicker.h \
    src/loudness.h \
    src/mediaplayer.h \
    src/timeline.h \
    src/timeline_p.h \
    src/playerworker.h \
    src/playerworker_p.h \
    src/recorder.h \
    src/renderqueue.h \
    src/scopes.h \
    src/stills.h \
    src/soundlibrary.h \
    src/theme.h \
    src/thumbnailprovider.h \
    src/videoview.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/backendaudio.cpp \
    src/backendcaptions.cpp \
    src/backendbeats.cpp \
    src/beatmapper.cpp \
    src/beatmap.cpp \
    src/backendclipsettings.cpp \
    src/backenddecor.cpp \
    src/backenddeliver.cpp \
    src/backendedits.cpp \
    src/backendgrading.cpp \
    src/backendhelpers.cpp \
    src/backendinterchange.cpp \
    src/backendlibrary.cpp \
    src/backendmulticam.cpp \
    src/backendpreferences.cpp \
    src/backendproject.cpp \
    src/backendsequences.cpp \
    src/backendtiming.cpp \
    src/backendviewer.cpp \
    src/captionformats.cpp \
    src/exportspec.cpp \
    src/filepicker.cpp \
    src/loudness.cpp \
    src/mediaplayer.cpp \
    src/timeline.cpp \
    src/timeline_attributes.cpp \
    src/timeline_cliplist.cpp \
    src/timeline_colour.cpp \
    src/timeline_compound.cpp \
    src/timeline_connected.cpp \
    src/timeline_edits.cpp \
    src/timeline_helpers.cpp \
    src/timeline_linkedsound.cpp \
    src/timeline_mixes.cpp \
    src/timeline_multicam.cpp \
    src/timeline_render.cpp \
    src/timeline_sound.cpp \
    src/timeline_soundcuts.cpp \
    src/timeline_tracks.cpp \
    src/playerworker.cpp \
    src/playerworkercaptions.cpp \
    src/playerworkerclipsettings.cpp \
    src/playerworkerdecor.cpp \
    src/playerworkeredits.cpp \
    src/playerworkerexport.cpp \
    src/playerworkergrading.cpp \
    src/playerworkerhelpers.cpp \
    src/playerworkerhistory.cpp \
    src/playerworkermedia.cpp \
    src/playerworkermulticam.cpp \
    src/playerworkerplayback.cpp \
    src/playerworkerproject.cpp \
    src/playerworkerrender.cpp \
    src/playerworkersequences.cpp \
    src/playerworkertracking.cpp \
    src/recorder.cpp \
    src/renderqueue.cpp \
    src/scopes.cpp \
    src/stills.cpp \
    src/soundlibrary.cpp \
    src/theme.cpp \
    src/videoview.cpp

RESOURCES += src/resources.qrc
HEADERS += src/lutfilter.h
SOURCES += src/lutfilter.cpp src/backendluts.cpp

HEADERS += src/mediacatalog.h
SOURCES += src/mediacatalog.cpp

HEADERS += src/mediatasks.h src/psddocument.h
SOURCES += src/mediatasks.cpp src/backendmedia.cpp src/psddocument.cpp

HEADERS += src/mediarefs.h
SOURCES += src/mediarefs.cpp

HEADERS += src/mediacsv.h
SOURCES += src/mediacsv.cpp

HEADERS += src/gradeengine.h
SOURCES += src/gradeengine.cpp
SOURCES += src/backendcolor.cpp
HEADERS += src/windowtracker.h
SOURCES += src/windowtracker.cpp

SOURCES += src/playerworkercolor.cpp
SOURCES += src/playerworkersources.cpp

SOURCES += src/colormaskservice.cpp
HEADERS += src/colormaskservice.h

HEADERS += src/audioengine.h
SOURCES += src/audioengine.cpp
include(audiotiming.pri)

SOURCES += src/playerworkeraudio.cpp src/playbackaudio.cpp
HEADERS += src/playbackaudio.h

HEADERS += src/beatmapper.h src/beatmap.h
HEADERS += src/rendersnapshot.h
SOURCES += src/rendersnapshot.cpp
HEADERS += src/wavepeaks.h src/waveformitem.h
HEADERS += src/beatlinesitem.h
SOURCES += src/beatlinesitem.cpp
SOURCES += src/wavepeaks.cpp src/waveformitem.cpp src/wavesamples.cpp

HEADERS += src/editengine.h
SOURCES += src/editengine.cpp

HEADERS += src/projectsafety.h
SOURCES += src/projectsafety.cpp

HEADERS += src/projectarchive.h
SOURCES += src/projectarchive.cpp
SOURCES += src/backendsafety.cpp

SOURCES += src/workspacelayouts.cpp
HEADERS += src/workspacelayouts.h
SOURCES += src/effectshelf.cpp src/hardwaresettings.cpp src/transcriber.cpp src/lutlibrary.cpp src/volumes.cpp src/removabledisks.cpp src/audiometers.cpp src/loudnesspass.cpp
HEADERS += src/effectshelf.h src/hardwaresettings.h src/transcriber.h src/lutlibrary.h src/volumes.h src/removabledisks.h src/audiometers.h src/loudnesspass.h
# The caption list: its checks, words, timing and speakers.
SOURCES += src/backendcaptionlist.cpp src/playerworkercaptionlist.cpp src/captionchecks.cpp
HEADERS += src/captionchecks.h

SOURCES += src/commandregistry.cpp
HEADERS += src/commandregistry.h

HEADERS += src/backgroundtasks.h
SOURCES += src/backgroundtasks.cpp src/backendfeedback.cpp

HEADERS += src/playbackview.h
SOURCES += src/playbackview.cpp src/backendplayback.cpp

HEADERS += src/relinkmatcher.h
SOURCES += src/relinkmatcher.cpp

HEADERS += src/markeredit.h

include(shared/hardware/hardware.pri)
SOURCES += src/backendhardware.cpp

HEADERS += src/retimeedit.h
HEADERS += src/sourcetiming.h
HEADERS += src/multicamtiming.h
SOURCES += src/retimeedit.cpp

HEADERS += src/creationnames.h
SOURCES += src/creationnames.cpp

include(viewer.pri)

include(timelineui.pri)

# Timeline interchange: OTIO through the library, EDL/FCPXML native, AAF through a bundled pyaaf2 helper.
HEADERS += src/interchange.h
SOURCES += src/interchange.cpp src/interchangeotio.cpp src/interchangeaaf.cpp
RESOURCES += src/tools.qrc
INCLUDEPATH += /usr/include/Imath
LIBS += -lopentimelineio -lopentime

include(translations.pri)
include(version.pri)
include(carried.pri)

HEADERS += src/agentcli.h src/agentskill.h
SOURCES += src/agentcli.cpp src/agentskill.cpp
RESOURCES += src/agentcli.qrc
include($$PWD/src/engine/engine.pri)
include($$PWD/src/render/render.pri)
include($$PWD/src/sound/sound.pri)
include($$PWD/src/deliver/deliver.pri)
include($$PWD/src/media/media.pri)
include($$PWD/src/edit/edit.pri)
HEADERS += $$PWD/src/renderrequest.h $$PWD/src/videoeffects.h $$PWD/src/keyer.h
SOURCES += $$PWD/src/videoeffects.cpp $$PWD/src/keyer.cpp
