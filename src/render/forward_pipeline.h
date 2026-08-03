#pragma once
// Default render pipeline: single-pass forward PBR. Owns its shader program +
// uniforms — and NOTHING ELSE. Culling, sort keys, visibility and light packing
// live in render/world (rworld::), so a project that swaps this pipeline to
// change how surfaces LOOK inherits the machinery instead of reimplementing it.
// That was the whole reason IRenderPipeline was a customization point nobody
// could use; see docs/architecture/renderer-architecture.md §3 and §5.
#include "render/render_pipeline.h"
#include "render/world/light_packing.h"
#include "render/world/visibility.h"
#include "render/shader/shader_library.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include "core/debug_draw.h"          // dbg::DebugDraw / DebugVertex (line pass)
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <set>
#include <string>

// Compiled shaders — bgfx cmake compiles per-platform; pick the right binary.
// Variable names follow bgfx convention: vs_triangle_mtl, vs_triangle_spv, etc.
// We alias them to a common name so the pipeline code stays platform-agnostic.
#if defined(__APPLE__)
    #include "metal/vs_triangle.sc.bin.h"
    #include "metal/fs_triangle.sc.bin.h"
    #include "metal/vs_shadow.sc.bin.h"
    #include "metal/fs_shadow.sc.bin.h"
    #include "metal/vs_instanced.sc.bin.h"
    #include "metal/vs_skinned.sc.bin.h"
    #include "metal/vs_shadow_instanced.sc.bin.h"
    #include "metal/vs_shadow_skinned.sc.bin.h"
    #include "metal/vs_line.sc.bin.h"
    #include "metal/fs_line.sc.bin.h"
    #define VS_LINE_DATA           vs_line_mtl
    #define VS_LINE_SIZE           sizeof(vs_line_mtl)
    #define FS_LINE_DATA           fs_line_mtl
    #define FS_LINE_SIZE           sizeof(fs_line_mtl)
    #define VS_TRIANGLE_DATA       vs_triangle_mtl
    #define VS_TRIANGLE_SIZE       sizeof(vs_triangle_mtl)
    #define FS_TRIANGLE_DATA       fs_triangle_mtl
    #define FS_TRIANGLE_SIZE       sizeof(fs_triangle_mtl)
    #define VS_SHADOW_DATA         vs_shadow_mtl
    #define VS_SHADOW_SIZE         sizeof(vs_shadow_mtl)
    #define FS_SHADOW_DATA         fs_shadow_mtl
    #define FS_SHADOW_SIZE         sizeof(fs_shadow_mtl)
    #define VS_INSTANCED_DATA      vs_instanced_mtl
    #define VS_INSTANCED_SIZE      sizeof(vs_instanced_mtl)
    #define VS_SKINNED_DATA        vs_skinned_mtl
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_mtl)
    #define VS_SHADOW_INST_DATA    vs_shadow_instanced_mtl
    #define VS_SHADOW_INST_SIZE    sizeof(vs_shadow_instanced_mtl)
    #define VS_SHADOW_SKINNED_DATA vs_shadow_skinned_mtl
    #define VS_SHADOW_SKINNED_SIZE sizeof(vs_shadow_skinned_mtl)
#elif defined(_WIN32)
    #include "dxbc/vs_triangle.sc.bin.h"
    #include "dxbc/fs_triangle.sc.bin.h"
    #include "dxbc/vs_shadow.sc.bin.h"
    #include "dxbc/fs_shadow.sc.bin.h"
    #include "dxbc/vs_instanced.sc.bin.h"
    #include "dxbc/vs_skinned.sc.bin.h"
    #include "dxbc/vs_shadow_instanced.sc.bin.h"
    #include "dxbc/vs_shadow_skinned.sc.bin.h"
    #include "dxbc/vs_line.sc.bin.h"
    #include "dxbc/fs_line.sc.bin.h"
    #define VS_LINE_DATA           vs_line_dxbc
    #define VS_LINE_SIZE           sizeof(vs_line_dxbc)
    #define FS_LINE_DATA           fs_line_dxbc
    #define FS_LINE_SIZE           sizeof(fs_line_dxbc)
    #define VS_TRIANGLE_DATA       vs_triangle_dxbc
    #define VS_TRIANGLE_SIZE       sizeof(vs_triangle_dxbc)
    #define FS_TRIANGLE_DATA       fs_triangle_dxbc
    #define FS_TRIANGLE_SIZE       sizeof(fs_triangle_dxbc)
    #define VS_SHADOW_DATA         vs_shadow_dxbc
    #define VS_SHADOW_SIZE         sizeof(vs_shadow_dxbc)
    #define FS_SHADOW_DATA         fs_shadow_dxbc
    #define FS_SHADOW_SIZE         sizeof(fs_shadow_dxbc)
    #define VS_INSTANCED_DATA      vs_instanced_dxbc
    #define VS_INSTANCED_SIZE      sizeof(vs_instanced_dxbc)
    #define VS_SKINNED_DATA        vs_skinned_dxbc
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_dxbc)
    #define VS_SHADOW_INST_DATA    vs_shadow_instanced_dxbc
    #define VS_SHADOW_INST_SIZE    sizeof(vs_shadow_instanced_dxbc)
    #define VS_SHADOW_SKINNED_DATA vs_shadow_skinned_dxbc
    #define VS_SHADOW_SKINNED_SIZE sizeof(vs_shadow_skinned_dxbc)
