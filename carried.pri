# The package carries the libraries whose file names change when the system
# updates them (bin/carry-libs copies them into /usr/lib/omaedit/lib), so such an
# update cannot stop the program starting. The program looks there first.
# A carried library must find the carried libraries it uses in turn. It has no
# run path of its own, so the program links each of those itself: the loader
# takes the program's own libraries first, from its run path, and a library
# asked for again by name is the one already loaded.
packaged {
    QMAKE_LFLAGS += -Wl,-rpath,/usr/lib/omaedit/lib
    # Written as file names (-l:libImath-3_2.so): qmake drops a library it has
    # already been given, and OpenEXR's are named earlier, where the linker may
    # leave out the ones the program does not call itself.
    LIBS += -Wl,--no-as-needed \
            $$system("pkg-config --libs-only-l OpenEXR Imath yaml-cpp | sed -E 's/-l([^ ]+)/-l:lib\\1.so/g'") \
            -lopencv_dnn -lopencv_imgcodecs -lopencv_geometry -lopencv_flann -lpystring -lminizip-ng -lopenjph -Wl,--as-needed
}
