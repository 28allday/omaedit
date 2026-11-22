isEmpty(OMAEDIT_FFMPEG_PRI) {
OMAEDIT_FFMPEG_PRI = 1
# FFmpeg 9.1 carried for the engine's Vulkan video (bin/build-ffmpeg): the copy
# beside the build while developing, then where the package installs it. It
# keeps the system FFmpeg's major versions, so one FFmpeg is loaded in the
# process.
carried_ffmpeg {
    # Its headers ahead of /usr/include, which other packages' flags name.
    INCLUDEPATH = $$CARRIED_FFMPEG/include $$INCLUDEPATH
    QMAKE_LFLAGS += -Wl,--enable-new-dtags
    # The copy beside the build only while developing: the package's binary
    # looks in its install place alone.
    !packaged: QMAKE_LFLAGS += -Wl,-rpath,$$CARRIED_FFMPEG/lib
    QMAKE_LFLAGS += -Wl,-rpath,/usr/lib/omaedit/ffmpeg/lib
    # Every FFmpeg library of the carried build is loaded by the program
    # itself, so a library that asks for one later (Qt Multimedia's) gets this build's,
    # never the system's of another version beside it.
    LIBS += -Wl,--no-as-needed -L$$CARRIED_FFMPEG/lib -lavdevice -lavfilter -lavformat -lavcodec \
            -lswscale -lswresample -lavutil -Wl,--as-needed
}
}
