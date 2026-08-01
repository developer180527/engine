#pragma once
// ── shaderc invocation — one compile, one child process ─────────────────────
//
// ONE concern: run bgfx's `shaderc` for a single (source, type, profile,
// defines) and hand back the bytecode or the compiler's diagnostics.
//
// Out-of-process on purpose, matching CookPipeline's worker model: shaderc is
// a third-party compiler stack (glslang, SPIRV-Cross, fcpp) that is already
// excluded from the sanitizer lanes because it aborts under them
// (CMakeLists.txt:179-185). Linking it into the cooker would drag that
// exclusion — and any crash in it — into the editor process.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace shadercook {

enum class ShaderStage { Vertex, Fragment };

struct CompileRequest {
    std::filesystem::path source;
    std::filesystem::path varyingDef;
    std::vector<std::filesystem::path> includeDirs;
    std::vector<std::string> defines;      // feature defines for this variant
    ShaderStage stage    = ShaderStage::Vertex;
    uint32_t    profile  = 0;              // assetlib::ShaderProfileId
    int         optimize = 3;              // shaderc -O
};

struct CompileResult {
    bool ok = false;
    std::vector<uint8_t> bytecode;
    std::string diagnostics;   // shaderc's stdout+stderr, verbatim
    std::string error;         // our own failure (spawn, missing tool, ...)
};

// Absolute path to the shaderc binary. Resolution order:
//   1. $ENGINE_SHADERC        — override, e.g. a Windows runner's own build
//   2. ENGINE_SHADERC_PATH    — baked in by CMake from $<TARGET_FILE:bgfx::shaderc>
// Empty when neither resolves to an existing file.
std::filesystem::path findShaderc();

// Include directories every compile needs regardless of the shader: bgfx's own
// `bgfx_shader.sh` and friends, which every `.sc` includes. Like shaderc
// itself this is a property of the BUILD ENVIRONMENT, not of the asset, so it
// is baked in by CMake (ENGINE_SHADER_INCLUDE_PATH, a `;`-separated list) with
// $ENGINE_SHADER_INCLUDES as the override.
std::vector<std::filesystem::path> defaultIncludeDirs();

CompileResult compileShader(const std::filesystem::path& shadercExe,
                            const CompileRequest& req);

} // namespace shadercook
