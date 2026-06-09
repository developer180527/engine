#pragma once
// Default render pipeline: single-pass forward PBR. Owns its shader program +
// uniforms (a swappable pipeline brings its own shaders). The engine hands it a
// RenderView (camera + unculled draw list + frustum); it culls, sorts, submits.
#include "render/render_pipeline.h"
#include "render/mesh.h"
#include "render/material.h"
#include "render/texture.h"
#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdint>

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

    void onAttach(RenderContext&) override {
        m_program = bgfx::createProgram(
            bgfx::createShader(bgfx::makeRef(VS_TRIANGLE_DATA, VS_TRIANGLE_SIZE)),
            bgfx::createShader(bgfx::makeRef(FS_TRIANGLE_DATA, FS_TRIANGLE_SIZE)),
            true);
        m_sBaseColor   = bgfx::createUniform("s_baseColor",   bgfx::UniformType::Sampler);
        m_uParams      = bgfx::createUniform("u_params",      bgfx::UniformType::Vec4);
        m_uColorFactor = bgfx::createUniform("u_colorFactor", bgfx::UniformType::Vec4);
        m_uLightParams = bgfx::createUniform("u_lightParams", bgfx::UniformType::Vec4);
        m_uLights      = bgfx::createUniform("u_lights",      bgfx::UniformType::Vec4, MAX_LIGHTS * 4);
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
        m_shadowFB  = bgfx::createFrameBuffer(1, &m_shadowMap, false);
        m_sShadowMap    = bgfx::createUniform("s_shadowMap",    bgfx::UniformType::Sampler);
        m_uShadowMtx    = bgfx::createUniform("u_shadowMtx",    bgfx::UniformType::Mat4);
        m_uShadowParams = bgfx::createUniform("u_shadowParams", bgfx::UniformType::Vec4);
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
    }

    void render(const RenderView& v, RenderContext& ctx) override {
        renderShadow(v, ctx);

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

        // TODO (Jun 3, 04:00 PM):
        // Long-term renderer architecture:
        // ECS World -> Render Extraction -> RenderWorld -> Visibility -> Submission.
        // Keep extraction, culling, sorting and submission as separate stages.
        // ForwardPipeline should eventually consume pre-extracted render data only.
        // Pack lights into u_lights[4*N]; fall back to the legacy sun when none.
        // TODO (Jun 3, 04:00 PM):
        // Move light packing into a dedicated render extraction stage.
        // Build GPU-ready RenderLight data before entering the render pipeline.
        // The pipeline should consume extracted render data rather than ECS-derived scene data.
        // Consider replacing float-encoded light type with packed integer/light flags.
        float packed[MAX_LIGHTS * 4 * 4] = { 0.0f };
        int   count = 0;
        auto add = [&](LightType type, const Vec3& pos, const Vec3& dir,
                       const Vec3& col, float inten, float range,
                       float spotInnerCos, float spotOuterCos) {
            if (count >= MAX_LIGHTS) return;
            float* b = &packed[count * 4 * 4];
            b[0]=pos.x; b[1]=pos.y; b[2]=pos.z; b[3]=(float)(uint32_t)type;
            b[4]=col.x; b[5]=col.y; b[6]=col.z; b[7]=inten;
            b[8]=dir.x; b[9]=dir.y; b[10]=dir.z; b[11]=range;
            b[12]=spotInnerCos; b[13]=spotOuterCos;
            ++count;
        };
        if (v.lights.empty()) {
            add(LightType::Directional, bx::Vec3{0.0f,0.0f,0.0f},
                bx::normalize(bx::Vec3{0.6f, 0.8f, 0.4f}),
                bx::Vec3{1.0f, 0.98f, 0.92f}, 2.2f, 0.0f, 1.0f, 0.0f);
        } else {
            for (uint32_t i = 0; i < (uint32_t)v.lights.size() && count < MAX_LIGHTS; ++i) {
                const LightItem& l = v.lights[i];
                add(l.type, l.position, l.direction, l.color, l.intensity, l.range,
                    l.spotInnerCos, l.spotOuterCos);
            }
        }
        bgfx::setUniform(m_uLights, packed, (uint16_t)(count * 4));
        const float lp[4] = { v.ambient, (float)count, 0.0f, 0.0f };
        bgfx::setUniform(m_uLightParams, lp);
        bgfx::setUniform(m_uCamPos, v.camPos.ptr());
        bgfx::setUniform(m_uShadowMtx, m_shadowMtx);
        const float sp[4] = { m_hasShadowCaster ? 1.0f : 0.0f, SHADOW_BIAS,
                              1.0f / (float)SHADOW_SIZE, (float)m_shadowLightIndex };
        bgfx::setUniform(m_uShadowParams, sp);

        // cull against the engine-provided frustum
        // TODO (Jun 3, 04:00 PM):
        // Separate visibility/culling from pipeline submission.
        // Visibility should eventually operate on a RenderWorld produced by extraction.
        m_visible.clear();
        for (uint32_t i = 0; i < (uint32_t)v.items.size(); ++i) {
            const RenderItem& it = v.items[i];
            if (!it.mesh) continue;
            if (it.mesh->hasBounds() && culled(it, v.frustum)) continue;
            m_visible.push_back(i);
        }
        // TODO (Jun 3, 04:00 PM):
        // Introduce packed render sort keys to reduce per-frame comparison cost.
        std::sort(m_visible.begin(), m_visible.end(), [&](uint32_t a, uint32_t b){
            const RenderItem& A = v.items[a]; const RenderItem& B = v.items[b];
            if (A.meshKey != B.meshKey) return A.meshKey < B.meshKey;
            return A.matKey < B.matKey;
        });

        for (uint32_t idx : m_visible) {
            const RenderItem& it = v.items[idx];
            const Material* mat  = it.mat;
            const Texture*  nm   = (mat && mat->normalMapTexture.valid())
                                 ? ctx.textures.getTexture(mat->normalMapTexture) : nullptr;
            const float rough = mat ? mat->roughness : 0.7f;
            const float metal = mat ? mat->metallic  : 0.0f;
            float params[4] = { it.tex ? 1.0f : 0.0f, rough, metal, nm ? 1.0f : 0.0f };
            float factor[4] = { 1, 1, 1, 1 };
            if (mat) { factor[0]=mat->baseColorFactor[0]; factor[1]=mat->baseColorFactor[1];
                       factor[2]=mat->baseColorFactor[2]; factor[3]=mat->baseColorFactor[3]; }
            const uint64_t state = it.mesh->doubleSided
                ? (BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_WRITE_Z |
                   BGFX_STATE_DEPTH_TEST_LESS | BGFX_STATE_MSAA)
                : (BGFX_STATE_DEFAULT | BGFX_STATE_CULL_CCW);
            const bgfx::TextureHandle base = it.tex ? it.tex->handle : ctx.whiteTex;
            const bgfx::TextureHandle norm = nm    ? nm->handle      : ctx.flatNormalTex;

            // Select skinned or static program
            const bool skinned = it.boneMatrices != nullptr && it.boneCount > 0;
            const bgfx::ProgramHandle prog = skinned ? m_skinnedProgram : m_program;

            if (it.mesh->submeshes.empty()) {
                if (skinned)
                    bgfx::setUniform(m_uBoneMatrices, it.boneMatrices, (uint16_t)(it.boneCount * 4));
                bind(params, factor, base, norm, state, it);
                bgfx::setIndexBuffer(it.mesh->ibh);
                bgfx::submit(id, prog);
            } else {
                for (const auto& sub : it.mesh->submeshes) {
                    if (skinned)
                        bgfx::setUniform(m_uBoneMatrices, it.boneMatrices, (uint16_t)(it.boneCount * 4));
                    bind(params, factor, base, norm, state, it);
                    bgfx::setIndexBuffer(it.mesh->ibh, sub.indexOffset, sub.indexCount);
                    bgfx::submit(id, prog);
                }
            }
        }
    }