#else // Linux — Vulkan (SPIR-V)
    #include "spirv/vs_triangle.sc.bin.h"
    #include "spirv/fs_triangle.sc.bin.h"
    #include "spirv/vs_shadow.sc.bin.h"
    #include "spirv/fs_shadow.sc.bin.h"
    #include "spirv/vs_instanced.sc.bin.h"
    #include "spirv/vs_skinned.sc.bin.h"
    #include "spirv/vs_shadow_instanced.sc.bin.h"
    #include "spirv/vs_shadow_skinned.sc.bin.h"
    #include "spirv/vs_line.sc.bin.h"
    #include "spirv/fs_line.sc.bin.h"
    #define VS_LINE_DATA           vs_line_spv
    #define VS_LINE_SIZE           sizeof(vs_line_spv)
    #define FS_LINE_DATA           fs_line_spv
    #define FS_LINE_SIZE           sizeof(fs_line_spv)
    #define VS_TRIANGLE_DATA       vs_triangle_spv
    #define VS_TRIANGLE_SIZE       sizeof(vs_triangle_spv)
    #define FS_TRIANGLE_DATA       fs_triangle_spv
    #define FS_TRIANGLE_SIZE       sizeof(fs_triangle_spv)
    #define VS_SHADOW_DATA         vs_shadow_spv
    #define VS_SHADOW_SIZE         sizeof(vs_shadow_spv)
    #define FS_SHADOW_DATA         fs_shadow_spv
    #define FS_SHADOW_SIZE         sizeof(fs_shadow_spv)
    #define VS_INSTANCED_DATA      vs_instanced_spv
    #define VS_INSTANCED_SIZE      sizeof(vs_instanced_spv)
    #define VS_SKINNED_DATA        vs_skinned_spv
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_spv)
    #define VS_SHADOW_INST_DATA    vs_shadow_instanced_spv
    #define VS_SHADOW_INST_SIZE    sizeof(vs_shadow_instanced_spv)
    #define VS_SHADOW_SKINNED_DATA vs_shadow_skinned_spv
    #define VS_SHADOW_SKINNED_SIZE sizeof(vs_shadow_skinned_spv)
#endif

class ForwardPipeline final : public IRenderPipeline {
public:
    const char* name() const override { return "Forward PBR"; }

    // Must be called BEFORE onAttach() — the shadow map is created there and
    // is not resized afterwards. Values are validated at the project layer
    // (clamped, rounded to a power of two); this only guards against a caller
    // that bypasses it.
    void setShadowResolution(uint32_t px) {
        if (px < 256)  px = 256;
        if (px > 8192) px = 8192;
        SHADOW_SIZE = (uint16_t)px;
    }

    void onAttach(RenderContext& attachCtx) override {
        // THE COOKED SHADER IS PREFERRED. This is the point of Phase 5: the
        // program a game runs is content it ships, not a byte array the engine
        // was compiled with. `standard.shader` cooks into the project's .cache
        // as an engine default asset (CookService's second asset root).
        //
        // The compiled-in blob remains as a FALLBACK, and deliberately so — a
        // tree that has never been cooked, a bare tool, or a unit test with no
        // project must still render rather than showing a black screen. The
        // fallback goes away once every shipping path is confirmed cooked; see
        // docs/plans/renderer-audit-and-plan.md Phase 5 step 4.
        m_program = BGFX_INVALID_HANDLE;
        if (attachCtx.shaders && !attachCtx.standardShader.empty()) {
            m_program = attachCtx.shaders->program(attachCtx.standardShader,
                                                   /*featureMask*/ 0);
            // Record which path won, once, at attach. Silently falling back is
            // how "my shader edits do nothing" happens.
            m_programFromAsset = bgfx::isValid(m_program);
            std::printf("[ForwardPipeline] standard program: %s\n",
                        m_programFromAsset ? "cooked .cshader"
                                           : "compiled-in (cook failed/absent)");
        } else {
            std::printf("[ForwardPipeline] standard program: compiled-in "
                        "(no cooked shader supplied)\n");
        }
        if (!bgfx::isValid(m_program)) {
            m_program = bgfx::createProgram(
                bgfx::createShader(bgfx::makeRef(VS_TRIANGLE_DATA, VS_TRIANGLE_SIZE)),
                bgfx::createShader(bgfx::makeRef(FS_TRIANGLE_DATA, FS_TRIANGLE_SIZE)),
                true);
            m_programFromAsset = false;
        }
        m_sBaseColor   = bgfx::createUniform("s_baseColor",   bgfx::UniformType::Sampler);
        m_uParams      = bgfx::createUniform("u_params",      bgfx::UniformType::Vec4);
        m_uTexFlags    = bgfx::createUniform("u_texFlags",    bgfx::UniformType::Vec4);
        m_uColorFactor = bgfx::createUniform("u_colorFactor", bgfx::UniformType::Vec4);
        m_uLightParams = bgfx::createUniform("u_lightParams", bgfx::UniformType::Vec4);
        m_uLights      = bgfx::createUniform("u_lights",      bgfx::UniformType::Vec4, rworld::kMaxLights * 4);
        m_uCamPos      = bgfx::createUniform("u_camPos",      bgfx::UniformType::Vec4);
        m_sNormalMap   = bgfx::createUniform("s_normalMap",   bgfx::UniformType::Sampler);

        m_shadowProgram = bgfx::createProgram(
            bgfx::createShader(bgfx::makeRef(VS_SHADOW_DATA, VS_SHADOW_SIZE)),
            bgfx::createShader(bgfx::makeRef(FS_SHADOW_DATA, FS_SHADOW_SIZE)),
            true);

        // Skinned mesh programs — same fragment shaders, different vertex shaders
        m_skinnedProgram = bgfx::createProgram(
            bgfx::createShader(bgfx::makeRef(VS_SKINNED_DATA, VS_SKINNED_SIZE)),
            bgfx::createShader(bgfx::makeRef(FS_TRIANGLE_DATA, FS_TRIANGLE_SIZE)),
            true);
        m_skinnedShadowProgram = bgfx::createProgram(
            bgfx::createShader(bgfx::makeRef(VS_SHADOW_SKINNED_DATA, VS_SHADOW_SKINNED_SIZE)),
            bgfx::createShader(bgfx::makeRef(FS_SHADOW_DATA, FS_SHADOW_SIZE)),
            true);

        // Instanced variant of the standard program: same fragment shader, a
        // vertex shader that reads the model matrix from instance data instead of
        // u_model. Used for runs of consecutive draws sharing mesh AND material
        // (rworld::sameBatch) — N objects become one submit.
        m_instancedProgram = bgfx::createProgram(
            bgfx::createShader(bgfx::makeRef(VS_INSTANCED_DATA, VS_INSTANCED_SIZE)),
            bgfx::createShader(bgfx::makeRef(FS_TRIANGLE_DATA, FS_TRIANGLE_SIZE)),
            true);
        m_instancedShadowProgram = bgfx::createProgram(
            bgfx::createShader(bgfx::makeRef(VS_SHADOW_INST_DATA, VS_SHADOW_INST_SIZE)),
            bgfx::createShader(bgfx::makeRef(FS_SHADOW_DATA, FS_SHADOW_SIZE)),
            true);
        m_uBoneMatrices = bgfx::createUniform("u_boneMatrices", bgfx::UniformType::Vec4, 512);
        const uint64_t smFlags = BGFX_TEXTURE_RT
            | BGFX_SAMPLER_MIN_POINT | BGFX_SAMPLER_MAG_POINT
            | BGFX_SAMPLER_U_CLAMP   | BGFX_SAMPLER_V_CLAMP;
        m_shadowMap = bgfx::createTexture2D(SHADOW_SIZE, SHADOW_SIZE, false, 1,
                                            bgfx::TextureFormat::D32F, smFlags);
        // Report the cost. This one allocation dominates GPU memory on a
        // low-end machine, so it should never be a silent decision.
        std::printf("[Renderer] Shadow map %ux%u D32F = %.1f MB\n",
                    SHADOW_SIZE, SHADOW_SIZE,
                    (double)SHADOW_SIZE * SHADOW_SIZE * 4.0 / (1024.0 * 1024.0));
        m_shadowFB  = bgfx::createFrameBuffer(1, &m_shadowMap, false);
        m_sShadowMap    = bgfx::createUniform("s_shadowMap",    bgfx::UniformType::Sampler);
        m_uShadowMtx    = bgfx::createUniform("u_shadowMtx",    bgfx::UniformType::Mat4);
        m_uShadowParams = bgfx::createUniform("u_shadowParams", bgfx::UniformType::Vec4);

        // Debug-line pass: unlit vertex-colored line list (engineDraw*).
        m_lineProgram = bgfx::createProgram(
            bgfx::createShader(bgfx::makeRef(VS_LINE_DATA, VS_LINE_SIZE)),
            bgfx::createShader(bgfx::makeRef(FS_LINE_DATA, FS_LINE_SIZE)),
            true);
        m_lineLayout.begin()
            .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
            .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
            .end();
    }

