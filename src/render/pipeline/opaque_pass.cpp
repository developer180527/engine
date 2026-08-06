// ── ForwardPipeline: the colour pass ─────────────────────────────────────────
// View uniforms, the visible set, batch runs, per-item state, material binding
// (fixed-struct and data-driven), instanced runs, submeshes, debug lines.
//
// This was 274 lines inside an 812-line header, and it is where every change of
// the last month landed — instancing, the draw ceiling, the submit counters. It is
// its own unit now so that reading "how does a frame get submitted" does not mean
// reading program creation and shadow matrices first.
#include "render/forward_pipeline.h"

#include "core/profiler.h"

void ForwardPipeline::render(const RenderView& v, RenderContext& ctx) {
        // Lights are packed FIRST, because the shadow pass and the lighting
        // shader must agree on which slot is shadowed, and only packing knows
        // the packed slot numbering (rworld::PackedLights::shadowLightIndex).
        m_submitStats.reset();   // per view; the shadow pass counts into it too
        m_boundMat.reset();      // R7: no material carried over from another view
        const rworld::PackedLights lights = rworld::packLights(v.lights, v.ambient);
        { ENGINE_PROFILE_SCOPE("Render.shadow");
          renderShadow(v, ctx, lights); }

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
        { ENGINE_PROFILE_SCOPE("Render.cull");
            rworld::buildVisibleSet(v.world(), v.camera(), m_visible,
                                    &m_cullParallel);
        m_submitStats.itemsConsidered = m_visible.consideredCount;
        m_submitStats.itemsCulled     = m_visible.culledCount;

        } // Render.cull

        // ── Expand submesh ranges into their own draws ──────────────────────
        // The cull produces one draw per ITEM. A multi-material mesh needs one
        // draw per range, and each range must carry ITS OWN material in the key —
        // otherwise the sort groups by the item's material, submission binds
        // A,B,C per item, and neither instancing nor bind dedup can ever apply.
        //
        // Done here rather than in rworld because this is where Mesh* is legal:
        // world/ is deliberately GPU-free and never dereferences a Mesh, which is
        // what makes culling and sorting unit-testable. The cost is one pass plus
        // one extra radix sort over ~1.7x the entries on real content, and ZERO on
        // content without submeshes — that case skips both and uses the cull's
        // list as-is.
        const std::vector<rworld::VisibleDraw>* drawList = &m_visible.draws;
        { ENGINE_PROFILE_SCOPE("Render.expand");
          // SKINNED items are deliberately NOT expanded. Their bone palette is a
          // per-ITEM uniform (4.7 KB for 73 bones), uploaded once per item and
          // relied upon to persist across that item's submits — audit R4. Expanding
          // them scatters their ranges across the sorted list, so other items'
          // palettes land in between and each range needs its own re-upload: the
          // exact regression R4 fixed, re-introduced. Expansion buys them nothing
          // anyway, since skinned draws are excluded from instancing.
          auto expandable = [&](const RenderItem& it) {
              return !it.mesh->submeshes.empty()
                  && !(it.boneMatrices != nullptr && it.boneCount > 0);
          };
          bool anySub = false;
          for (const auto& d : m_visible.draws)
              if (expandable(v.items[d.index])) { anySub = true; break; }
          if (anySub) {
              m_drawList.clear();
              m_drawList.reserve(m_visible.draws.size() * 2);
              for (const auto& d : m_visible.draws) {
                  const RenderItem& it = v.items[d.index];
                  if (!expandable(it)) { m_drawList.push_back(d); continue; }
                  // Depth is already quantised in the key; only the material half
                  // is rebuilt, so ranges of one mesh sort next to each other and
                  // ranges sharing a material sort together across items.
                  const uint32_t depth = rworld::depthOf(d.key);
                  uint16_t si = 0;
                  for (const auto& sub : it.mesh->submeshes) {
                      const uint32_t matKey = sub.material.valid() ? sub.material.id
                                                                  : it.matKey;
                      rworld::VisibleDraw e;
                      e.key = rworld::withOpaqueDepthCode(
                                  rworld::opaqueKeyBase(matKey, it.meshKey), depth);
                      e.index   = d.index;
                      e.submesh = si++;
                      m_drawList.push_back(e);
                  }
              }
              rworld::sortDraws(m_drawList, m_drawScratch);
              drawList = &m_drawList;
          }
        }

        // Runs of consecutive draws sharing mesh, material AND submesh range —
        // counted over the EXPANDED list, because that is the list submission walks
        // and therefore what instancing can actually collapse. draws - batchRuns is
        // the saving, stated as a number rather than assumed (audit R5).
        for (std::size_t i = 0; i < drawList->size(); ) {
            i += rworld::batchRunLength(*drawList, i);
            ++m_submitStats.batchRuns;
        }

        ENGINE_PROFILE_SCOPE("Render.submit");

        // Walk RUNS, not individual draws. rworld::batchRunLength returns how
        // many consecutive sorted draws share mesh AND material — the sort key
        // was built so that adjacency means batchability (see world/sort_key.h),
        // and a run longer than 1 collapses into a single instanced submit.
        const bool caps = 0 != (bgfx::getCaps()->supported & BGFX_CAPS_INSTANCING);
        if (!caps && !m_warnedNoInstancing) {
            m_warnedNoInstancing = true;
            // Silent until now: without instancing every batch run falls through to
            // per-draw, which is the difference between 299 draws and 41 571 on a
            // real scene — and nothing said so.
            LOG_WARN("Renderer", "backend reports no BGFX_CAPS_INSTANCING — every "
                                 "batch run falls back to per-draw submits");
        }
        for (std::size_t di = 0; di < drawList->size(); ) {
            const rworld::VisibleDraw& d = (*drawList)[di];
            const std::size_t runLen = rworld::batchRunLength(*drawList, di);
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
                // Resolving the handle is cheap (a vector index) and the fixed path
                // below needs `mat` only when it actually re-uploads; the
                // data-driven path needs it every draw.
                const Material* mat = mh.valid() ? ctx.materials.getMaterial(mh) : nullptr;

                // ── Data-driven: a cooked .material ─────────────────────────
                // Upload what the cook produced. No name lookup, no defaulting,
                // no validation — all of that happened offline against the
                // shader's declared interface, and repeating it here would be a
                // second source of truth that can drift from the first.
                if (mat && mat->dataDriven && ctx.shaders) {
                    const bgfx::ProgramHandle mp = programFor(*mat, ctx);
                    if (bgfx::isValid(mp)) {
                        // NOT deduped, deliberately: a data-driven material's
                        // uniform blocks and sampler set are variable-length, so
                        // caching them means caching a vector per material and
                        // invalidating it correctly. The fixed path is what cooked
                        // geometry uses and where the measured win is; this stays a
                        // full bind per draw until something measures it as a cost.
                        ++m_submitStats.materialBinds;
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
                            LOG_INFO("Renderer",
                                     "data-driven bind: material %u on shader "
                                     "\"%s\" (%zu block(s), %zu texture(s))",
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
                // R7. The uniform uploads happen only when the material actually
                // changed since the last draw; the resolved texture handles are
                // cached alongside so an unchanged material costs no registry
                // lookups either. The per-draw state below is re-issued always.
                if (!m_boundMat.holds(mh.id)) {
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
                    bgfx::setUniform(m_uTexFlags, texFlags);
                    bgfx::setUniform(m_uParams, params);
                    bgfx::setUniform(m_uColorFactor, factor);
                    m_boundMat.id   = mh.id;
                    m_boundMat.base = tex ? tex->handle : ctx.whiteTex;
                    m_boundMat.norm = nm  ? nm->handle  : ctx.flatNormalTex;
                    ++m_submitStats.materialBinds;   // now counts REAL binds
                }
                bindDrawState(m_boundMat.base, m_boundMat.norm, state, it);
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
            // SUBMESHED MESHES CAN NOW INSTANCE. batchRunLength requires the whole
            // run to share the same submesh ordinal, so every instance in the run
            // draws the same index range with the same material — which is exactly
            // the condition instancing needs. Before submesh expansion this had to
            // exclude them, and that single clause is why 96 of the kit's 176 meshes
            // never instanced.
            const bool instanceable =
                   caps && runLen > 1 && !skinned
                && !(runMat && runMat->dataDriven)
                && bgfx::isValid(m_instancedProgram);

            // The index range this draw covers, and the material that owns it.
            const bool whole = d.submesh == rworld::VisibleDraw::kWholeMesh
                            || d.submesh >= it.mesh->submeshes.size();
            const SubmeshRange* sub = whole ? nullptr
                                            : &it.mesh->submeshes[d.submesh];
            const MaterialHandle drawMat =
                (sub && sub->material.valid()) ? sub->material : it.material;

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
                        const RenderItem& ri = v.items[(*drawList)[di + k].index];
                        std::memcpy(dst, ri.model.ptr(), stride);
                        dst += stride;
                    }
                    m_instancing = true;
                    drawProgram = m_instancedProgram;
                    bindMaterial(drawMat);               // shared by the whole run
                    bgfx::setInstanceDataBuffer(&idb);
                    if (sub) bgfx::setIndexBuffer(it.mesh->ibh, sub->indexOffset,
                                                  sub->indexCount);
                    else     bgfx::setIndexBuffer(it.mesh->ibh);
                    bgfx::submit(id, drawProgram);
                    m_instancing = false;
                    ++m_submitStats.draws;
                    if (sub) ++m_submitStats.submeshDraws;
                    ++m_submitStats.instancedDraws;
                    m_submitStats.instancedItems += avail;
                    di += avail;
                    continue;
                }
                // Instance buffer exhausted this frame — fall through to
                // per-draw rather than dropping the objects.
                if (!m_warnedInstanceBuf) {
                    m_warnedInstanceBuf = true;
                    LOG_WARN("Renderer",
                             "instance data buffer exhausted (%u of %u instances "
                             "available) — this run and later ones submit per-draw, "
                             "so the draw count for this frame is higher than the "
                             "batch runs suggest", avail, want);
                }
            }

            // One draw per list entry — EXCEPT an unexpanded multi-range item, which
            // today means a skinned one (see `expandable`). That still needs its
            // ranges walked here, which is safe because its palette was uploaded
            // once above and persists across these submits.
            if (!sub && !it.mesh->submeshes.empty()) {
                for (const auto& sm : it.mesh->submeshes) {
                    if (drawBudgetExhausted()) break;
                    drawProgram = defaultProg;   // a data-driven range must not leak
                    bindMaterial(sm.material.valid() ? sm.material : it.material);
                    bgfx::setIndexBuffer(it.mesh->ibh, sm.indexOffset, sm.indexCount);
                    bgfx::submit(id, drawProgram);
                    ++m_submitStats.draws;
                    ++m_submitStats.submeshDraws;
                    if (skinned) ++m_submitStats.skinnedDraws;
                }
                ++di;
                continue;
            }

            drawProgram = defaultProg;
            bindMaterial(drawMat);
            if (sub) bgfx::setIndexBuffer(it.mesh->ibh, sub->indexOffset,
                                          sub->indexCount);
            else     bgfx::setIndexBuffer(it.mesh->ibh);
            bgfx::submit(id, drawProgram);
            ++m_submitStats.draws;
            if (sub) ++m_submitStats.submeshDraws;
            if (skinned) ++m_submitStats.skinnedDraws;
            ++di;
        }

        submitDebugLines(id, ctx);   // debug lines last, drawn over the meshes
    }

// PER-DRAW state only. Everything here is discarded by submit(), so it must be
// re-issued for every draw — which is exactly why the material UNIFORMS were split
// out of it (R7): those persist, these do not.
void ForwardPipeline::bindDrawState(bgfx::TextureHandle base,
              bgfx::TextureHandle norm, uint64_t state, const RenderItem& it) {
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

void ForwardPipeline::submitDebugLines(bgfx::ViewId id, RenderContext& ctx) {
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
