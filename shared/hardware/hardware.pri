# Versioned, app-local Qt Core component. No linked Vulkan or CUDA dependency.
QT += core
CONFIG += c++17
INCLUDEPATH += $$PWD $$PWD/third_party/vulkan
HEADERS += $$PWD/vulkaninventory.h $$PWD/hardwareprobe.h
SOURCES += $$PWD/vulkaninventory.cpp $$PWD/hardwareprobe.cpp