    void onDetach() override {
        auto d = [](bgfx::UniformHandle& h){ if (bgfx::isValid(h)) bgfx::destroy(h); h = BGFX_INVALID_HANDLE; };
        d(m_sBaseColor); d(m_uParams); d(m_uTexFlags); d(m_uColorFactor); d(m_uLights);
        d(m_uLightParams); d(m_uCamPos); d(m_sNormalMap);
        d(m_sShadowMap); d(m_uShadowMtx); d(m_uShadowParams);
        d(m_uBoneMatrices);
        // A cooked program belongs to ShaderLibrary's cache (refcounted, shared
        // with any other material on the same variant); destroying it here
        // would pull it out from under them. Only the fallback is ours.
        if (!m_programFromAsset && bgfx::isValid(m_program)) bgfx::destroy(m_program);
        m_program = BGFX_INVALID_HANDLE;
        m_programFromAsset = false;
        if (bgfx::isValid(m_skinnedProgram))       { bgfx::destroy(m_skinnedProgram);       m_skinnedProgram       = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_instancedProgram))     { bgfx::destroy(m_instancedProgram);     m_instancedProgram     = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_instancedShadowProgram)) { bgfx::destroy(m_instancedShadowProgram); m_instancedShadowProgram = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_skinnedShadowProgram)) { bgfx::destroy(m_skinnedShadowProgram); m_skinnedShadowProgram = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_shadowFB))      { bgfx::destroy(m_shadowFB);      m_shadowFB      = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_shadowMap))     { bgfx::destroy(m_shadowMap);     m_shadowMap     = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_shadowProgram)) { bgfx::destroy(m_shadowProgram); m_shadowProgram = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_lineProgram))   { bgfx::destroy(m_lineProgram);   m_lineProgram   = BGFX_INVALID_HANDLE; }
    }

    // Drain the debug-line collector into view `id` (its view-transform is
    // already set). Depth-tested against the scene so lines occlude naturally;
    // no writes to depth. One transient buffer, one submit.
    void submitDebugLines(bgfx::ViewId id, RenderContext& ctx) {
        if (!ctx.debugDraw || ctx.debugDraw->empty() || !bgfx::isValid(m_lineProgram)) return;
        const auto& verts = ctx.debugDraw->vertices();
        const uint32_t count = (uint32_t)verts.size();
        if (bgfx::getAvailTransientVertexBuffer(count, m_lineLayout) < count) return;
        bgfx::TransientVertexBuffer tvb;
        bgfx::allocTransientVertexBuffer(&tvb, count, m_lineLayout);
        std::memcpy(tvb.data, verts.data(), count * sizeof(dbg::DebugVertex));
        bgfx::setVertexBuffer(0, &tvb);
        bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                       | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_PT_LINES);
        bgfx::submit(id, m_lineProgram);
    }

    void render(const RenderView& v, RenderContext& ctx) override {
        // Lights are packed FIRST, because the shadow pass and the lighting
        // shader must agree on which slot is shadowed, and only packing knows
        // the packed slot numbering (rworld::PackedLights::shadowLightIndex).
        m_submitStats.reset();   // per view; the shadow pass counts into it too
        const rworld::PackedLights lights = rworld::packLights(v.lights, v.ambient);
        renderShadow(v, ctx, lights);

        const bgfx::ViewId id = v.baseViewId;

        const uint32_t cc =
              (uint32_t)(uint8_t)(v.target.clearColor.x * 255.0f) << 24
            | (uint32_t)(uint8_t)(v.target.clearColor.y * 255.0f) << 16
            | (uint32_t)(uint8_t)(v.target.clearColor.z * 255.0f) << 8
            | (uint32_t)(uint8_t)(v.target.clearColor.w * 255.0f);
        bgfx::setViewFrameBuffer(id, v.target.fb);
        bgfx::setViewRect(id, 0, 0, v.target.w, v.target.h);
        bgfx::setViewClear(id, v.target.clearFlags, cc, v.target.clearDepth, 0);
        bgfx::setViewTransform(id, v.view.ptr(), v.proj.ptr());
        bgfx::touch(id);

        // Light packing lives in rworld::packLights now. It used to be ~30
        // lines here, which meant a project swapping the pipeline to change how
        // surfaces LOOK also had to reimplement the uniform layout — and the
        // layout was invisible to any test.
        bgfx::setUniform(m_uLights, lights.data, (uint16_t)lights.vec4Count());
        const float lp[4] = { lights.ambient, (float)lights.count, 0.0f, 0.0f };
        bgfx::setUniform(m_uLightParams, lp);
        bgfx::setUniform(m_uCamPos, v.camPos.ptr());
        bgfx::setUniform(m_uShadowMtx, m_shadowMtx);
        const float sp[4] = { m_hasShadowCaster ? 1.0f : 0.0f, SHADOW_BIAS,
                              1.0f / (float)SHADOW_SIZE, (float)m_shadowLightIndex };
        bgfx::setUniform(m_uShadowParams, sp);

        // Cull + key + sort. This is rworld::buildVisibleSet now: a pipeline is
        // a statement about how surfaces LOOK, and it should not have to own
        // visibility to make one. m_visible is reused across frames for its
        // capacity (buildVisibleSet clears it).
            rworld::buildVisibleSet(v.world(), v.camera(), m_visible);
        m_submitStats.itemsConsidered = m_visible.consideredCount;
        m_submitStats.itemsCulled     = m_visible.culledCount;

        // Runs of consecutive draws sharing mesh AND material. Counted from the
        // sorted set BEFORE submitting, because that is what instancing would
        // collapse: draws - batchRuns is the saving, stated as a number rather
        // than assumed (audit R5).
        for (std::size_t i = 0; i < m_visible.draws.size(); ) {
            i += rworld::batchRunLength(m_visible, i);
            ++m_submitStats.batchRuns;
        }

        // Walk RUNS, not individual draws. rworld::batchRunLength returns how
        // many consecutive sorted draws share mesh AND material — the sort key
        // was built so that adjacency means batchability (see world/sort_key.h),
        // and a run longer than 1 collapses into a single instanced submit.
        const bool caps = 0 != (bgfx::getCaps()->supported & BGFX_CAPS_INSTANCING);
        for (std::size_t di = 0; di < m_visible.draws.size(); ) {
            const rworld::VisibleDraw& d = m_visible.draws[di];
            const std::size_t runLen = rworld::batchRunLength(m_visible, di);
            const RenderItem& it = v.items[d.index];
            const uint64_t state = it.mesh->doubleSided
                ? (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                   BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA)
                : (BGFX_STATE_DEFAULT | BGFX_STATE_CULL_CCW);

            // Select skinned or static program
            const bool skinned = it.boneMatrices != nullptr && it.boneCount > 0;
            // The default. A data-driven material overrides it with its own
            // shader's program — which is what "a project defines its own look"
            // actually means at the draw call.
            const bgfx::ProgramHandle defaultProg = skinned ? m_skinnedProgram : m_program;
            bgfx::ProgramHandle drawProgram = defaultProg;

            // ONCE PER ITEM, not per submesh (audit R4). A bone palette belongs
            // to the skinned MESH, so every submesh of it wants the same values;
            // uploading inside bindMaterial re-sent the whole thing for each
            // range, every frame — 73 bones is 4.7 KB memcpy'd into the frame's
            // uniform buffer per submesh, for identical data.
            //
            // Safe to hoist because bgfx uniform VALUES persist across submits:
            // setUniform records an update, submit applies it, and it stays in
            // effect until something overwrites it. BGFX_DISCARD_STATE discards
            // the pending update RANGE, not the applied values — which is why the
            // view-level uniforms above (lights, camPos, shadow) can also be set
            // once before this loop and still reach every draw.
            if (skinned) {
                bgfx::setUniform(m_uBoneMatrices, it.boneMatrices,
                                 (uint16_t)(it.boneCount * 4));
                ++m_submitStats.skinnedItems;
                ++m_submitStats.bonePaletteUploads;   // ONCE per item — R4
            }

            // Resolve ONE material handle to its uniforms/textures + bind. Runs
            // per submesh, so a merged multi-material mesh draws each range with
            // its OWN material (falling back to the item's material when unset)
            // instead of the whole mesh sharing a single material.
            auto bindMaterial = [&](MaterialHandle mh) {
                ++m_submitStats.materialBinds;   // R7: one per DRAW today
                const Material* mat = mh.valid() ? ctx.materials.getMaterial(mh) : nullptr;

                // ── Data-driven: a cooked .material ─────────────────────────
                // Upload what the cook produced. No name lookup, no defaulting,
                // no validation — all of that happened offline against the
                // shader's declared interface, and repeating it here would be a
                // second source of truth that can drift from the first.
                if (mat && mat->dataDriven && ctx.shaders) {
                    const bgfx::ProgramHandle mp = programFor(*mat, ctx);
                    if (bgfx::isValid(mp)) {
                        for (const auto& b : mat->blocks) {
                            if (b.values.empty()) continue;
                            const bgfx::UniformHandle u = ctx.shaders->uniform(
                                b.name, bgfx::UniformType::Vec4,
                                (uint16_t)(b.values.size() / 4));
                            bgfx::setUniform(u, b.values.data(),
                                             (uint16_t)(b.values.size() / 4));
                        }
                        // Every DECLARED sampler binds, set or not: an unbound
                        // stage keeps whatever the previous draw left there.
                        float texFlags[4] = { 0, 0, 0, 0 };
                        for (const auto& tb : mat->textureBinds) {
                            const Texture* t = tb.texture.valid()
                                             ? ctx.textures.getTexture(tb.texture) : nullptr;
                            const bgfx::TextureHandle h = t ? t->handle
                                : (tb.fallback == "flatNormal" ? ctx.flatNormalTex
                                                               : ctx.whiteTex);
                            bgfx::setTexture((uint8_t)tb.stage,
                                ctx.shaders->uniform(tb.uniform,
                                                     bgfx::UniformType::Sampler),
                                h);
                            // Engine-driven, never authored: which optional
                            // maps are actually resident. Kept out of the
                            // material's own vec4 (see fs_triangle.sc) because a
                            // material block owns its whole register.
                            if (t && tb.stage == 0) texFlags[0] = 1.0f;
                            if (t && tb.stage == 1) texFlags[1] = 1.0f;
                        }
                        bgfx::setUniform(m_uTexFlags, texFlags);
                        bgfx::setTexture(2, m_sShadowMap, m_shadowMap);
                        bgfx::setState(mat->doubleSided
                            ? (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A
                               | BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS
                               | BGFX_STATE_MSAA)
                            : state);
                        bgfx::setTransform(it.model.ptr());
                        bgfx::setVertexBuffer(0, it.mesh->vbh);
                        // Once per material. The difference between "the
                        // .cmat loaded" and "the .cmat is what you are looking
                        // at" is exactly this branch being taken.
                        if (m_boundDataDriven.insert(mat->shaderName + "/"
                                + std::to_string(mh.id)).second)
                            std::printf("[ForwardPipeline] data-driven bind: "
                                        "material %u on shader \"%s\" "
                                        "(%zu block(s), %zu texture(s))\n",
                                        mh.id, mat->shaderName.c_str(),
                                        mat->blocks.size(),
                                        mat->textureBinds.size());
                        drawProgram = mp;
                        return;
                    }
                    // Program missing (wrong backend, uncooked variant) —
                    // ShaderLibrary already said which, once. Fall through to
                    // the fixed path rather than dropping the draw entirely.
                }

                // ── Fixed path: materials embedded in cooked geometry ───────
                const Texture*  tex = (mat && mat->hasTexture())
                                    ? ctx.textures.getTexture(mat->baseColorTexture) : nullptr;
                const Texture*  nm  = (mat && mat->normalMapTexture.valid())
                                    ? ctx.textures.getTexture(mat->normalMapTexture) : nullptr;
                const float rough = mat ? mat->roughness : 0.7f;
                const float metal = mat ? mat->metallic  : 0.0f;
                float params[4] = { 0.0f, rough, metal, 0.0f };
                float texFlags[4] = { tex ? 1.0f : 0.0f, nm ? 1.0f : 0.0f, 0, 0 };
                float factor[4] = { 1, 1, 1, 1 };
                if (mat) { factor[0]=mat->baseColorFactor[0]; factor[1]=mat->baseColorFactor[1];
                           factor[2]=mat->baseColorFactor[2]; factor[3]=mat->baseColorFactor[3]; }
                const bgfx::TextureHandle base = tex ? tex->handle : ctx.whiteTex;
                const bgfx::TextureHandle norm = nm  ? nm->handle  : ctx.flatNormalTex;
                bgfx::setUniform(m_uTexFlags, texFlags);
                bind(params, factor, base, norm, state, it);
            };

            if (drawBudgetExhausted()) { ++di; continue; }

            // ── Instanced run ───────────────────────────────────────────────
            // Restricted on purpose. Skinned items carry a per-item bone palette
            // in uniforms, so they cannot share a submit. Submeshes need per-range
            // index draws. A data-driven material supplies its OWN program, which
            // is not the instanced variant — instancing it would silently render
            // with the wrong shader, so those fall through to per-draw.
            const Material* runMat = it.material.valid()
                ? ctx.materials.getMaterial(it.material) : nullptr;
            const bool instanceable =
                   caps && runLen > 1 && !skinned
                && it.mesh->submeshes.empty()
                && !(runMat && runMat->dataDriven)
                && bgfx::isValid(m_instancedProgram);

            if (instanceable) {
                const uint16_t stride = 64;               // one mat4 per instance
                const uint32_t want   = (uint32_t)runLen;
                const uint32_t avail  =
                    bgfx::getAvailInstanceDataBuffer(want, stride);
                if (avail > 1) {
                    bgfx::InstanceDataBuffer idb;
                    bgfx::allocInstanceDataBuffer(&idb, avail, stride);
                    uint8_t* dst = idb.data;
                    for (uint32_t k = 0; k < avail; ++k) {
                        const RenderItem& ri =
                            v.items[m_visible.draws[di + k].index];
                        std::memcpy(dst, ri.model.ptr(), stride);
                        dst += stride;
                    }
                    m_instancing = true;
                    drawProgram = m_instancedProgram;
                    bindMaterial(it.material);           // shared by the whole run
                    bgfx::setInstanceDataBuffer(&idb);
                    bgfx::setIndexBuffer(it.mesh->ibh);
                    bgfx::submit(id, drawProgram);
                    m_instancing = false;
                    ++m_submitStats.draws;
                    ++m_submitStats.instancedDraws;
                    m_submitStats.instancedItems += avail;
                    di += avail;
                    continue;
                }
                // Instance buffer exhausted this frame — fall through to
                // per-draw rather than dropping the objects.
            }

            if (it.mesh->submeshes.empty()) {
                drawProgram = defaultProg;
                bindMaterial(it.material);
                bgfx::setIndexBuffer(it.mesh->ibh);
                bgfx::submit(id, drawProgram);
                ++m_submitStats.draws;
                if (skinned) ++m_submitStats.skinnedDraws;
            } else {
                for (const auto& sub : it.mesh->submeshes) {
                    if (drawBudgetExhausted()) break;
                    // Reset per submesh: each range picks its own material, so
                    // a data-driven one must not leak its program to the next.
                    drawProgram = defaultProg;
                    bindMaterial(sub.material.valid() ? sub.material : it.material);
                    bgfx::setIndexBuffer(it.mesh->ibh, sub.indexOffset, sub.indexCount);
                    bgfx::submit(id, drawProgram);
                    ++m_submitStats.draws;
                    ++m_submitStats.submeshDraws;
                    if (skinned) ++m_submitStats.skinnedDraws;
                }
            }
            ++di;
        }

        submitDebugLines(id, ctx);   // debug lines last, drawn over the meshes
    }

