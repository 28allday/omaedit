include($$PWD/../../ffmpeg.pri)
QT += core
CONFIG += c++17 link_pkgconfig
PKGCONFIG += libavcodec libavformat libavutil libswscale vulkan
INCLUDEPATH += $$PWD
HEADERS += $$PWD/decoder.h
SOURCES += $$PWD/decoder.cpp
HEADERS += $$PWD/startup.h
SOURCES += $$PWD/startup.cpp
RESOURCES += $$PWD/video.qrc
