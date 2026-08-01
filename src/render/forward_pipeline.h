#pragma once
// Default render pipeline: single-pass forward PBR. Owns its shader program +
// uniforms — and NOTHING ELSE. Culling, sort keys, visibility and light packing
// live in render/world (rworld::), so a project that swaps this pipeline to
// change how surfaces LOOK inherits the machinery instead of reimplementing it.
// That was the whole reason IRenderPipeline was a customization point nobody
// could use; see docs/renderer-architecture.md §3 and §5.
#include "render/render_pipeline.h"
#include "render/world/light_packing.h"
#include "render/world/visibility.h"
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

// Compiled shaders — bgfx cmake compiles per-platform; pick the right binary.
// Variable names follow bgfx convention: vs_triangle_mtl, vs_triangle_spv, etc.
// We alias them to a common name so the pipeline code stays platform-agnostic.
#if defined(__APPLE__)
    #include "metal/vs_triangle.sc.bin.h"
    #include "metal/fs_triangle.sc.bin.h"
    #include "metal/vs_shadow.sc.bin.h"
    #include "metal/fs_shadow.sc.bin.h"
    #include "metal/vs_skinned.sc.bin.h"
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
    #define VS_SKINNED_DATA        vs_skinned_mtl
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_mtl)
    #define VS_SHADOW_SKINNED_DATA vs_shadow_skinned_mtl
    #define VS_SHADOW_SKINNED_SIZE sizeof(vs_shadow_skinned_mtl)
#elif defined(_WIN32)
    #include "dxbc/vs_triangle.sc.bin.h"
    #include "dxbc/fs_triangle.sc.bin.h"
    #include "dxbc/vs_shadow.sc.bin.h"
    #include "dxbc/fs_shadow.sc.bin.h"
    #include "dxbc/vs_skinned.sc.bin.h"
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
    #define VS_SKINNED_DATA        vs_skinned_dxbc
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_dxbc)
    #define VS_SHADOW_SKINNED_DATA vs_shadow_skinned_dxbc
    #define VS_SHADOW_SKINNED_SIZE sizeof(vs_shadow_skinned_dxbc)
#else // Linux — Vulkan (SPIR-V)
    #include "spirv/vs_triangle.sc.bin.h"
    #include "spirv/fs_triangle.sc.bin.h"
    #include "spirv/vs_shadow.sc.bin.h"
    #include "spirv/fs_shadow.sc.bin.h"
    #include "spirv/vs_skinned.sc.bin.h"
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
    #define VS_SKINNED_DATA        vs_skinned_spv
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_spv)
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

    void onAttach(RenderContext&) override {
        m_program = bgfx::createProgram(
            bgfx::createShader(bgfx::makeRef(VS_TRIANGLE_DATA, VS_TRIANGLE_SIZE)),
            bgfx::createShader(bgfx::makeRef(FS_TRIANGLE_DATA, FS_TRIANGLE_SIZE)),
            true);
        m_sBaseColor   = bgfx::createUniform("s_baseColor",   bgfx::UniformType::Sampler);
        m_uParams      = bgfx::createUniform("u_params",      bgfx::UniformType::Vec4);
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
        d(m_sBaseColor); d(m_uParams); d(m_uColorFactor); d(m_uLights);
        d(m_uLightParams); d(m_uCamPos); d(m_sNormalMap);
        d(m_sShadowMap); d(m_uShadowMtx); d(m_uShadowParams);
        d(m_uBoneMatrices);
        if (bgfx::isValid(m_program)) { bgfx::destroy(m_program); m_program = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(m_skinnedProgram))       { bgfx::destroy(m_skinnedProgram);       m_skinnedProgram       = BGFX_INVALID_HANDLE; }
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

        for (const rworld::VisibleDraw& d : m_visible.draws) {
            const RenderItem& it = v.items[d.index];
            const uint64_t state = it.mesh->doubleSided
                ? (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                   BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA)
                : (BGFX_STATE_DEFAULT | BGFX_STATE_CULL_CCW);

            // Select skinned or static program
            const bool skinned = it.boneMatrices != nullptr && it.boneCount > 0;
            const bgfx::ProgramHandle prog = skinned ? m_skinnedProgram : m_program;

            // Resolve ONE material handle to its uniforms/textures + bind. Runs
            // per submesh, so a merged multi-material mesh draws each range with
            // its OWN material (falling back to the item's material when unset)
            // instead of the whole mesh sharing a single material.
            auto bindMaterial = [&](MaterialHandle mh) {
                const Material* mat = mh.valid() ? ctx.materials.getMaterial(mh) : nullptr;
                const Texture*  tex = (mat && mat->hasTexture())
                                    ? ctx.textures.getTexture(mat->baseColorTexture) : nullptr;
                const Texture*  nm  = (mat && mat->normalMapTexture.valid())
                                    ? ctx.textures.getTexture(mat->normalMapTexture) : nullptr;
                const float rough = mat ? mat->roughness : 0.7f;
                const float metal = mat ? mat->metallic  : 0.0f;
                float params[4] = { tex ? 1.0f : 0.0f, rough, metal, nm ? 1.0f : 0.0f };
                float factor[4] = { 1, 1, 1, 1 };
                if (mat) { factor[0]=mat->baseColorFactor[0]; factor[1]=mat->baseColorFactor[1];
                           factor[2]=mat->baseColorFactor[2]; factor[3]=mat->baseColorFactor[3]; }
                const bgfx::TextureHandle base = tex ? tex->handle : ctx.whiteTex;
                const bgfx::TextureHandle norm = nm  ? nm->handle  : ctx.flatNormalTex;
                if (skinned)
                    bgfx::setUniform(m_uBoneMatrices, it.boneMatrices, (uint16_t)(it.boneCount * 4));
                bind(params, factor, base, norm, state, it);
            };

            if (it.mesh->submeshes.empty()) {
                bindMaterial(it.material);
                bgfx::setIndexBuffer(it.mesh->ibh);
                bgfx::submit(id, prog);
            } else {
                for (const auto& sub : it.mesh->submeshes) {
                    bindMaterial(sub.material.valid() ? sub.material : it.material);
                    bgfx::setIndexBuffer(it.mesh->ibh, sub.indexOffset, sub.indexCount);
                    bgfx::submit(id, prog);
                }
            }
        }

        submitDebugLines(id, ctx);   // debug lines last, drawn over the meshes
    }

