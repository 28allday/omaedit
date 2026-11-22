# Timeline pictures drawn by the engine from the editor's own timeline
# description: the media pool (decoding) and the timeline renderer.
INCLUDEPATH += $$PWD $$PWD/..
HEADERS += $$PWD/mediapool.h $$PWD/framing.h $$PWD/timelinerenderer.h $$PWD/lookuptables.h $$PWD/generators.h $$PWD/effectsteps.h $$PWD/stabilise.h $$PWD/engines.h $$PWD/gradeplan.h $$PWD/cachecodec.h
SOURCES += $$PWD/mediapool.cpp $$PWD/timelinerenderer.cpp $$PWD/lookuptables.cpp $$PWD/generators.cpp $$PWD/effectsteps.cpp $$PWD/stabilise.cpp $$PWD/engines.cpp $$PWD/gradeplan.cpp $$PWD/cachecodec.cpp
LIBS += -lvidstab
