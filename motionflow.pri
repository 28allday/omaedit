# Dense motion analysis uses only core, image processing and video modules.
packagesExist(opencv5): OMAEDIT_OPENCV_PACKAGE = opencv5
else:packagesExist(opencv4): OMAEDIT_OPENCV_PACKAGE = opencv4
else: error("OpenCV is required. Install the opencv development package.")
INCLUDEPATH += $$system(pkg-config --variable=includedir $$OMAEDIT_OPENCV_PACKAGE)
LIBS += $$system(pkg-config --libs-only-L $$OMAEDIT_OPENCV_PACKAGE)
LIBS += -lopencv_video -lopencv_imgproc -lopencv_core
HEADERS += $$PWD/src/motionflow.h
SOURCES += $$PWD/src/motionflow.cpp
# Reframe to Fit's saliency uses the same modules.
HEADERS += $$PWD/src/reframetofit.h
SOURCES += $$PWD/src/reframetofit.cpp
