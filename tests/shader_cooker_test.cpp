// ── shader_cooker_test — the declared interface, without a compiler ─────────
//
// Phase 5 of docs/plans/renderer-audit-and-plan.md: shaders become cooked assets
// that PUBLISH the parameters a material may set, so a material can stop being
// a fixed C++ struct.
//
// Everything here is hermetic. Manifest parsing, reflection, verification and
// the container format are pure functions over bytes, so none of it needs
// shaderc, a GPU, or an asset on disk. Uniform tables are hand-built to the
// layout shaderc writes (shaderc_metal.cpp:240-260), which is exactly the
// layout the reflector claims to read — if that claim is wrong, these fail.
//
// The subprocess path (findShaderc + compileShader) is NOT covered here on
// purpose: it depends on an external binary, and a unit test that silently
// skips when the tool is missing is worse than no test. It is verified by
// actually cooking shaders/standard.shader.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <assetlib/shader_asset.h>

#include "assets/cookers/shader/shader_manifest.h"
#include "assets/cookers/shader/shader_reflect.h"
#include "assets/cookers/shader/shader_verify.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using namespace shadercook;
namespace fs = std::filesystem;

// ── a hand-built bgfx shader binary ─────────────────────────────────────────
struct BlobBuilder {
    std::vector<uint8_t> b;

    void u8(uint8_t v)  { b.push_back(v); }
    void u16(uint16_t v){ b.push_back((uint8_t)(v & 0xFF)); b.push_back((uint8_t)(v >> 8)); }
    void u32(uint32_t v){ for (int i = 0; i < 4; ++i) b.push_back((uint8_t)(v >> (8*i))); }

    // 'FSH' + version byte, then the two hashes shaderc writes.
    void header(char kind) {
        u8((uint8_t)kind); u8('S'); u8('H'); u8(11);
        u32(0xAAAAAAAA);   // inputHash
        u32(0xBBBBBBBB);   // outputHash
    }
    void uniform(const char* name, UniformKind kind, uint8_t num,
                 uint16_t regIndex, uint16_t regCount, bool fragment) {
        uniformRaw(name, (uint8_t)((uint8_t)kind | (fragment ? 0x10 : 0x00)),
                   num, regIndex, regCount);
    }
    // The type byte verbatim, so a backend's exact flag encoding can be tested.
    void uniformRaw(const char* name, uint8_t typeByte, uint8_t num,
                    uint16_t regIndex, uint16_t regCount) {
        const uint8_t len = (uint8_t)std::strlen(name);
        u8(len);
        for (uint8_t i = 0; i < len; ++i) u8((uint8_t)name[i]);
        u8(typeByte);
        u8(num); u16(regIndex); u16(regCount);
        u8(0); u8(0); u16(0);   // texComponent, texDimension, texFormat
    }
};

// The uniform set fs_triangle.sc actually declares.
static std::vector<uint8_t> fragmentBlob() {
    BlobBuilder bb;
    bb.header('F');
    bb.u16(5);
    bb.uniform("s_baseColor",  UniformKind::Sampler, 1, 0, 1, true);
    bb.uniform("s_normalMap",  UniformKind::Sampler, 1, 1, 1, true);
    bb.uniform("s_shadowMap",  UniformKind::Sampler, 1, 2, 1, true);
    bb.uniform("u_params",     UniformKind::Vec4,    1, 0, 1, true);
    bb.uniform("u_colorFactor",UniformKind::Vec4,    1, 1, 1, true);
    return bb.b;
}
static std::vector<uint8_t> vertexBlob() {
    BlobBuilder bb;
    bb.header('V');
    bb.u16(1);
    bb.uniform("u_shadowMtx", UniformKind::Mat4, 1, 0, 4, false);
    return bb.b;
}