private:
    // The program a data-driven material wants: its own shader, its own
    // feature mask. Cached per (path, mask, profile) by ShaderLibrary, so this
    // is a hash lookup after the first draw and not a per-draw load.
    bgfx::ProgramHandle programFor(const Material& mat, RenderContext& ctx) {
        if (!ctx.shaders || mat.shaderName.empty()) return BGFX_INVALID_HANDLE;
        const auto path = ctx.shaders->resolveByName(mat.shaderName);
        if (path.empty()) {
            if (m_missingShaders.insert(mat.shaderName).second)
                std::printf("[ForwardPipeline] material wants shader \"%s\", "
                            "which is not in the cooked cache\n",
                            mat.shaderName.c_str());
            return BGFX_INVALID_HANDLE;
        }
        return ctx.shaders->program(path, mat.featureMask);
    }

    void renderShadow(const RenderView& v, RenderContext& ctx,
                      const rworld::PackedLights& lights) {
        m_hasShadowCaster = false;
        const LightItem* sun = nullptr;
        for (uint32_t i = 0; i < (uint32_t)v.lights.size(); ++i) {
            if (v.lights[i].type != LightType::Directional
                || !v.lights[i].castShadows) continue;
            // The shader indexes the PACKED array, not the source list. Packing
            // is order-preserving, so the two agree — but only below the cap. A
            // caster past kMaxLights was never uploaded, so shadowing it would
            // point the shader at some other light's direction.
            if (i >= (uint32_t)lights.count) break;
            sun = &v.lights[i];
            m_shadowLightIndex = (int)i;
            break;
        }
        if (!sun) return;
        m_hasShadowCaster = true;

        const bx::Vec3 toLight = bx::normalize(sun->direction);
        const bx::Vec3 center  { 0.0f, 0.0f, 0.0f };
        const bx::Vec3 eye     = bx::add(center, bx::mul(toLight, SHADOW_EYE_DIST));
        const bx::Vec3 up      = (std::fabs(toLight.y) > 0.99f)
                                   ? bx::Vec3{0.0f, 0.0f, 1.0f} : bx::Vec3{0.0f, 1.0f, 0.0f};
        bx::mtxLookAt(m_lightView, eye, center, up);
        const float r = SHADOW_ORTHO_RADIUS;
        bx::mtxOrtho(m_lightProj, -r, r, -r, r, SHADOW_NEAR, SHADOW_FAR, 0.0f,
                     bgfx::getCaps()->homogeneousDepth);

        const bgfx::Caps* caps = bgfx::getCaps();
        const float sy = caps->originBottomLeft ? 0.5f : -0.5f;
        const float sz = caps->homogeneousDepth ? 0.5f :  1.0f;
        const float tz = caps->homogeneousDepth ? 0.5f :  0.0f;
        const float crop[16] = {
            0.5f, 0.0f, 0.0f, 0.0f,
            0.0f, sy,   0.0f, 0.0f,
            0.0f, 0.0f, sz,   0.0f,
            0.5f, 0.5f, tz,   1.0f,
        };
        float tmp[16];
        bx::mtxMul(tmp, m_lightProj, crop);
        bx::mtxMul(m_shadowMtx, m_lightView, tmp);

        // ── Cull against the LIGHT's frustum ────────────────────────────────
        // This pass used to walk EVERY item in the scene: `shadowDraws ==
        // itemsConsidered`, measured at 2 001 shadow draws for 2 001 items, which
        // roughly doubled total submits and doubled uniform-buffer pressure.
        //
        // The light's frustum, NOT the camera's. An object behind the camera can
        // still cast a shadow into view, so reusing the camera planes here would
        // delete real shadows — the classic shadow-popping bug. These planes come
        // from m_lightView * m_lightProj, and the ortho box is bounded by
        // SHADOW_ORTHO_RADIUS, so anything outside it genuinely cannot contribute.
        float lightVp[16];
        bx::mtxMul(lightVp, m_lightView, m_lightProj);
        ViewCamera lightCam;   // global scope, not rworld:: (render_world.h)
        lightCam.view   = Mat4::from(m_lightView);
        lightCam.proj   = Mat4::from(m_lightProj);
        lightCam.camPos = { eye.x, eye.y, eye.z, 1.0f };
        rworld::extractFrustumPlanes(lightVp, lightCam.frustum);

        // The SAME buildVisibleSet the main pass uses, against the light. It
        // culls and SORTS, and sorting is what makes instancing possible here:
        // the sort key groups by mesh+material, so consecutive casters collapse
        // into one submit exactly as they do in the colour pass. A hand-rolled
        // cull loop culled but could not batch.
        rworld::buildVisibleSet(v.world(), lightCam, m_shadowVisible);
        m_submitStats.shadowItemsConsidered = m_shadowVisible.consideredCount;
        m_submitStats.shadowItemsCulled     = m_shadowVisible.culledCount;

        const bgfx::ViewId sv = ctx.shadowViewId;
        bgfx::setViewFrameBuffer(sv, m_shadowFB);
        bgfx::setViewRect(sv, 0, 0, SHADOW_SIZE, SHADOW_SIZE);
        bgfx::setViewClear(sv, BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
        bgfx::setViewTransform(sv, m_lightView, m_lightProj);
        bgfx::touch(sv);

        const uint64_t st = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CCW;
        const bool shCaps = 0 != (bgfx::getCaps()->supported & BGFX_CAPS_INSTANCING);
        for (std::size_t sdi = 0; sdi < m_shadowVisible.draws.size(); ) {
            const std::size_t runLen = rworld::batchRunLength(m_shadowVisible, sdi);
            const RenderItem& it = v.items[m_shadowVisible.draws[sdi].index];
            if (!it.mesh) { ++sdi; continue; }
            const bool skinned = it.boneMatrices != nullptr && it.boneCount > 0;

            // Instanced run. Simpler than the colour pass: the shadow program is
            // fixed, so there is no data-driven material to conflict with. Skinned
            // casters still carry a per-item bone palette in uniforms and submeshes
            // still need per-range index draws, so both fall through.
            if (shCaps && runLen > 1 && !skinned && it.mesh->submeshes.empty()
                && bgfx::isValid(m_instancedShadowProgram)) {
                const uint16_t stride = 64;
                const uint32_t avail =
                    bgfx::getAvailInstanceDataBuffer((uint32_t)runLen, stride);
                if (avail > 1) {
                    bgfx::InstanceDataBuffer idb;
                    bgfx::allocInstanceDataBuffer(&idb, avail, stride);
                    uint8_t* dst = idb.data;
                    for (uint32_t k = 0; k < avail; ++k) {
                        const RenderItem& ri =
                            v.items[m_shadowVisible.draws[sdi + k].index];
                        std::memcpy(dst, ri.model.ptr(), stride);
                        dst += stride;
                    }
                    bgfx::setState(st);
                    bgfx::setVertexBuffer(0, it.mesh->vbh);
                    bgfx::setIndexBuffer(it.mesh->ibh);
                    bgfx::setInstanceDataBuffer(&idb);
                    bgfx::submit(sv, m_instancedShadowProgram);
                    ++m_submitStats.shadowDraws;
                    ++m_submitStats.shadowInstancedDraws;
                    m_submitStats.shadowInstancedItems += avail;
                    sdi += avail;
                    continue;
                }
            }
            const bgfx::ProgramHandle shadowProg = skinned ? m_skinnedShadowProgram : m_shadowProgram;
            // Once per item here too (R4) — the shadow pass draws every submesh
            // of every caster, so it paid the same redundant upload.
            if (skinned) {
                bgfx::setUniform(m_uBoneMatrices, it.boneMatrices,
                                 (uint16_t)(it.boneCount * 4));
                ++m_submitStats.shadowBonePaletteUploads;   // once per item — R4
            }
            if (drawBudgetExhausted()) continue;
            if (it.mesh->submeshes.empty()) {
                bgfx::setState(st); bgfx::setTransform(it.model.ptr());
                bgfx::setVertexBuffer(0, it.mesh->vbh);
                bgfx::setIndexBuffer(it.mesh->ibh);
                bgfx::submit(sv, shadowProg);
                ++m_submitStats.shadowDraws;
            } else {
                for (const auto& sub : it.mesh->submeshes) {
                    if (drawBudgetExhausted()) break;
                    bgfx::setState(st); bgfx::setTransform(it.model.ptr());
                    bgfx::setVertexBuffer(0, it.mesh->vbh);
                    bgfx::setIndexBuffer(it.mesh->ibh, sub.indexOffset, sub.indexCount);
                    bgfx::submit(sv, shadowProg);
                    ++m_submitStats.shadowDraws;
                }
            }
            ++sdi;
        }
    }

    const rdiag::SubmitStats& submitStats() const override { return m_submitStats; }

    void bind(const float params[4], const float factor[4],
              bgfx::TextureHandle base, bgfx::TextureHandle norm,
              uint64_t state, const RenderItem& it) {
        bgfx::setUniform(m_uParams, params);
        bgfx::setUniform(m_uColorFactor, factor);
        bgfx::setTexture(0, m_sBaseColor, base);
        bgfx::setTexture(1, m_sNormalMap, norm);
        bgfx::setTexture(2, m_sShadowMap, m_shadowMap);
        bgfx::setState(state);
        // INSTANCED draws must not set a transform: the model matrix arrives as
        // instance data (vs_instanced.sc), and a setTransform here would be
        // ignored by that shader while still costing a matrix-cache slot.
        if (!m_instancing) bgfx::setTransform(it.model.ptr());
        bgfx::setVertexBuffer(0, it.mesh->vbh);
    }

    // True only while submitting an instanced run — see bind() and the run loop.
    bool m_instancing = false;

    // NOTE: culling moved to render/world/frustum.{h,cpp} and lighting's fixed
    // cap to rworld::kMaxLights. Replacing the fixed-cap forward path with
    // clustered forward is docs/architecture/renderer-architecture.md §2 — lights beyond the
    // cap are still dropped today.
    bgfx::ProgramHandle m_program              = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_instancedProgram     = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_instancedShadowProgram = BGFX_INVALID_HANDLE;
    // True when m_program came from a .cshader — i.e. ShaderLibrary owns it.
    bool                m_programFromAsset     = false;
    bgfx::ProgramHandle m_skinnedProgram       = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_skinnedShadowProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_lineProgram          = BGFX_INVALID_HANDLE;   // debug lines
    bgfx::VertexLayout  m_lineLayout;
    bgfx::UniformHandle m_uBoneMatrices        = BGFX_INVALID_HANDLE;
    rworld::VisibleSet  m_shadowVisible;   // light-space, culled + sorted
    rdiag::SubmitStats  m_submitStats{};

    // ── Hard ceiling on submits per frame ───────────────────────────────────
    // bgfx's Metal backend commits per-draw uniform data into a FIXED 8 MB
    // scratch buffer (renderer_mtl.cpp:23, UNIFORM_BUFFER_SIZE) at an advancing
    // offset, with NO bounds check. This pipeline writes ~1 KB of uniform traffic
    // per draw (vertex + fragment blocks, each alignment-padded), so the buffer
    // is exhausted at ~8192 draws and bgfx then writes past the end of the Metal
    // allocation: SIGSEGV inside _platform_memmove, stack in
    // RendererContextMtl::commit. Found with ASan against a generated stress
    // scene (scripts/gen_stress_scene.py); measured 8 000 objects renders 320
    // frames clean and 8 100 crashes.
    //
    // Handing bgfx a frame it cannot survive is not an option, so stop and SAY
    // SO. Dropping draws makes a frame visibly incomplete, which is bad — but a
    // loud incomplete frame beats a segfault, and `drawsDropped` makes it
    // impossible to miss. The real fix is fewer uniform bytes per draw
    // (instancing R5, material-bind dedup R7), not a bigger buffer.
    // 4096, not "just under 8192". Two reasons. The per-draw uniform cost is
    // MEASURED at ~1035 B (8 MB / the ~8100-draw empirical threshold), not the
    // round 1 KB it looks like, so a cap near the limit still overflows — 7800
    // draws was tried and still crashed. And this engine's own budget target is
    // 500 draw calls (docs/plans/renderer-audit-and-plan.md), so 4096 is already
    // 8x the envelope: any frame reaching it is far outside what the renderer is
    // designed for, and clamping there costs nothing real while leaving ~4 MB of
    // headroom against a limit we do not control.
    static constexpr uint32_t kMaxDrawsPerFrame = 4096;

    // Loud ONCE per run: a per-frame message would bury the log it belongs in.
    bool m_warnedDrawCeiling = false;
    bool drawBudgetExhausted() {
        const uint32_t total = m_submitStats.draws + m_submitStats.shadowDraws;
        if (total < kMaxDrawsPerFrame) return false;
        ++m_submitStats.drawsDropped;
        if (!m_warnedDrawCeiling) {
            m_warnedDrawCeiling = true;
            std::fprintf(stderr,
                "[ForwardPipeline] DRAW CEILING HIT: %u draws this frame. The "
                "Metal backend's uniform scratch buffer is a fixed 8 MB and this "
                "pipeline uses ~1 KB per draw, so bgfx writes past the end at "
                "~8192 draws (SIGSEGV in RendererContextMtl::commit). Refusing to "
                "submit more — THIS FRAME IS INCOMPLETE. Reduce draws (instancing) "
                "or per-draw uniforms (material-bind dedup).\n", total);
        }
        return true;
    }
    bgfx::UniformHandle m_sBaseColor   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uParams      = BGFX_INVALID_HANDLE;
    // Engine-driven texture presence, kept OUT of u_params so a material's
    // uniform block (which owns its whole vec4) cannot zero it.
    bgfx::UniformHandle m_uTexFlags    = BGFX_INVALID_HANDLE;
    // Shader names already reported missing — otherwise one bad material logs
    // per draw, per frame.
    std::set<std::string> m_missingShaders;
    // Materials already reported as bound through the data-driven path.
    std::set<std::string> m_boundDataDriven;
    bgfx::UniformHandle m_uColorFactor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLightParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uCamPos      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sNormalMap   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLights      = BGFX_INVALID_HANDLE;
    rworld::VisibleSet m_visible;   // reused across frames for its capacity
    // Shadow-map edge length. NOT a constant any more: it is the single
    // biggest VRAM lever in the engine (cost is size² × 4 bytes for D32F), and
    // the right value depends on the machine the GAME ships to. Set from
    // project.json's "graphics.shadowResolution" via setShadowResolution()
    // BEFORE init(). The old hardcoded 4096 cost 64 MB — by itself 90% of a
    // shipped game's GPU footprint, more than every mesh and texture combined.
    uint16_t SHADOW_SIZE = 2048;   // 16 MB; see ProjectContext::Graphics
    static constexpr float    SHADOW_ORTHO_RADIUS = 22.0f;  // half-width of the light box (world units) — tighten to fit the scene
    static constexpr float    SHADOW_EYE_DIST     = 60.0f;  // light-camera distance from the box center
    static constexpr float    SHADOW_NEAR         = 20.0f;  // tightened depth range -> better precision -> less acne
    static constexpr float    SHADOW_FAR          = 120.0f;
    static constexpr float    SHADOW_BIAS         = 0.0025f; // normalized-depth slope bias base
    bgfx::ProgramHandle     m_shadowProgram = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_shadowFB      = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     m_shadowMap     = BGFX_INVALID_HANDLE;
    float m_lightView[16] = {0};
    float m_lightProj[16] = {0};
    bool  m_hasShadowCaster = false;
    bgfx::UniformHandle m_sShadowMap    = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uShadowMtx    = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uShadowParams = BGFX_INVALID_HANDLE;
    float m_shadowMtx[16] = {0};
    int   m_shadowLightIndex = 0;
};
