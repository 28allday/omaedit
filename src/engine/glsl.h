#pragma once

// GLSL compute shaders compiled to SPIR-V while the program runs, for shader
// text that only exists then (an OCIO transform's). Shaders known when the
// editor is built are compiled then instead (shaders/embed-spirv).

#include <cstdint>
#include <string>
#include <vector>

namespace engine {
// Vulkan 1.2 / SPIR-V 1.5. False with `error` set when the text is refused.
bool compileCompute(const std::string &source, std::vector<uint32_t> &spirv, std::string *error);
}
