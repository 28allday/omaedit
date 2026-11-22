# The editor's own sound: plain-float DSP and the timeline mixer.
INCLUDEPATH += $$PWD $$PWD/..
HEADERS += $$PWD/dsp.h $$PWD/mixer.h $$PWD/inserts.h $$PWD/peaklimit.h
SOURCES += $$PWD/dsp.cpp $$PWD/mixer.cpp $$PWD/inserts.cpp $$PWD/peaklimit.cpp
LIBS += -lrubberband
