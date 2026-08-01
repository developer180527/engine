// ── material_cooker_test — materials as data ────────────────────────────────
//
// Phase 5 of docs/plans/renderer-audit-and-plan.md. This is the payoff of the shader
// interface: a material names a shader and supplies values, and EVERY name is
// checked against what that shader declared. What is asserted here is precisely
// the set of mistakes that would otherwise be invisible — a misspelled
// parameter, a wrong-arity value, an undeclared sampler, a feature with no
// cooked variant — plus the one correctness property the runtime depends on:
// uniform blocks are COMPLETE, so an unset parameter reads its default rather
// than the previous draw's register.
//
// Hermetic: resolution and the container are pure functions over PODs.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <assetlib/material_asset.h>
#include <assetlib/shader_asset.h>

#include "assets/cookers/material/material_manifest.h"
#include "assets/cookers/material/material_resolve.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using namespace matcook;
namespace fs = std::filesystem;

// The standard shader's interface, as shaders/standard.shader declares it.
static std::vector<assetlib::ShaderParam> stdParams() {
    std::vector<assetlib::ShaderParam> p(3);
    p[0].name = "baseColorFactor"; p[0].type = assetlib::kParamColor;
    p[0].uniform = "u_colorFactor"; p[0].offset = 0;
    p[0].defaults[0] = p[0].defaults[1] = p[0].defaults[2] = p[0].defaults[3] = 1.0f;

    p[1].name = "roughness"; p[1].type = assetlib::kParamFloat;
    p[1].uniform = "u_params"; p[1].offset = 1; p[1].defaults[0] = 0.7f;

    p[2].name = "metallic"; p[2].type = assetlib::kParamFloat;
    p[2].uniform = "u_params"; p[2].offset = 2; p[2].defaults[0] = 0.0f;
    return p;
}
static std::vector<assetlib::ShaderSampler> stdSamplers() {
    std::vector<assetlib::ShaderSampler> s(2);
    s[0].name = "baseColor"; s[0].uniform = "s_baseColor"; s[0].stage = 0;
    s[0].fallback = "white";
    s[1].name = "normalMap"; s[1].uniform = "s_normalMap"; s[1].stage = 1;
    s[1].fallback = "flatNormal";
    return s;
}