private:
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

        const bgfx::ViewId sv = ctx.shadowViewId;
        bgfx::setViewFrameBuffer(sv, m_shadowFB);
        bgfx::setViewRect(sv, 0, 0, SHADOW_SIZE, SHADOW_SIZE);
        bgfx::setViewClear(sv, BGFX_CLEAR_DEPTH, 0x00000000, 1.0f, 0);
        bgfx::setViewTransform(sv, m_lightView, m_lightProj);
        bgfx::touch(sv);

        const uint64_t st = BGFX_STATE_WRITE_Z | BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_CULL_CCW;
        for (uint32_t i = 0; i < (uint32_t)v.items.size(); ++i) {
            const RenderItem& it = v.items[i];
            if (!it.mesh) continue;
            const bool skinned = it.boneMatrices != nullptr && it.boneCount > 0;
            const bgfx::ProgramHandle shadowProg = skinned ? m_skinnedShadowProgram : m_shadowProgram;
            if (it.mesh->submeshes.empty()) {
                if (skinned)
                    bgfx::setUniform(m_uBoneMatrices, it.boneMatrices, (uint16_t)(it.boneCount * 4));
                bgfx::setState(st); bgfx::setTransform(it.model.ptr());
                bgfx::setVertexBuffer(0, it.mesh->vbh);
                bgfx::setIndexBuffer(it.mesh->ibh);
                bgfx::submit(sv, shadowProg);
            } else {
                for (const auto& sub : it.mesh->submeshes) {
                    if (skinned)
                        bgfx::setUniform(m_uBoneMatrices, it.boneMatrices, (uint16_t)(it.boneCount * 4));
                    bgfx::setState(st); bgfx::setTransform(it.model.ptr());
                    bgfx::setVertexBuffer(0, it.mesh->vbh);
                    bgfx::setIndexBuffer(it.mesh->ibh, sub.indexOffset, sub.indexCount);
                    bgfx::submit(sv, shadowProg);
                }
            }
        }
    }

    void bind(const float params[4], const float factor[4],
              bgfx::TextureHandle base, bgfx::TextureHandle norm,
              uint64_t state, const RenderItem& it) {
        bgfx::setUniform(m_uParams, params);
        bgfx::setUniform(m_uColorFactor, factor);
        bgfx::setTexture(0, m_sBaseColor, base);
        bgfx::setTexture(1, m_sNormalMap, norm);
        bgfx::setTexture(2, m_sShadowMap, m_shadowMap);
        bgfx::setState(state);
        bgfx::setTransform(it.model.ptr());
        bgfx::setVertexBuffer(0, it.mesh->vbh);
    }

    // NOTE: culling moved to render/world/frustum.{h,cpp} and lighting's fixed
    // cap to rworld::kMaxLights. Replacing the fixed-cap forward path with
    // clustered forward is docs/renderer-architecture.md §2 — lights beyond the
    // cap are still dropped today.
    bgfx::ProgramHandle m_program              = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_skinnedProgram       = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_skinnedShadowProgram = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_lineProgram          = BGFX_INVALID_HANDLE;   // debug lines
    bgfx::VertexLayout  m_lineLayout;
    bgfx::UniformHandle m_uBoneMatrices        = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sBaseColor   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uParams      = BGFX_INVALID_HANDLE;
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
