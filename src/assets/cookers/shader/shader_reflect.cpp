#include "assets/cookers/shader/shader_reflect.h"

#include <cstring>

namespace shadercook {

namespace {
// shaderc ORs this into the type byte for fragment-stage uniforms
// (kUniformFragmentBit in bgfx).
constexpr uint8_t kFragmentBit = 0x10;

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
        const uint8_t kind = type & ~kFragmentBit;
        u.kind = kind <= (uint8_t)UniformKind::Mat4
               ? (UniformKind)kind : UniformKind::Unknown;
        u.num      = c.read<uint8_t>();
        u.regIndex = c.read<uint16_t>();
        u.regCount = c.read<uint16_t>();
        c.read<uint8_t>();    // texComponent
        c.read<uint8_t>();    // texDimension
        c.read<uint16_t>();   // texFormat
        if (c.ok) r.uniforms.push_back(std::move(u));
    }

    if (!c.ok) { r.error = "truncated uniform table"; r.uniforms.clear(); return r; }
    r.ok = true;
    return r;
}

} // namespace shadercook
