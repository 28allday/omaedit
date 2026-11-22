# Optional app-local Vulkan point processing; the loader remains dynamic.
QT += core
CONFIG += c++17
INCLUDEPATH += $$PWD $$clean_path($$PWD/../hardware/third_party/vulkan)
HEADERS += $$PWD/compute.h
SOURCES += $$PWD/compute.cpp $$PWD/pointshader.cpp

HEADERS += $$PWD/shadercompiler.h
SOURCES += $$PWD/shadercompiler.cpp
