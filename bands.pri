# Shared band helpers: split image work across a pool. Included by more than
# one group, so guard against listing the sources twice.
!contains(SOURCES, $$PWD/src/bands.cpp) {
    HEADERS += $$PWD/src/bands.h $$PWD/src/imagebands.h
    SOURCES += $$PWD/src/bands.cpp $$PWD/src/imagebands.cpp
}
