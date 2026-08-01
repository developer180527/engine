#pragma once
// ── Reflection — read the uniform table back out of compiled bytecode ───────
//
// ONE concern: what uniforms does this compiled shader ACTUALLY have?
//
// Without this, a declared interface is a JSON file that can lie. A material
// could set "roughness", the cooker could accept it, and the value would land
// nowhere — a silent no-op, the worst possible failure for an artist-facing
// system, because the material looks correct and the render is subtly wrong.
//
// bgfx's shader binary carries its own uniform table (shaderc writes it at
// tools/shaderc/shaderc_metal.cpp:240-260 and the equivalent in every other
// backend), so the cooker can compile, read the truth back, and REJECT a
// declaration that doesn't match. Same discipline as the doc contract: the
// claim is checked mechanically or it isn't worth making.
#include <cstdint>
#include <string>
#include <vector>

namespace shadercook {

// bgfx::UniformType::Enum — Sampler, End, Vec4, Mat3, Mat4 (bgfx.h:282).
// Mirrored rather than included: this runs inside engine_core, which is
// deliberately GPU-free so cookers link without bgfx.
enum class UniformKind : uint8_t {
    Sampler = 0, End = 1, Vec4 = 2, Mat3 = 3, Mat4 = 4, Unknown = 255,
};
const char* uniformKindName(UniformKind k);

struct ReflectedUniform {
    std::string name;
    UniformKind kind     = UniformKind::Unknown;
    uint8_t     num      = 0;    // array length
    uint16_t    regIndex = 0;
    uint16_t    regCount = 0;    // vec4 registers occupied
    bool        fragment = false;
    // Set from the type byte's sampler bit, which some backends use and others
    // don't — see the flag note in shader_reflect.cpp. Prefer this over
    // `kind == Sampler` when asking "is this a texture slot?".
    bool        sampler  = false;
    bool        compare  = false;   // shadow/compare sampler
    bool        readOnly = false;
};

struct Reflection {
    std::vector<ReflectedUniform> uniforms;
    bool ok = false;
    std::string error;

    const ReflectedUniform* find(const std::string& name) const;
};

// Parse a shaderc output blob. Layout, from shaderc.cpp:1707-1720 and the
// per-backend writers:
//   magic u32 ('VSH'/'FSH'/'CSH' + version), inputHash u32, outputHash u32,
//   count u16, then per uniform:
//     nameLen u8, name[nameLen], type u8 (|0x10 = fragment), num u8,
//     regIndex u16, regCount u16, texComponent u8, texDimension u8,
//     texFormat u16
Reflection reflectShaderBinary(const uint8_t* data, size_t size);

} // namespace shadercook
