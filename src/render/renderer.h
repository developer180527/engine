#pragma once
#include <filesystem>
#include <memory>

class ShaderLibrary;   // render/shader/shader_library.h
#include <vector>
#include <bgfx/bgfx.h>
#include <flecs.h>

#include "render/render_pipeline.h"   // IRenderPipeline, RenderView, RenderItem, LightItem, RenderTarget, Mat4, Vec4
#include "render/render_context.h"    // RenderContext + asset/texture/material registries
#include "core/transform.h"
#include "components/prev_transform.h"   // optional query term in extraction
#include "components/mesh_renderer.h"
#include "components/skinned_mesh.h"
#include "components/light.h"
#include "animation/skeleton_registry.h"
#include "render/world/cull_stream.h"   // CullStreamStore (extraction fills it)
#include "runtime/world_query_cache.h"

// Owns the GPU device lifecycle and ALL render-side state: framebuffers,
// fallback textures, reserved view ids, the swappable pipeline, and per-frame
// extraction (RenderView). EngineRuntime holds one and forwards its frame calls
// here. Borrows the ECS world + registries (owned by EngineRuntime).
//
// Four jobs, one translation unit each — start in the one that matches the
// question rather than reading the class top to bottom:
//   renderer.cpp          pipeline ownership: attach/detach, RenderContext
//   renderer/device.cpp   bgfx up and down (+ the Rendering-heap allocator)
//   renderer/targets.cpp  framebuffers and the three render* entry points
//   renderer/extract.cpp  ECS world → RenderView — the per-item hot path
class Renderer {
public:
    // Fixed-timestep render interpolation: extraction lerps
    // PrevTransform -> Transform by alpha (1 = current state, editor default).
    void setSimAlpha(float a) { m_simAlpha = a; }
    Renderer();
    ~Renderer();
    Renderer(const Renderer&)            = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool init(void* nwh, int width, int height,
              flecs::world& editorWorld,
              AssetRegistry& assets, TextureRegistry& textures, MaterialRegistry& materials,
              SkeletonRegistry& skeletons);
    void shutdown();

    void resize(int w, int h);          // bgfx::reset
    void createSceneFB(int w, int h);   // (re)create the offscreen scene framebuffer

    // Shadow-map edge length, from project.json's graphics.shadowResolution.
    // The default pipeline's shadow map is the single largest GPU allocation
    // in the engine (size² × 4 B), so this is the main graphics-quality knob:
    // 1024 = 4 MB, 2048 = 16 MB, 4096 = 64 MB.
    //
    // Safe to call before OR after init(): a change after the pipeline is
    // attached re-attaches it, because the shadow map is created at attach
    // time and never resized. That happens on project open, not per frame.
    void setShadowResolution(uint32_t px);

    // Where cooked shaders live (the project's .cache). Set from openProject,
    // which happens AFTER init() — so this re-attaches the pipeline, exactly as
    // setShadowResolution does, or the programs would already have been built
    // from the compiled-in fallback.
    void setShaderCacheRoot(const std::filesystem::path& cacheRoot);

    // Frame flip + device caps — the runtime orchestrates THROUGH these so
    // runtime*.cpp never touches bgfx directly (the Renderer owns the whole
    // GPU device lifecycle; audit A.1).
    void frame();                       // bgfx::frame()
    bool homogeneousDepth() const;      // bgfx::getCaps()->homogeneousDepth

    void renderScene(const float view[16], const float proj[16]);
    void renderGameView(const float view[16], const float proj[16],
                        const float clearColor[4], flecs::world* gameWorld);
    // Standalone-game path: render a world straight to the backbuffer (no
    // offscreen FB, no ImGui composite). world == nullptr renders the world
    // passed at init(). Uses the same view id as the editor's game FB path —
    // call one or the other per frame, not both.
    void renderToBackbuffer(const float view[16], const float proj[16],
                            const float clearColor[4], flecs::world* world = nullptr);

    // Drop cached queries against the play-mode world — the runtime calls
    // this when that world is destroyed (sim stop).
    void resetWorldCaches();

    // Bring-your-own-renderer: swap the pipeline (default is ForwardPipeline).
    // Safe before or after init(); attaches once the device is ready.
    void setPipeline(std::unique_ptr<IRenderPipeline> pipeline);

    // Read-only, for diagnostics: what the active pipeline last submitted.
    // Null before init() or if a pipeline was never attached.
    const IRenderPipeline* pipeline() const { return m_pipeline.get(); }

    // Debug-line collector (owned by EngineRuntime) drawn into each world view.
    void setDebugDraw(const dbg::DebugDraw* dd) { m_debugDraw = dd; }

    bgfx::TextureHandle sceneColorTexture() const { return m_sceneColorTex; }
    bgfx::TextureHandle gameColorTex()      const { return m_gameColorTex; }
    int sceneW() const { return m_sceneW; }
    int sceneH() const { return m_sceneH; }

private:
    float m_simAlpha = 1.0f;
    RenderView    buildView(flecs::world& world, const float view[16],
                            const float proj[16], const RenderTarget& target,
                            bgfx::ViewId baseViewId);
    RenderContext makeContext();
    // Destroys the scene AND game FBs with their attachments, leaving every
    // handle invalid. Shared by createSceneFB and shutdown, because a resize
    // that forgets the game FB instead of destroying it leaks one per drag
    // until the bgfx texture pool runs out.
    void destroyTargets();
    bool ensureGameFB();     // lazily create at scene FB size; false = skip view