private:
    void renderShadow(const RenderView& v, RenderContext& ctx) {
        m_hasShadowCaster = false;
        const LightItem* sun = nullptr;
        for (uint32_t i = 0; i < (uint32_t)v.lights.size(); ++i)
            if (v.lights[i].type == LightType::Directional && v.lights[i].castShadows) { sun = &v.lights[i]; m_shadowLightIndex = (int)i; break; }
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

    static bool culled(const RenderItem& it, const float planes[6][4]) {
        const bx::Vec3 c = it.mesh->boundsCenter();
        const bx::Vec3 s = it.mesh->boundsSize();
        const float* m = it.model.m;
        const float wx = m[0]*c.x + m[4]*c.y + m[8]*c.z  + m[12];
        const float wy = m[1]*c.x + m[5]*c.y + m[9]*c.z  + m[13];
        const float wz = m[2]*c.x + m[6]*c.y + m[10]*c.z + m[14];
        auto rl = [&](int i){ return std::sqrt(m[i]*m[i] + m[i+1]*m[i+1] + m[i+2]*m[i+2]); };
        const float maxS = std::max({ rl(0), rl(4), rl(8) });
        const float r = bx::length(s) * 0.5f * maxS;
        for (int p = 0; p < 6; ++p)
            if (planes[p][0]*wx + planes[p][1]*wy + planes[p][2]*wz + planes[p][3] < -r)
                return true;
        return false;
    }

    // TODO (Jun 3, 04:00 PM):
    // Replace fixed MAX_LIGHTS forward path with Forward+ or clustered lighting.
    // Current renderer truncates visible lights beyond MAX_LIGHTS.
    static constexpr int MAX_LIGHTS = 16;
    bgfx::ProgramHandle m_program              = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_skinnedProgram       = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle m_skinnedShadowProgram = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uBoneMatrices        = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sBaseColor   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uParams      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uColorFactor = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLightParams = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uCamPos      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sNormalMap   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uLights      = BGFX_INVALID_HANDLE;
    std::vector<uint32_t> m_visible;
    static constexpr uint16_t SHADOW_SIZE         = 4096;
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
