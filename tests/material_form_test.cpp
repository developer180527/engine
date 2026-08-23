// ── material_form_test — every material has ONE shape ───────────────────────
//
// Phase 5 step 4. Until it landed, `Material` carried two representations at
// once: dedicated fields (baseColorFactor / roughness / metallic /
// baseColorTexture / normalMapTexture) for materials embedded in cooked
// geometry, and uniform blocks for cooked `.cmat` assets. `ForwardPipeline`
// branched on a `dataDriven` flag, so two upload paths existed and — the actual
// hazard — EITHER COULD BE THE ONE THAT RUNS for a given surface, decided by
// where the material happened to come from rather than by anything authored.
//
// Everything is blocks now. This gauntlet pins the properties that make that
// true, because the failure mode if it silently reverts is not a crash: it is a
// surface rendering with the wrong roughness, or with the previous draw's
// texture, and nothing reports it.
//
// Deliberately a UNIT test with no GPU, no project and no cooked content: what
// is being asserted is the SHAPE of the data the renderer consumes, and that is
// decided entirely by Material::standard.
#include <cstdio>
#include <cstring>
#include <string>

#include "render/material.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static const Material::UniformBlock* find(const Material& m, const char* name) {
    for (const auto& b : m.blocks) if (b.name == name) return &b;
    return nullptr;
}
static const Material::TextureBind* bindAt(const Material& m, uint32_t stage) {
    for (const auto& t : m.textureBinds) if (t.stage == stage) return &t;
    return nullptr;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("material_form_test: one material representation\n");

    const float factor[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    TextureHandle base, norm;   // invalid: the untextured case is the common one
    Material m = Material::standard(factor, 0.3f, 0.8f, base, norm);

    // ── The declared interface, exactly ─────────────────────────────────────
    // These names and offsets mirror shaders/standard.shader. If the manifest
    // and this synthesis ever disagree, a mesh-embedded material uploads into
    // the wrong register and the surface renders with someone else's values —
    // silently, because bgfx has no idea what the float was meant to be.
    {
        const auto* color = find(m, "u_colorFactor");
        CHECK(color != nullptr, "u_colorFactor block exists");
        if (color) {
            CHECK(color->values.size() == 4,
                  "u_colorFactor is a FULL vec4 (%zu) — a short block leaves the "
                  "tail holding the previous draw's bytes", color->values.size());
            CHECK(color->values[0] == 0.25f && color->values[1] == 0.5f
               && color->values[2] == 0.75f && color->values[3] == 1.0f,
                  "...carrying the authored base colour verbatim");
        }

        const auto* params = find(m, "u_params");
        CHECK(params != nullptr, "u_params block exists");
        if (params) {
            CHECK(params->values.size() == 4,
                  "u_params is a FULL vec4 (%zu)", params->values.size());
            // standard.shader: x=hasBaseColor y=roughness z=metallic w=hasNormalMap.
            // x and w are ENGINE-set every draw, so synthesis must leave them
            // zeroed rather than guessing — the renderer is the only writer that
            // may decide texture residency.
            CHECK(params->values[1] == 0.3f, "u_params.y is roughness");
            CHECK(params->values[2] == 0.8f, "u_params.z is metallic");
            CHECK(params->values[0] == 0.0f && params->values[3] == 0.0f,
                  "u_params.x/.w are left to the engine (texture residency flags)");
        }
    }

    // ── Both samplers are always DECLARED ───────────────────────────────────
    // Even with no texture. An undeclared stage is not bound at all, and bgfx
    // keeps whatever the previous draw left there — so an untextured material
    // would inherit the last textured one's albedo. The fallback name is what
    // turns that into white.
    {
        const auto* s0 = bindAt(m, 0);
        const auto* s1 = bindAt(m, 1);
        CHECK(s0 && s0->uniform == "s_baseColor",
              "stage 0 declares s_baseColor even when untextured");
        CHECK(s1 && s1->uniform == "s_normalMap",
              "stage 1 declares s_normalMap even when untextured");
        CHECK(s0 && s0->fallback == "white",  "...falling back to white");
        CHECK(s1 && s1->fallback == "flatNormal", "...and to flatNormal");
        CHECK(m.textureBinds.size() == 2,
              "exactly two sampler binds (%zu)", m.textureBinds.size());
    }

    // ── The program choice, which decides whether a draw can instance ───────
    // A synthesized material leaves shaderName EMPTY on purpose: naming
    // "standard" would make the pipeline resolve the cooked shader asset, which
    // is not the instanced variant — so every mesh-embedded material would drop
    // out of instanced runs the moment that shader happened to be cooked. That
    // is the R18/R5 submission win (3 067 draws -> 299) disappearing based on
    // the presence of a file.
    CHECK(m.shaderName.empty(),
          "a synthesized material names no shader, so it uses the built-in "
          "program and stays instanceable");

    // ── The typed views are a WINDOW, not a copy ────────────────────────────
    // This is the property that killed the old dual representation: the editor
    // inspector writes through these, and the renderer uploads the blocks. If
    // they were separate storage the slider would move a shadow copy and the
    // surface would not change — which is precisely the bug class the migration
    // exists to make impossible.
    {
        float* rough = m.roughness();
        CHECK(rough != nullptr, "roughness() resolves on the standard interface");
        if (rough) {
            *rough = 0.11f;
            const auto* params = find(m, "u_params");
            CHECK(params && params->values[1] == 0.11f,
                  "writing through roughness() edits the BLOCK the renderer "
                  "uploads, not a copy of it");
        }
        float* color = m.baseColorFactor();
        CHECK(color != nullptr, "baseColorFactor() resolves");
        if (color) {
            color[2] = 0.42f;
            const auto* c = find(m, "u_colorFactor");
            CHECK(c && c->values[2] == 0.42f, "...and so does baseColorFactor()");
        }
    }

    // ── A material on somebody else's shader ────────────────────────────────
    // The inspector is hard-coded to the standard interface, so it has to cope
    // with a material that does not have one. Null is the honest answer; the
    // panel greys the row rather than editing a parameter that does not exist.
    {
        Material other;
        other.shaderName = "acme_toon";
        other.blocks.push_back({ "u_toonRamp", { 1, 0, 0, 0 } });
        CHECK(other.roughness() == nullptr,
              "roughness() is null on a shader that does not declare it");
        CHECK(other.baseColorFactor() == nullptr, "...and so is baseColorFactor()");
        CHECK(other.textureAt(0).valid() == false,
              "textureAt() on an unbound stage is invalid, not a crash");
    }

    // ── A block too short to hold the parameter ─────────────────────────────
    // Guards the accessor's arity check. Reading u_params.z out of a 2-float
    // block would be an out-of-bounds read into the vector's tail.
    {
        Material stunted;
        stunted.blocks.push_back({ "u_params", { 0.0f, 0.5f } });
        CHECK(stunted.metallic() == nullptr,
              "metallic() refuses a u_params block too short to contain it");
    }

    if (g_failures) {
        std::printf("\nmaterial_form_test: FAIL — %d\n", g_failures);
        return 1;
    }
    std::printf("\nmaterial_form_test: PASS\n");
    return 0;
}
