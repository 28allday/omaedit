include($$PWD/ffmpeg.pri)
# The colour pipeline makes the render engine's colour programs (frame.h): C++20.
CONFIG += c++20
# Use system OCIO or a private dependency prefix prepared by bin/setup-ocio.
OMAEDIT_OCIO_PREFIX = $$(OMAEDIT_OCIO_PREFIX)
isEmpty(OMAEDIT_OCIO_PREFIX):!packagesExist(OpenColorIO):exists($$PWD/build/deps/ocio/usr/include/OpenColorIO/OpenColorIO.h): OMAEDIT_OCIO_PREFIX = $$PWD/build/deps/ocio/usr
!isEmpty(OMAEDIT_OCIO_PREFIX) {
    INCLUDEPATH += $$OMAEDIT_OCIO_PREFIX/include
    LIBS += -L$$OMAEDIT_OCIO_PREFIX/lib -lOpenColorIO
    QMAKE_LFLAGS += -Wl,--disable-new-dtags
    QMAKE_RPATHDIR += $$OMAEDIT_OCIO_PREFIX/lib
} else {
    PKGCONFIG += OpenColorIO
}
include($$PWD/bands.pri)
HEADERS += $$PWD/src/colourpipeline.h
SOURCES += $$PWD/src/colourpipeline.cpp
HEADERS += $$PWD/src/lineargrade.h
SOURCES += $$PWD/src/lineargrade.cpp
HEADERS += $$PWD/src/displaygrade.h
SOURCES += $$PWD/src/displaygrade.cpp
HEADERS += $$PWD/src/workingframe.h
SOURCES += $$PWD/src/workingframe.cpp
PKGCONFIG += libavcodec libavformat libswscale OpenEXR
HEADERS += $$PWD/src/nativevideo.h $$PWD/src/nativepixels_p.h
HEADERS += $$PWD/src/rawpixels.h
SOURCES += $$PWD/src/nativevideo.cpp $$PWD/src/workingframeio.cpp
HEADERS += $$PWD/src/linearrenderer.h $$PWD/src/solidcolour.h
SOURCES += $$PWD/src/linearrenderer.cpp
HEADERS += $$PWD/src/retimemap.h $$PWD/src/framesampling.h
SOURCES += $$PWD/src/retimemap.cpp
HEADERS += $$PWD/src/timelinerate.h
SOURCES += $$PWD/src/timelinerate.cpp
HEADERS += $$PWD/src/sourcerateedit.h
SOURCES += $$PWD/src/sourcerateedit.cpp
include($$PWD/motionflow.pri)
HEADERS += $$PWD/src/speedrampedit.h
SOURCES += $$PWD/src/speedrampedit.cpp
RESOURCES += $$PWD/colour/colour.qrc

include($$PWD/shared/processing/processing.pri)
