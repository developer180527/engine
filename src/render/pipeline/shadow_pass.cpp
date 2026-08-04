// ── ForwardPipeline: the shadow pass ────────────────────────────────────────
// Light matrices, the light-space visible set (culled AND sorted — the sort is
// what lets casters batch), instanced runs, and the per-item fallback for skinned
// casters and submeshes.
#include "render/forward_pipeline.h"
#include "render/world/frustum.h"   // extractFrustumPlanes for the LIGHT frustum

void ForwardPipeline::renderShadow(const RenderView& v, RenderContext& ctx,
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
        rworld::buildVisibleSet(v.world(), lightCam, m_shadowVisible,
                                &m_cullParallel);
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
