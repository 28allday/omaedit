#include "glsl.h"

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

#include <mutex>

namespace engine {

bool compileCompute(const std::string &source, std::vector<uint32_t> &spirv, std::string *error) {
    // The library's process state, set up once and kept for the program's life.
    static std::once_flag once;
    std::call_once(once, [] { glslang_initialize_process(); });
    glslang_input_t in{};
    in.language = GLSLANG_SOURCE_GLSL;
    in.stage = GLSLANG_STAGE_COMPUTE;
    in.client = GLSLANG_CLIENT_VULKAN;
    in.client_version = GLSLANG_TARGET_VULKAN_1_2;
    in.target_language = GLSLANG_TARGET_SPV;
    in.target_language_version = GLSLANG_TARGET_SPV_1_5;
    in.code = source.c_str();
    in.default_version = 460;
    in.default_profile = GLSLANG_NO_PROFILE;
    in.messages = GLSLANG_MSG_DEFAULT_BIT;
    in.resource = glslang_default_resource();
    glslang_shader_t *shader = glslang_shader_create(&in);
    if (!shader) { if (error) *error = "the shader compiler could not start"; return false; }
    bool ok = glslang_shader_preprocess(shader, &in) && glslang_shader_parse(shader, &in);
    if (!ok && error) *error = glslang_shader_get_info_log(shader);
    if (ok) {
        glslang_program_t *program = glslang_program_create();
        glslang_program_add_shader(program, shader);
        ok = glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT | GLSLANG_MSG_VULKAN_RULES_BIT);
        if (ok) {
            glslang_program_SPIRV_generate(program, GLSLANG_STAGE_COMPUTE);
            const size_t words = glslang_program_SPIRV_get_size(program);
            spirv.resize(words);
            glslang_program_SPIRV_get(program, spirv.data());
            ok = words > 0;
            if (!ok && error) *error = "the shader compiler made no code";
        } else if (error) {
            *error = glslang_program_get_info_log(program);
        }
        glslang_program_delete(program);
    }
    glslang_shader_delete(shader);
    return ok;
}

}  // namespace engine
