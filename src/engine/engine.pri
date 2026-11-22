# OmaEdit's render engine: Vulkan compute with a CPU twin doing the same maths.
# Its own shaders are compiled to SPIR-V when the program is built and linked
# into it; colour transforms that only exist at run time (OCIO's) are compiled
# then, with glslang's library (glsl.cpp).
CONFIG += c++20 link_pkgconfig
PKGCONFIG += vulkan libavutil libavcodec
INCLUDEPATH += $$PWD
HEADERS += $$PWD/engine.h $$PWD/frame.h $$PWD/glsl.h $$PWD/grade.h $$PWD/matting.h
SOURCES += $$PWD/vk_engine.cpp $$PWD/cpu_engine.cpp $$PWD/shaders.cpp $$PWD/probe.cpp $$PWD/glsl.cpp $$PWD/matting.cpp
# The weights of the network that finds people (matting.h).
RESOURCES += $$PWD/models/models.qrc
# glslang's own code is linked into the program: the file name of its shared
# library changes with each release, which would stop an installed program
# starting. SPIRV-Tools, which it calls, stays the system's shared library: the
# graphics drivers load that same library into the program, and two copies of it
# would mix (its symbols carry no version). The program keeps glslang's names to
# itself, so a library loaded later that uses its own glslang keeps that one.
LIBS += -l:libglslang-default-resource-limits.a -l:libglslang.a -lSPIRV-Tools-opt -lSPIRV-Tools
QMAKE_LFLAGS += -Wl,--exclude-libs,libglslang.a:libglslang-default-resource-limits.a
# Video effects: a shader (shaders/fx_*.comp) and a CPU twin (effects/fx_*.cpp) each.
HEADERS += $$PWD/effects/fx.h $$PWD/parallel.h
SOURCES += $$files($$PWD/effects/fx_*.cpp)

isEmpty(GLSLANG): GLSLANG = $$system(command -v glslangValidator)
isEmpty(GLSLANG): error("glslangValidator (package glslang) is needed to build OmaEdit's engine shaders.")
ENGINE_SHADERS = $$files($$PWD/shaders/*.comp)
spirv.input = ENGINE_SHADERS
spirv.output = $$OUT_PWD/spirv/${QMAKE_FILE_BASE}.spv.cpp
spirv.depends = $$PWD/shaders/common.glsl $$PWD/shaders/fx_common.glsl
spirv.commands = $$PWD/shaders/embed-spirv $$GLSLANG ${QMAKE_FILE_NAME} ${QMAKE_FILE_OUT}
spirv.variable_out = SOURCES
spirv.name = SPIR-V ${QMAKE_FILE_IN}
QMAKE_EXTRA_COMPILERS += spirv
