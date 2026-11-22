include($$PWD/shared/processing/async.pri)
include($$PWD/shared/quick/quick.pri)
HEADERS += $$PWD/src/viewerprocessing.h $$PWD/src/viewerprocessor.h
SOURCES += $$PWD/src/viewerprocessing.cpp $$PWD/src/viewerprocessor.cpp
HEADERS += $$PWD/src/scopeprocessor.h
SOURCES += $$PWD/src/scopeprocessor.cpp

include($$PWD/shared/processing/vulkan.pri)
include($$PWD/shared/performance/performance.pri)
HEADERS += $$PWD/src/viewergpu.h $$PWD/src/gpuprocessing.h
SOURCES += $$PWD/src/viewergpu.cpp $$PWD/src/gpuprocessing.cpp