    // Borrowed (owned by EngineRuntime)
    flecs::world*      m_editorWorld = nullptr;
    AssetRegistry*     m_assets      = nullptr;
    std::filesystem::path m_shaderCacheRoot;
    // Owns every program built from a .cshader; content-keyed and refcounted,
    // so two materials on one variant share a program. Destroyed in shutdown()
    // BEFORE bgfx goes down.
    std::unique_ptr<ShaderLibrary> m_shaderLib;
    TextureRegistry*   m_textures    = nullptr;
    MaterialRegistry*  m_materials   = nullptr;
    SkeletonRegistry*  m_skeletons   = nullptr;
    const dbg::DebugDraw* m_debugDraw = nullptr;

    std::unique_ptr<IRenderPipeline>                  m_pipeline;
    bool                                              m_initialized = false;
    uint32_t                                          m_shadowResolution = 2048;
    // ── Extraction queries: the shape here IS the optimisation ──────────────
    // Renderable items are matched by TWO queries partitioned on whether the
    // entity has a ChildOf parent, and PrevTransform is an OPTIONAL term (the
    // pointer) rather than a lookup. Both exist for one measured reason: over
    // 20 000 objects, extraction cost 15.2 ms and almost none of it was
    // arithmetic — it was per-entity component lookups asking questions the
    // query engine already knows the answer to per archetype.
    //
    //   ~2.2 ms  target(ChildOf) + is_alive() + has<Transform>(), to learn
    //            "no parent" for every entity in a flat scene
    //   ~1.5 ms  try_get<Transform>, for what the query just handed us
    //   ~6.3 ms  try_get<PrevTransform> plus the interpolation
    //   ~1.4 ms  try_get<SkinnedMesh>, also now an optional term
    //
    // So: the parentless set (the overwhelming majority of scene content) takes
    // a path that never asks about parents, and nothing re-looks-up a component
    // the iteration already has. Together the two queries cover exactly what the
    // single query did — a parented entity still gets its full ancestor walk.
    using ItemQuery = flecs::query<const Transform, const MeshRenderer,
                                   const PrevTransform*, const SkinnedMesh*>;
    ItemQuery m_itemQuery;      // editor, parentless
    ItemQuery m_childItemQuery; // editor, parented
    flecs::query<const Transform, const Light> m_lightQuery;     // editor world
    WorldQueryCache<const Transform, const MeshRenderer, const PrevTransform*,
                    const SkinnedMesh*> m_gameItemQuery;         // sim, parentless
    WorldQueryCache<const Transform, const MeshRenderer, const PrevTransform*,
                    const SkinnedMesh*> m_gameChildItemQuery;    // sim, parented
    WorldQueryCache<const Transform, const Light> m_gameLightQuery;  // sim world
    std::vector<RenderItem> m_items;
    std::vector<LightItem>  m_lights;

    // ── Parallel extraction scratch ─────────────────────────────────────────
    // One archetype's component arrays, sliced to a job-sized range. Extraction
    // collects these serially (a handful of tables — cheap) and then processes
    // them on the job pool, because a chunk is CONTIGUOUS component data with no
    // ECS handle in it: that is what makes the work safe off the main thread.
    //
    // `outBegin` is assigned in table order so the output array comes out in the
    // same order a serial pass would produce — the sort key does not depend on
    // it, but a stable order means the submit counters are comparable run to run.
    // `written` is per-chunk because an item whose mesh is missing produces
    // nothing, so a chunk can write fewer items than it read.
    struct ExtractChunk {
        const Transform*     tr   = nullptr;
        const MeshRenderer*  mr   = nullptr;
        const PrevTransform* prev = nullptr;   // null: absent in this archetype
        const SkinnedMesh*   skin = nullptr;   // null: absent in this archetype
        uint32_t count    = 0;
        uint32_t outBegin = 0;
        uint32_t written  = 0;
    };
    std::vector<ExtractChunk> m_chunks;   // reused for capacity across frames

    // The cull's SoA working set, filled alongside m_items and kept parallel to it.
    // Owned here rather than by the pipeline because extraction is what produces
    // it — and producing it here is the point: the bounding sphere is built once,
    // while the model matrix is hot, instead of once per view inside each cull.
    rworld::CullStreamStore m_cull;

    bgfx::ViewId        m_viewCursor    = 5; // first free view past reserved 0..4
    bgfx::TextureHandle m_flatNormalTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_whiteTex      = BGFX_INVALID_HANDLE;

    bgfx::FrameBufferHandle m_sceneFB       = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     m_sceneColorTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     m_sceneDepthTex = BGFX_INVALID_HANDLE;
    bgfx::FrameBufferHandle m_gameFB        = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     m_gameColorTex  = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     m_gameDepthTex  = BGFX_INVALID_HANDLE;
    int m_sceneW = 1280, m_sceneH = 720;
    int m_backW  = 1280, m_backH  = 720; // backbuffer (window) size

    static constexpr bgfx::ViewId kShadowView  = 0; // depth-from-light (renders first)
    static constexpr bgfx::ViewId kSceneView   = 1;
    static constexpr bgfx::ViewId kBgView      = 2; // clears backbuffer
    static constexpr bgfx::ViewId kResolveView = 3; // MSAA blit resolve
    static constexpr bgfx::ViewId kGameView    = 4; // game camera view
};
