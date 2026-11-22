include($$PWD/ffmpeg.pri)
PKGCONFIG += libavformat libavcodec libavutil libswresample
HEADERS += $$PWD/src/audiosequence.h $$PWD/src/audiosamplesource.h
SOURCES += $$PWD/src/audiosamplesource.cpp
HEADERS += $$PWD/src/audiotimeline.h
SOURCES += $$PWD/src/audiotimeline.cpp
HEADERS += $$PWD/src/audiotimingedit.h
SOURCES += $$PWD/src/audiotimingedit.cpp
HEADERS += $$PWD/src/audioclipboard.h
SOURCES += $$PWD/src/audioclipboard.cpp
