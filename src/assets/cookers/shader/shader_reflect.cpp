#include "assets/cookers/shader/shader_reflect.h"

#include <cstring>

namespace shadercook {

namespace {
// The type byte carries FOUR flag bits, not one (bgfx_p.h:1598-1607). Masking
// off only the fragment bit was a real bug: the SPIR-V backend writes samplers
// as `Sampler | kUniformSamplerBit` (shaderc_spirv.cpp:790), so the masked
// value came out as 0x20 — larger than Mat4 — and every sampler reflected as
// Unknown. Metal does not set that bit, which is exactly why verifying one
// profile and assuming the rest hid this.
constexpr uint8_t kFragmentBit = 0x10;
constexpr uint8_t kSamplerBit  = 0x20;
constexpr uint8_t kReadOnlyBit = 0x40;
constexpr uint8_t kCompareBit  = 0x80;
constexpr uint8_t kFlagMask    = kFragmentBit | kSamplerBit
                               | kReadOnlyBit | kCompareBit;

// A cursor that can never read past the end. Every field goes through it, so
// a truncated or hostile blob returns an error instead of walking off the
// buffer — this parses the output of an EXTERNAL PROCESS, which makes it an
// untrusted-input boundary no matter how much we trust shaderc itself.
struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    template <class T> T read() {
        if (!ok || (size_t)(end - p) < sizeof(T)) { ok = false; return T{}; }
        T v{};
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::string readStr(uint8_t n) {
        if (!ok || (size_t)(end - p) < n) { ok = false; return {}; }
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
};

bool isShaderMagic(uint32_t m) {
    // 'VSH'/'FSH'/'CSH' + a version byte that moves with bgfx. Match the three
    // letters and ignore the version: rejecting on version would break every
    // cooked shader on a bgfx bump, and the version we actually care about is
    // the .cshader container's.
    const uint8_t a = (uint8_t)(m & 0xFF);
    const uint8_t b = (uint8_t)((m >> 8) & 0xFF);
    const uint8_t c = (uint8_t)((m >> 16) & 0xFF);
    return (a == 'V' || a == 'F' || a == 'C') && b == 'S' && c == 'H';
}
} // namespace

const char* uniformKindName(UniformKind k) {
    switch (k) {
        case UniformKind::Sampler: return "sampler";
        case UniformKind::End:     return "end";
        case UniformKind::Vec4:    return "vec4";
        case UniformKind::Mat3:    return "mat3";
        case UniformKind::Mat4:    return "mat4";
        default:                   return "unknown";
    }
}

const ReflectedUniform* Reflection::find(const std::string& name) const {
    for (const auto& u : uniforms) if (u.name == name) return &u;
    return nullptr;
}

Reflection reflectShaderBinary(const uint8_t* data, size_t size) {
    Reflection r;
    if (!data || size < 12) { r.error = "blob too small to be a shader"; return r; }

    Cursor c{ data, data + size };
    const uint32_t magic = c.read<uint32_t>();
    if (!isShaderMagic(magic)) { r.error = "bad shader magic"; return r; }
    c.read<uint32_t>();   // inputHash
    c.read<uint32_t>();   // outputHash

    const uint16_t count = c.read<uint16_t>();
    r.uniforms.reserve(count);
    for (uint16_t i = 0; i < count && c.ok; ++i) {
        ReflectedUniform u;
        const uint8_t nameLen = c.read<uint8_t>();
        u.name = c.readStr(nameLen);
        const uint8_t type = c.read<uint8_t>();
        u.fragment = (type & kFragmentBit) != 0;
        u.sampler  = (type & kSamplerBit)  != 0;
        u.compare  = (type & kCompareBit)  != 0;
        u.readOnly = (type & kReadOnlyBit) != 0;
        const uint8_t kind = type & ~kFlagMask;
        u.kind = kind <= (uint8_t)UniformKind::Mat4
               ? (UniformKind)kind : UniformKind::Unknown;
        // The sampler BIT is authoritative where present; backends that omit it
        // still encode Sampler as the base type.
        if (u.sampler) u.kind = UniformKind::Sampler;
        u.num      = c.read<uint8_t>();
        u.regIndex = c.read<uint16_t>();
        u.regCount = c.read<uint16_t>();
        c.read<uint8_t>();    // texComponent
        c.read<uint8_t>();    // texDimension
        c.read<uint16_t>();   // texFormat
        if (!c.ok) break;
        // Bytecode is EXTERNAL INPUT (another process wrote it, and a cooked
        // blob can arrive from a shared DDC). Two uniforms under one name would
        // make find() return the first and silently ignore the rest, so the
        // declared interface could verify against a uniform that isn't the one
        // the shader actually uses.
        for (const auto& seen : r.uniforms) {
            if (seen.name == u.name) {
                r.error = "duplicate uniform \"" + u.name + "\" in uniform table";
                r.uniforms.clear();
                return r;
            }
        }
        r.uniforms.push_back(std::move(u));
    }

    if (!c.ok) { r.error = "truncated uniform table"; r.uniforms.clear(); return r; }
    r.ok = true;
    return r;
}

} // namespace shadercook