static std::string manifestJson(const char* extraParams = "",
                                const char* features = "") {
    return std::string(
        "{ \"name\": \"standard\","
        "  \"vertex\": \"vs_triangle.sc\","
        "  \"fragment\": \"fs_triangle.sc\","
        "  \"features\": [") + features + "],"
        "  \"parameters\": ["
        "    { \"name\": \"baseColorFactor\", \"type\": \"color\","
        "      \"uniform\": \"u_colorFactor\", \"offset\": 0,"
        "      \"default\": [1,1,1,1] },"
        "    { \"name\": \"roughness\", \"type\": \"float\","
        "      \"uniform\": \"u_params\", \"offset\": 1, \"default\": 0.7 }"
        + std::string(extraParams) +
        "  ],"
        "  \"samplers\": ["
        "    { \"name\": \"baseColor\", \"uniform\": \"s_baseColor\","
        "      \"stage\": 0, \"fallback\": \"white\" }"
        "  ] }";
}

int main() {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("shader_cooker_test\n");
    const fs::path base = "/shaders";

    // ── manifest parsing ────────────────────────────────────────────────────
    {
        ShaderManifest m;
        auto r = parseShaderManifest(manifestJson(), base, m);
        CHECK(r.ok, "a well-formed manifest parses (%s)", r.error.c_str());
        CHECK(m.name == "standard", "name read");
        CHECK(m.vertexPath == base / "vs_triangle.sc", "vertex path resolved");
        CHECK(m.varyingPath == base / "varying.def.sc",
              "varying defaults to varying.def.sc");
        CHECK(m.params.size() == 2 && m.samplers.size() == 1,
              "2 params, 1 sampler");
        CHECK(m.params[0].type == assetlib::kParamColor
              && m.params[0].defaults[3] == 1.0f, "color default read");
        CHECK(m.params[1].defaults[0] == 0.7f, "scalar default read");
        CHECK(m.variantCount() == 1, "no features means exactly one variant");
    }

    // Rejections. Each of these, accepted, produces a material parameter that
    // writes to the wrong place or nowhere — invisible in the editor.
    {
        ShaderManifest m;
        CHECK(!parseShaderManifest("{ not json", base, m).ok,
              "malformed JSON is rejected");
        CHECK(!parseShaderManifest("{\"vertex\":\"v.sc\",\"fragment\":\"f.sc\"}",
                                   base, m).ok, "missing name is rejected");
        CHECK(!parseShaderManifest("{\"name\":\"x\",\"fragment\":\"f.sc\"}",
                                   base, m).ok, "missing vertex is rejected");

        auto badType = parseShaderManifest(
            "{\"name\":\"x\",\"vertex\":\"v.sc\",\"fragment\":\"f.sc\","
            " \"parameters\":[{\"name\":\"p\",\"type\":\"quat\","
            " \"uniform\":\"u_a\"}]}", base, m);
        CHECK(!badType.ok && badType.error.find("quat") != std::string::npos,
              "unknown param type is rejected and named: %s",
              badType.error.c_str());

        // vec3 at offset 2 would occupy floats 2,3,4 — crossing into the next
        // vec4 register, corrupting whatever uniform sits there.
        auto straddle = parseShaderManifest(
            "{\"name\":\"x\",\"vertex\":\"v.sc\",\"fragment\":\"f.sc\","
            " \"parameters\":[{\"name\":\"p\",\"type\":\"vec3\","
            " \"uniform\":\"u_a\",\"offset\":2}]}", base, m);
        CHECK(!straddle.ok
              && straddle.error.find("straddles") != std::string::npos,
              "a param crossing a vec4 boundary is rejected");

        auto overlap = parseShaderManifest(
            "{\"name\":\"x\",\"vertex\":\"v.sc\",\"fragment\":\"f.sc\","
            " \"parameters\":["
            "  {\"name\":\"a\",\"type\":\"vec2\",\"uniform\":\"u_a\",\"offset\":0},"
            "  {\"name\":\"b\",\"type\":\"float\",\"uniform\":\"u_a\",\"offset\":1}]}",
            base, m);
        CHECK(!overlap.ok && overlap.error.find("overlap") != std::string::npos,
              "two params writing the same float are rejected");

        auto dupStage = parseShaderManifest(
            "{\"name\":\"x\",\"vertex\":\"v.sc\",\"fragment\":\"f.sc\","
            " \"samplers\":["
            "  {\"name\":\"a\",\"uniform\":\"s_a\",\"stage\":0},"
            "  {\"name\":\"b\",\"uniform\":\"s_b\",\"stage\":0}]}", base, m);
        CHECK(!dupStage.ok, "two samplers on one stage are rejected");

        auto dupFeature = parseShaderManifest(
            "{\"name\":\"x\",\"vertex\":\"v.sc\",\"fragment\":\"f.sc\","
            " \"features\":[\"A\",\"A\"]}", base, m);
        CHECK(!dupFeature.ok, "a duplicate feature is rejected");

        // Two features mapping to one DEFINE doubles the variant matrix while
        // half of it compiles to byte-identical output — distinct-looking
        // variants, redundant DDC entries.
        auto dupDefine = parseShaderManifest(
            "{\"name\":\"x\",\"vertex\":\"v.sc\",\"fragment\":\"f.sc\","
            " \"features\":[{\"name\":\"A\",\"define\":\"USE_V\"},"
            "                {\"name\":\"B\",\"define\":\"USE_V\"}]}", base, m);
        CHECK(!dupDefine.ok
              && dupDefine.error.find("USE_V") != std::string::npos,
              "two features sharing one define are rejected");

        std::string many = "{\"name\":\"x\",\"vertex\":\"v.sc\",\"fragment\":\"f.sc\","
                           " \"features\":[";
        for (int i = 0; i <= (int)assetlib::kMaxShaderFeatures; ++i)
            many += (i ? ",\"F" : "\"F") + std::to_string(i) + "\"";
        many += "]}";
        auto tooMany = parseShaderManifest(many, base, m);
        CHECK(!tooMany.ok && tooMany.error.find("2^n") != std::string::npos,
              "exceeding the feature cap is rejected — the anti-bloat rule");
    }

    // ── feature masks -> defines ────────────────────────────────────────────
    {
        ShaderManifest m;
        auto r = parseShaderManifest(
            manifestJson("", "\"SKINNED\", {\"name\":\"FOG\",\"define\":\"USE_FOG\"}"),
            base, m);
        CHECK(r.ok, "features parse in both forms (%s)", r.error.c_str());
        CHECK(m.variantCount() == 4, "2 features means 4 variants (%u)",
              m.variantCount());
        CHECK(m.definesFor(0).empty(), "mask 0 defines nothing");
        CHECK(m.definesFor(1).size() == 1 && m.definesFor(1)[0] == "SKINNED",
              "bit 0 is the first declared feature");
        CHECK(m.definesFor(2).size() == 1 && m.definesFor(2)[0] == "USE_FOG",
              "an explicit define overrides the name");
        CHECK(m.definesFor(3).size() == 2, "both bits set gives both defines");
    }

    // ── reflection ──────────────────────────────────────────────────────────
    {
        const auto fb = fragmentBlob();
        const Reflection r = reflectShaderBinary(fb.data(), fb.size());
        CHECK(r.ok, "a well-formed shader binary reflects (%s)", r.error.c_str());
        CHECK(r.uniforms.size() == 5, "5 uniforms found (%zu)", r.uniforms.size());
        const auto* p = r.find("u_params");
        CHECK(p && p->kind == UniformKind::Vec4 && p->regCount == 1 && p->fragment,
              "u_params is a fragment vec4 with 1 register");
        const auto* s = r.find("s_baseColor");
        CHECK(s && s->kind == UniformKind::Sampler, "s_baseColor is a sampler");
        CHECK(r.find("u_nope") == nullptr, "an absent uniform is not found");

        const auto vb = vertexBlob();
        const Reflection rv = reflectShaderBinary(vb.data(), vb.size());
        CHECK(rv.ok && rv.find("u_shadowMtx")
              && rv.find("u_shadowMtx")->regCount == 4
              && !rv.find("u_shadowMtx")->fragment,
              "a vertex mat4 reflects with 4 registers");

        // Hostile / truncated input must not walk off the buffer.
        CHECK(!reflectShaderBinary(nullptr, 0).ok, "null blob rejected");
        CHECK(!reflectShaderBinary(fb.data(), 4).ok, "tiny blob rejected");
        std::vector<uint8_t> bad = fb;
        bad[0] = 'X';
        CHECK(!reflectShaderBinary(bad.data(), bad.size()).ok,
              "bad magic rejected");
        for (size_t cut = 12; cut < fb.size(); cut += 3) {
            const Reflection t = reflectShaderBinary(fb.data(), cut);
            if (t.ok && t.uniforms.size() == 5) {
                CHECK(false, "truncation at %zu was not detected", cut);
                break;
            }
        }
        CHECK(true, "every truncation point is detected, none read past the end");

        // Bytecode is external input. A duplicate name would make find()
        // return the first and ignore the rest, so the declared interface
        // could verify against a uniform the shader doesn't actually use.
        BlobBuilder dup;
        dup.header('F');
        dup.u16(2);
        dup.uniform("u_params", UniformKind::Vec4, 1, 0, 1, true);
        dup.uniform("u_params", UniformKind::Vec4, 1, 1, 1, true);
        const Reflection dr = reflectShaderBinary(dup.b.data(), dup.b.size());
        CHECK(!dr.ok && dr.error.find("duplicate") != std::string::npos,
              "a duplicate uniform name is rejected");

        // REGRESSION: bgfx's type byte carries four flag bits (bgfx_p.h:1598).
        // SPIR-V writes samplers as `Sampler | kUniformSamplerBit (0x20)`
        // (shaderc_spirv.cpp:790) while Metal omits that bit. Masking off only
        // the fragment bit made every SPIR-V sampler reflect as Unknown, so the
        // interface check failed on Vulkan builds — invisible while only one
        // profile was verified.
        BlobBuilder spv;
        spv.header('F');
        spv.u16(2);
        spv.uniformRaw("s_baseColor", (uint8_t)(0x00 | 0x20 | 0x10), 1, 0, 1);
        spv.uniformRaw("s_shadowMap", (uint8_t)(0x00 | 0x20 | 0x10 | 0x80), 1, 1, 1);
        const Reflection sr = reflectShaderBinary(spv.b.data(), spv.b.size());
        CHECK(sr.ok, "a SPIR-V uniform table reflects (%s)", sr.error.c_str());
        CHECK(sr.find("s_baseColor")
              && sr.find("s_baseColor")->kind == UniformKind::Sampler
              && sr.find("s_baseColor")->sampler,
              "a SPIR-V sampler reflects as a SAMPLER, not Unknown");
        CHECK(sr.find("s_shadowMap") && sr.find("s_shadowMap")->compare,
              "the compare bit is decoded (shadow sampler)");
    }

    // ── verification ────────────────────────────────────────────────────────
    {
        const auto vb = vertexBlob(), fb = fragmentBlob();
        const Reflection rv = reflectShaderBinary(vb.data(), vb.size());
        const Reflection rf = reflectShaderBinary(fb.data(), fb.size());

        ShaderManifest m;
        parseShaderManifest(manifestJson(), base, m);
        const VerifyResult good = verifyInterface(m.params, m.samplers, rv, rf);
        CHECK(good.ok, "a matching declaration verifies (%s)",
              good.joined(" | ").c_str());
        // s_normalMap, s_shadowMap and u_shadowMtx go undeclared by this
        // manifest — the first because it simply isn't exposed here, the other
        // two because they are engine-driven. All three warn; none fails.
        CHECK(good.warnings.size() == 3,
              "undeclared uniforms warn, they do not fail (%zu)",
              good.warnings.size());

        // A uniform present in BOTH stages is one fact about the shader, not
        // two. Warning twice per variant buries the diagnostics around it.
        BlobBuilder both;
        both.header('V');
        both.u16(1);
        both.uniform("u_shadowMtx", UniformKind::Mat4, 1, 0, 4, false);
        const Reflection rvBoth = reflectShaderBinary(both.b.data(), both.b.size());
        const VerifyResult dedup = verifyInterface({}, {}, rvBoth, rf);
        int shadowMtxWarnings = 0;
        for (const auto& w : dedup.warnings)
            if (w.find("u_shadowMtx") != std::string::npos) ++shadowMtxWarnings;
        CHECK(shadowMtxWarnings == 1,
              "a uniform in both stages warns ONCE, not twice (%d)",
              shadowMtxWarnings);

        // THE bug this whole mechanism exists to catch: a typo'd uniform. The
        // material would set "roughness" forever and nothing would happen.
        std::vector<assetlib::ShaderParam> typo = m.params;
        typo[1].uniform = "u_parms";
        const VerifyResult bad = verifyInterface(typo, m.samplers, rv, rf);
        CHECK(!bad.ok && bad.joined().find("u_parms") != std::string::npos,
              "a parameter naming a nonexistent uniform FAILS the cook");

        // Declared past the end of the register: the write spills into
        // whatever uniform the driver placed next.
        std::vector<assetlib::ShaderParam> over = m.params;
        over[1].offset = 7;              // u_params holds 4 floats
        const VerifyResult spill = verifyInterface(over, m.samplers, rv, rf);
        CHECK(!spill.ok && spill.joined().find("runs past") != std::string::npos,
              "a parameter past the uniform's registers FAILS the cook");

        // Types crossed both ways.
        std::vector<assetlib::ShaderParam> asSampler = m.params;
        asSampler[1].uniform = "s_baseColor";
        CHECK(!verifyInterface(asSampler, m.samplers, rv, rf).ok,
              "a value parameter pointed at a sampler FAILS");
        std::vector<assetlib::ShaderSampler> asValue = m.samplers;
        asValue[0].uniform = "u_params";
        CHECK(!verifyInterface(m.params, asValue, rv, rf).ok,
              "a sampler pointed at a vec4 FAILS");

        // Every mismatch is reported, not just the first — an author fixing
        // one typo per cook cycle is a miserable loop.
        std::vector<assetlib::ShaderParam> two = m.params;
        two[0].uniform = "u_nope1";
        two[1].uniform = "u_nope2";
        CHECK(verifyInterface(two, m.samplers, rv, rf).errors.size() == 2,
              "both mismatches are reported at once");
    }

    // ── container round-trip ────────────────────────────────────────────────
    {
        assetlib::ShaderAsset a;
        a.name     = "standard";
        a.features = { "SKINNED" };
        assetlib::ShaderParam p;
        p.name = "roughness"; p.type = assetlib::kParamFloat;
        p.uniform = "u_params"; p.offset = 1; p.defaults[0] = 0.7f;
        a.params.push_back(p);
        assetlib::ShaderSampler s;
        s.name = "baseColor"; s.uniform = "s_baseColor"; s.stage = 0;
        s.fallback = "white";
        a.samplers.push_back(s);
        a.blob = { 1, 2, 3, 4, 5, 6, 7, 8 };
        a.variants.push_back({ /*mask*/0, assetlib::kProfileMetal, 0, 4, 4, 4 });
        a.variants.push_back({ /*mask*/1, assetlib::kProfileSpirv, 0, 4, 4, 4 });

        const fs::path out = fs::temp_directory_path() / "engine_test.cshader";
        CHECK(assetlib::saveShader(a, out), "saves");

        assetlib::ShaderAsset b;
        CHECK(assetlib::loadShader(b, out), "loads");
        CHECK(b.name == a.name && b.features == a.features, "identity survives");
        CHECK(b.params.size() == 1 && b.params[0].name == "roughness"
              && b.params[0].defaults[0] == 0.7f, "params survive");
        CHECK(b.samplers.size() == 1 && b.samplers[0].fallback == "white",
              "samplers survive");
        CHECK(b.variants.size() == 2 && b.blob == a.blob, "variants + blob survive");

        CHECK(b.find(0, assetlib::kProfileMetal) != nullptr,
              "the metal variant is findable");
        CHECK(b.find(1, assetlib::kProfileMetal) == nullptr,
              "a mask/profile pair that wasn't cooked is absent, not wrong");
        uint32_t bit = 0;
        CHECK(b.featureBit("SKINNED", bit) && bit == 1, "feature bit resolves");
        CHECK(!b.featureBit("FOG", bit), "an undeclared feature has no bit");
        CHECK(b.findParam("roughness") && !b.findParam("nope"), "param lookup");

        // A variant slice pointing outside the blob must be REJECTED, not
        // handed to bgfx — that arrives as a driver crash with no provenance.
        assetlib::ShaderAsset evil = a;
        evil.variants[0].fsSize = 999;
        const fs::path bad = fs::temp_directory_path() / "engine_test_bad.cshader";
        assetlib::saveShader(evil, bad);
        assetlib::ShaderAsset loaded;
        CHECK(!assetlib::loadShader(loaded, bad),
              "a variant slice past the end of the blob is rejected");

        // Truncation at every length: never a crash, never a bogus success.
        std::vector<uint8_t> whole;
        {
            FILE* f = std::fopen(out.string().c_str(), "rb");
            std::fseek(f, 0, SEEK_END);
            whole.resize((size_t)std::ftell(f));
            std::fseek(f, 0, SEEK_SET);
            (void)std::fread(whole.data(), 1, whole.size(), f);
            std::fclose(f);
        }
        const fs::path cutPath = fs::temp_directory_path() / "engine_test_cut.cshader";
        bool anyBogus = false;
        for (size_t n = 0; n < whole.size(); ++n) {
            FILE* f = std::fopen(cutPath.string().c_str(), "wb");
            if (n) std::fwrite(whole.data(), 1, n, f);
            std::fclose(f);
            assetlib::ShaderAsset t;
            if (assetlib::loadShader(t, cutPath)) anyBogus = true;
        }
        CHECK(!anyBogus, "no truncation of a .cshader loads successfully");

        std::error_code ec;
        fs::remove(out, ec); fs::remove(bad, ec); fs::remove(cutPath, ec);
    }

    // ── profile table ───────────────────────────────────────────────────────
    {
        uint32_t p = 99;
        CHECK(assetlib::profileFromName("metal", p) && p == assetlib::kProfileMetal,
              "profile names round-trip");
        CHECK(!assetlib::profileFromName("nintendo", p), "unknown profile rejected");
        for (uint32_t i = 0; i < assetlib::kProfileCount; ++i) {
            uint32_t back = 99;
            if (!assetlib::profileFromName(assetlib::profileName(i), back)
                || back != i) {
                CHECK(false, "profile %u does not round-trip", i);
                break;
            }
        }
        CHECK(true, "every profile round-trips through its name");
        // ── The host guard, EVERY combination, from any machine ─────────
        // BUG-0029: profileCookableOnThisHost answered `p < kProfileCount` for
        // all of _WIN32 — "Windows hosts do everything" — which is true of x64
        // and false of arm64. The D3D compilers bgfx vendors are PREBUILT
        // x86-64 binaries, and an arm64 process cannot load one, so the whole
        // shader cook died with "Unable to load DXC compiler": every shader in
        // the project, on that architecture only.
        //
        // It survived because the guard answers for the host it was COMPILED
        // for, so only the macOS branch was ever exercised where the tests ran.
        // The one platform whose answer was wrong had nothing checking it.
        //
        // The rule is now a pure function over (os, arch), so this table runs
        // on every machine and a wrong answer for a platform nobody is standing
        // on still fails here.
        using namespace assetlib;
        struct Case { uint32_t os, arch, profile; bool cookable; const char* why; };
        static const Case kCases[] = {
            // Windows x64 does everything; arm64 loses BOTH D3D profiles.
            { kHostWindows, kArchX86_64, kProfileDx11,  true,  "win x64 dx11" },
            { kHostWindows, kArchX86_64, kProfileDx12,  true,  "win x64 dx12" },
            { kHostWindows, kArchArm64,  kProfileDx11,  false, "win arm64 dx11 — d3dcompiler_47.dll is x86-64" },
            { kHostWindows, kArchArm64,  kProfileDx12,  false, "win arm64 dx12 — dxcompiler.dll is x86-64" },
            { kHostWindows, kArchArm64,  kProfileSpirv, true,  "win arm64 still cooks spirv" },
            { kHostWindows, kArchArm64,  kProfileGlsl,  true,  "win arm64 still cooks glsl" },
            // Linux never cooks DXBC; DXIL only on x86-64.
            { kHostLinux,   kArchX86_64, kProfileDx11,  false, "linux dx11 — d3d4linux covers DXIL, not DXBC" },
            { kHostLinux,   kArchX86_64, kProfileDx12,  true,  "linux x86-64 dx12 via d3d4linux" },
            { kHostLinux,   kArchArm64,  kProfileDx12,  false, "linux arm64 dx12 — libdxcompiler.so is x86-64" },
            { kHostLinux,   kArchArm64,  kProfileSpirv, true,  "linux arm64 still cooks spirv" },
            // macOS has no D3D at any architecture.
            { kHostMacOs,   kArchArm64,  kProfileDx11,  false, "macOS dx11" },
            { kHostMacOs,   kArchArm64,  kProfileDx12,  false, "macOS dx12" },
            { kHostMacOs,   kArchArm64,  kProfileMetal, true,  "macOS metal" },
            { kHostMacOs,   kArchX86_64, kProfileMetal, true,  "macOS metal on intel" },
        };
        for (const auto& c : kCases)
            CHECK(profileCookableOn(c.os, c.arch, c.profile) == c.cookable,
                  "cookable(%s) == %s", c.why, c.cookable ? "true" : "false");

        // An out-of-range profile is never cookable — the cook loop iterates a
        // count, and a garbage id must not read as "sure, go ahead".
        CHECK(!profileCookableOn(kHostWindows, kArchX86_64, kProfileCount)
              && !profileCookableOn(kHostWindows, kArchX86_64, 9999u),
              "an unknown profile id is never cookable");

        // And the compiled-in answer must agree with the table for THIS host,
        // so the two cannot drift apart.
        for (uint32_t i = 0; i < kProfileCount; ++i) {
#if defined(_WIN32)
            const uint32_t os = kHostWindows;
#elif defined(__linux__)
            const uint32_t os = kHostLinux;
#elif defined(__APPLE__)
            const uint32_t os = kHostMacOs;
#else
            const uint32_t os = kHostOther;
#endif
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__amd64__)
            const uint32_t arch = kArchX86_64;
#else
            const uint32_t arch = kArchArm64;
#endif
            if (profileCookableOnThisHost(i) != profileCookableOn(os, arch, i)) {
                CHECK(false, "profileCookableOnThisHost disagrees with the table "
                             "for %s", profileName(i));
                break;
            }
        }
        CHECK(true, "the compiled-in host answer matches the table");
    }

    if (g_failures) {
        std::printf("shader_cooker_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("shader_cooker_test: PASS\n");
    return 0;
}
