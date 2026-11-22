// The engine's compute shaders, compiled into the program when it is built
// (engine.pri, shaders/embed-spirv). Looked up by the name they had as files.
#include <QByteArray>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define SHADER(name) extern const uint32_t spirv_##name[]; extern const size_t spirv_##name##_size;
SHADER(composite) SHADER(convert) SHADER(deinterlace) SHADER(denoise) SHADER(filter) SHADER(grade)
SHADER(linear) SHADER(nn) SHADER(preview) SHADER(transition) SHADER(yuv) SHADER(yuv_in)
#define EFFECT(Name, stem, passes) SHADER(fx_##stem)
#include "effects/effects.def"
#undef EFFECT
#undef SHADER

namespace engine {
QByteArray loadSpirv(const char *file) {
    struct Entry { const char *name; const uint32_t *words; size_t bytes; };
#define SHADER(name) {#name ".spv", spirv_##name, spirv_##name##_size},
    static const Entry table[] = {
        SHADER(composite) SHADER(convert) SHADER(deinterlace) SHADER(denoise) SHADER(filter) SHADER(grade)
        SHADER(linear) SHADER(nn) SHADER(preview) SHADER(transition) SHADER(yuv) SHADER(yuv_in)
#define EFFECT(Name, stem, passes) SHADER(fx_##stem)
#include "effects/effects.def"
#undef EFFECT
    };
#undef SHADER
    for (const Entry &e : table)
        if (std::strcmp(e.name, file) == 0)
            return QByteArray::fromRawData(reinterpret_cast<const char *>(e.words), qsizetype(e.bytes));
    return {};
}
}  // namespace engine