int main() {
    std::printf("material_cooker_test\n");
    const std::vector<std::string> feats = { "SKINNED", "FOG" };

    // ── manifest parsing ────────────────────────────────────────────────────
    {
        MaterialManifest m;
        auto r = parseMaterialManifest(
            R"({ "shader": "shaders/standard.shader",
                 "name": "rust",
                 "doubleSided": true,
                 "features": ["SKINNED"],
                 "parameters": { "roughness": 0.25,
                                 "baseColorFactor": [0.9, 0.2, 0.1, 1.0] },
                 "textures": { "baseColor": "textures/rust.png" } })", m);
        CHECK(r.ok, "a well-formed material parses (%s)", r.error.c_str());
        CHECK(m.shaderRef == "shaders/standard.shader", "shader ref read");
        CHECK(m.name == "rust" && m.doubleSided, "name + doubleSided read");
        CHECK(m.input.values.size() == 2, "2 parameters read");
        CHECK(m.input.textures.size() == 1 && m.input.features.size() == 1,
              "1 texture, 1 feature read");

        CHECK(!parseMaterialManifest("{ not json", m).ok, "malformed JSON rejected");
        // A material with no shader must FAIL rather than defaulting: a lost
        // shader reference would otherwise render as something else entirely,
        // quietly.
        auto noShader = parseMaterialManifest(R"({"parameters":{}})", m);
        CHECK(!noShader.ok && noShader.error.find("shader") != std::string::npos,
              "a material with no shader is rejected");
        CHECK(!parseMaterialManifest(
            R"({"shader":"s","parameters":{"a":[1,2,3,4,5]}})", m).ok,
            "more than 4 values is rejected");
        CHECK(!parseMaterialManifest(
            R"({"shader":"s","parameters":{"a":"red"}})", m).ok,
            "a non-numeric parameter is rejected");
        CHECK(!parseMaterialManifest(
            R"({"shader":"s","textures":{"a":7}})", m).ok,
            "a non-string texture path is rejected");
        CHECK(!parseMaterialManifest(
            R"({"shader":"s","features":["A","A"]})", m).ok,
            "a duplicate feature is rejected");
    }

    // ── resolution: the happy path ──────────────────────────────────────────
    {
        ResolveInput in;
        in.values.push_back({ "roughness", { 0.25f }, 1 });
        in.textures.push_back({ "baseColor", "textures/rust.png" });
        in.features.push_back("FOG");

        assetlib::MaterialAsset out;
        const auto r = resolveMaterial(stdParams(), stdSamplers(), feats, in, out);
        CHECK(r.ok, "a valid material resolves (%s)", r.joined(" | ").c_str());

        const auto* up = out.findUniform("u_params");
        CHECK(up && up->values.size() == 4,
              "u_params is a whole vec4 (%zu floats)", up ? up->values.size() : 0);
        CHECK(up && up->values[1] == 0.25f, "the authored roughness lands at offset 1");
        // THE property the runtime depends on. metallic was never set by this
        // material; if the block were sparse, offset 2 would hold whatever the
        // previous draw wrote — "looks right alone, wrong next to another
        // object".
        CHECK(up && up->values[2] == 0.0f,
              "an UNSET parameter holds the shader's default, not stale data");
        const auto* uc = out.findUniform("u_colorFactor");
        CHECK(uc && uc->values.size() == 4 && uc->values[0] == 1.0f
              && uc->values[3] == 1.0f,
              "an entirely unset uniform is still fully populated from defaults");

        // Every DECLARED sampler binds, set or not — an unset stage would
        // otherwise keep the previous draw's texture.
        CHECK(out.textures.size() == 2, "both declared samplers bind (%zu)",
              out.textures.size());
        const auto* base = out.findTexture("s_baseColor");
        CHECK(base && base->path == "textures/rust.png" && base->stage == 0,
              "the authored texture binds to its declared stage");
        const auto* norm = out.findTexture("s_normalMap");
        CHECK(norm && norm->path.empty() && norm->fallback == "flatNormal",
              "an unset sampler carries its fallback");

        CHECK(out.featureMask == 0x2, "FOG is bit 1 (mask 0x%x)", out.featureMask);
    }

    // ── resolution: splat, and the mistakes ─────────────────────────────────
    {
        ResolveInput in;
        in.values.push_back({ "baseColorFactor", { 0.5f }, 1 });
        assetlib::MaterialAsset out;
        CHECK(resolveMaterial(stdParams(), stdSamplers(), feats, in, out).ok
              && out.findUniform("u_colorFactor")->values[3] == 0.5f,
              "a single number splats across a vector (grey = 0.5)");
    }
    {
        // The typo. Ignoring an unknown key is how a material silently looks
        // wrong with nothing reporting anything.
        ResolveInput in;
        in.values.push_back({ "roughtness", { 0.25f }, 1 });
        assetlib::MaterialAsset out;
        const auto r = resolveMaterial(stdParams(), stdSamplers(), feats, in, out);
        CHECK(!r.ok && r.joined().find("roughtness") != std::string::npos,
              "a misspelled parameter FAILS the cook");
        CHECK(r.joined().find("roughness") != std::string::npos,
              "...and the error lists what IS declared, so the fix is obvious");
    }
    {
        ResolveInput in;
        in.values.push_back({ "roughness", { 1, 2, 3, 0 }, 3 });
        assetlib::MaterialAsset out;
        const auto r = resolveMaterial(stdParams(), stdSamplers(), feats, in, out);
        CHECK(!r.ok && r.joined().find("3 were given") != std::string::npos,
              "3 values into a float FAILS rather than silently truncating");
    }
    {
        ResolveInput in;
        in.textures.push_back({ "emissive", "x.png" });
        assetlib::MaterialAsset out;
        const auto r = resolveMaterial(stdParams(), stdSamplers(), feats, in, out);
        CHECK(!r.ok && r.joined().find("emissive") != std::string::npos,
              "a texture the shader has no sampler for FAILS");
    }
    {
        // No variant was cooked for an undeclared feature, so honouring it
        // would mean loading a program that does not exist.
        ResolveInput in;
        in.features.push_back("PARALLAX");
        assetlib::MaterialAsset out;
        const auto r = resolveMaterial(stdParams(), stdSamplers(), feats, in, out);
        CHECK(!r.ok && r.joined().find("PARALLAX") != std::string::npos,
              "a feature the shader never declared FAILS");
    }
    {
        // All of them at once — an author fixing one error per cook cycle is a
        // miserable loop.
        ResolveInput in;
        in.values.push_back({ "nope1", { 1 }, 1 });
        in.values.push_back({ "nope2", { 1 }, 1 });
        in.textures.push_back({ "nope3", "x.png" });
        assetlib::MaterialAsset out;
        CHECK(resolveMaterial(stdParams(), stdSamplers(), feats, in, out)
                  .errors.size() == 3,
              "every mismatch is reported in one pass");
    }
    {
        // A shader with no parameters at all is legitimate (an unlit blit).
        ResolveInput in;
        assetlib::MaterialAsset out;
        const auto r = resolveMaterial({}, {}, {}, in, out);
        CHECK(r.ok && out.uniforms.empty() && out.textures.empty(),
              "a shader with no declared interface resolves to an empty material");
    }

    // ── container round-trip ────────────────────────────────────────────────
    {
        assetlib::MaterialAsset a;
        a.name = "rust";
        a.shaderPath = "shaders/standard.shader";
        a.featureMask = 0x3;
        a.doubleSided = true;
        a.uniforms.push_back({ "u_params", { 0.0f, 0.25f, 1.0f, 0.0f } });
        a.textures.push_back({ "s_baseColor", 0, "textures/rust.png", "white" });

        const fs::path out = fs::temp_directory_path() / "engine_test.cmat";
        CHECK(assetlib::saveMaterial(a, out), "saves");

        assetlib::MaterialAsset b;
        CHECK(assetlib::loadMaterial(b, out), "loads");
        CHECK(b.name == a.name && b.shaderPath == a.shaderPath
              && b.featureMask == 0x3 && b.doubleSided, "identity survives");
        CHECK(b.uniforms.size() == 1 && b.uniforms[0].values[1] == 0.25f,
              "uniform values survive");
        CHECK(b.textures.size() == 1 && b.textures[0].fallback == "white",
              "texture bindings survive");

        // A block that isn't a whole number of vec4s makes the register count
        // ambiguous and would read past the vector on upload.
        assetlib::MaterialAsset ragged = a;
        ragged.uniforms[0].values.resize(3);
        const fs::path bad = fs::temp_directory_path() / "engine_test_bad.cmat";
        assetlib::saveMaterial(ragged, bad);
        assetlib::MaterialAsset loaded;
        CHECK(!assetlib::loadMaterial(loaded, bad),
              "a uniform block that isn't vec4-aligned is rejected");

        std::vector<uint8_t> whole;
        {
            FILE* f = std::fopen(out.string().c_str(), "rb");
            std::fseek(f, 0, SEEK_END);
            whole.resize((size_t)std::ftell(f));
            std::fseek(f, 0, SEEK_SET);
            (void)std::fread(whole.data(), 1, whole.size(), f);
            std::fclose(f);
        }
        const fs::path cut = fs::temp_directory_path() / "engine_test_cut.cmat";
        bool anyBogus = false;
        for (size_t n = 0; n < whole.size(); ++n) {
            FILE* f = std::fopen(cut.string().c_str(), "wb");
            if (n) std::fwrite(whole.data(), 1, n, f);
            std::fclose(f);
            assetlib::MaterialAsset t;
            if (assetlib::loadMaterial(t, cut)) anyBogus = true;
        }
        CHECK(!anyBogus, "no truncation of a .cmat loads successfully");

        std::error_code ec;
        fs::remove(out, ec); fs::remove(bad, ec); fs::remove(cut, ec);
    }

    if (g_failures) {
        std::printf("material_cooker_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("material_cooker_test: PASS\n");
    return 0;
}
