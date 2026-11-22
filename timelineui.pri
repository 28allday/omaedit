isEmpty(OMAEDIT_TIMELINE_UI_INCLUDED) {
OMAEDIT_TIMELINE_UI_INCLUDED = 1
HEADERS += $$PWD/src/timelinewindowmodel.h
SOURCES += $$PWD/src/timelinewindowmodel.cpp
HEADERS += $$PWD/src/toolcursor.h
SOURCES += $$PWD/src/toolcursor.cpp

HEADERS += $$PWD/src/thumbnaildiskcache.h $$PWD/src/thumbnailprovider.h
SOURCES += $$PWD/src/thumbnaildiskcache.cpp $$PWD/src/thumbnailprovider.cpp

}
