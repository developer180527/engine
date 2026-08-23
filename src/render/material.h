#pragma once
#include "core/handle.h"

#include <cstdint>
#include <string>
#include <vector>

// ── Material — one representation, not two ───────────────────────────────────
//
// A material is uniform BLOCKS plus TEXTURE BINDS, both resolved offline by the
// cooker against a shader's declared interface. The runtime uploads what it is
// given: no name lookup, no defaulting, no validation. A misspelled parameter is
// a failed COOK, not a silent no-op at 60 Hz.
//
// ── What this used to be, and why it mattered ───────────────────────────────
// Until Phase 5 step 4 this struct carried BOTH forms: fixed fields
// (baseColorFactor / roughness / metallic / baseColorTexture / normalMapTexture)
// for materials embedded in cooked geometry, and blocks for cooked `.cmat`
// assets. The pipeline branched on a `dataDriven` flag, so two upload paths
// existed and — the actual hazard — *either could be the one that runs* for a
// given surface, decided by where the material happened to come from.
//
// Everything now becomes blocks at load time: AssetService::standardMaterial()
// synthesizes the standard shader's declared form from the importer's values, so
// mesh-embedded and asset-authored materials are the same thing by the time the
// renderer sees them. One path, one source of truth.
//
// The cost of the old shape was not performance, it was doubt. Two paths meant
// no answer to "what is this surface actually rendering with" short of reading
// the branch.
struct Material {
    // A uniform block, complete and vec4-aligned. Never sparse: the cooker fills
    // every component, including defaults, so a partial upload cannot leave a
    // register holding the previous draw's value.
    struct UniformBlock {
        std::string        name;     // "u_params"
        std::vector<float> values;   // vec4-aligned, COMPLETE
    };
    struct TextureBind {
        std::string   uniform;       // "s_baseColor"
        uint32_t      stage = 0;
        TextureHandle texture;       // invalid -> bind `fallback`
        std::string   fallback;      // "white" | "flatNormal"
    };

    std::string               shaderName;      // resolved by ShaderLibrary
    uint32_t                  featureMask = 0;
    bool                      doubleSided = false;
    std::vector<UniformBlock> blocks;
    std::vector<TextureBind>  textureBinds;

    // Author-facing filenames, for the inspector. Display only — nothing in the
    // render path reads them.
    std::string baseColorName;
    std::string normalMapName;

    // ── The standard shader's declared interface ────────────────────────────
    // Mirrors shaders/standard.shader. Kept here rather than parsed at runtime
    // because synthesis has to produce exactly what the cooked form produces,
    // and the two agreeing by construction is the point of the migration.
    //
    //   baseColorFactor  color  u_colorFactor  offset 0
    //   roughness        float  u_params       offset 1
    //   metallic         float  u_params       offset 2
    //
    // u_params x and w are engine-set flags (hasBaseColor / hasNormalMap), which
    // is why only y and z are material-settable.
    static constexpr const char* kStdName        = "standard";
    static constexpr const char* kStdColorBlock  = "u_colorFactor";
    static constexpr const char* kStdParamsBlock = "u_params";
    static constexpr uint32_t    kStdRoughness   = 1;
    static constexpr uint32_t    kStdMetallic    = 2;

    // The values the fixed struct used to default to, now written explicitly.
    // They exist as constants because three import paths relied on them
    // IMPLICITLY — by never assigning the members — and an implicit default is
    // invisible at the call site, which is how it survives a migration
    // unnoticed. See the note on the glTF importer.
    static constexpr float kStdDefaultRoughness = 0.7f;
    static constexpr float kStdDefaultMetallic  = 0.0f;

    // Builds the standard form from the values an importer or mesh cooker
    // produced. Both sampler stages are ALWAYS declared, set or not: an unbound
    // stage keeps whatever the previous draw left there, so the fallback is what
    // makes an untextured material render white instead of inheriting.
    static Material standard(const float baseColorFactor[4],
                             float roughness, float metallic,
                             TextureHandle baseColor = {},
                             TextureHandle normalMap = {});

    // ── Typed views over the blocks ─────────────────────────────────────────
    // The blocks are the truth; these are a window onto them, so an editor
    // slider writing through one of these edits the same bytes the renderer
    // uploads. Null when this material's shader does not declare the parameter —
    // which is the honest answer for a material on somebody else's shader, and
    // the reason the inspector has to check rather than assume.
    float*       block(const char* name, uint32_t minComponents = 1);
    const float* block(const char* name, uint32_t minComponents = 1) const;

    float* baseColorFactor() { return block(kStdColorBlock, 4); }
    float* roughness() { float* p = block(kStdParamsBlock, 4); return p ? p + kStdRoughness : nullptr; }
    float* metallic()  { float* p = block(kStdParamsBlock, 4); return p ? p + kStdMetallic  : nullptr; }

    // The texture bound at a sampler stage, or an invalid handle. Stage, not
    // index: textureBinds is ordered by whatever the cooker emitted.
    TextureHandle textureAt(uint32_t stage) const;
    bool hasTexture() const { return textureAt(0).valid(); }
};

inline float* Material::block(const char* name, uint32_t minComponents) {
    if (!name) return nullptr;
    for (auto& b : blocks)
        if (b.name == name && b.values.size() >= minComponents)
            return b.values.data();
    return nullptr;
}

inline const float* Material::block(const char* name, uint32_t minComponents) const {
    return const_cast<Material*>(this)->block(name, minComponents);
}

inline TextureHandle Material::textureAt(uint32_t stage) const {
    for (const auto& t : textureBinds)
        if (t.stage == stage) return t.texture;
    return {};
}

inline Material Material::standard(const float baseColorFactor[4],
                                   float roughness, float metallic,
                                   TextureHandle baseColor,
                                   TextureHandle normalMap) {
    Material m;
    // shaderName stays EMPTY, and that is a deliberate distinction between the
    // INTERFACE and the PROGRAM. The blocks below are exactly the standard
    // shader's declared interface — standard.shader *is* fs_triangle.sc — so
    // they upload correctly against the cooked shader asset and against the
    // compiled-in program alike.
    //
    // Naming "standard" here would make ForwardPipeline resolve the cooked
    // asset, which is not the INSTANCED variant, so every mesh-embedded material
    // would drop out of instanced runs the moment that shader was cooked. That
    // is the R18/R5 win (3 067 draws -> 299) disappearing based on whether an
    // asset happens to exist. Pointing these at the cooked standard shader needs
    // an instanced variant first; until then the built-in program is the correct
    // choice and the uniforms are identical either way.

    // Full vec4s, always. A block shorter than its register would leave the tail
    // holding the previous draw's bytes.
    m.blocks.push_back({ kStdColorBlock,
                         { baseColorFactor[0], baseColorFactor[1],
                           baseColorFactor[2], baseColorFactor[3] } });
    // x and w are engine-set every draw (u_texFlags carries residency); they are
    // written as 0 here so the block is complete and the renderer's overwrite is
    // the only writer that matters.
    m.blocks.push_back({ kStdParamsBlock, { 0.0f, roughness, metallic, 0.0f } });

    m.textureBinds.push_back({ "s_baseColor", 0, baseColor,  "white" });
    m.textureBinds.push_back({ "s_normalMap", 1, normalMap,  "flatNormal" });
    return m;
}
