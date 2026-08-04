// ── ForwardPipeline: device resources ────────────────────────────────────────
// Programs, uniforms, the shadow map and its framebuffer — created in onAttach,
// released in onDetach. The ONLY translation unit that includes shader_blobs.h,
// because the generated shader arrays are `static` and every extra includer would
// duplicate all of them (see that header).
#include "render/forward_pipeline.h"

#include "runtime/jobs/jobs.h"
#include "render/pipeline/shader_blobs.h"

void ForwardPipeline::onAttach(RenderContext& attachCtx) {
    // The cull's parallel dispatcher, built ONCE. jobs::parallelFor blocks, so
    // the ranges are guaranteed finished before buildVisibleSet returns and no
    // job outlives the frame. If the pool was never started (headless tools,
    // tests) this still installs; buildVisibleSet checks jobs::initialized()
    // through it and runs serially.
    m_cullParallel = [](uint32_t count, uint32_t grain,
                        const std::function<void(uint32_t, uint32_t)>& fn) {
        if (!jobs::initialized()) { fn(0, count); return; }
        jobs::parallelFor("Cull.range", count, grain, fn);
    };

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

void ForwardPipeline::onDetach() {
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
