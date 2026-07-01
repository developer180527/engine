#pragma once
#include <memory>
#include <vector>
#include <bgfx/bgfx.h>
#include <flecs.h>

#include "render/render_pipeline.h"   // IRenderPipeline, RenderView, RenderItem, LightItem, RenderTarget, Mat4, Vec4
#include "render/render_context.h"    // RenderContext + asset/texture/material registries
#include "core/transform.h"
#include "components/mesh_renderer.h"
#include "components/skinned_mesh.h"
#include "components/light.h"
#include "animation/skeleton_registry.h"
#include "runtime/world_query_cache.h"

// Owns the GPU device lifecycle and ALL render-side state: framebuffers,
// fallback textures, reserved view ids, the swappable pipeline, and per-frame
// extraction (RenderView). EngineRuntime holds one and forwards its frame calls
// here. Borrows the ECS world + registries (owned by EngineRuntime).
class Renderer {
public:
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

    // Debug-line collector (owned by EngineRuntime) drawn into each world view.
    void setDebugDraw(const dbg::DebugDraw* dd) { m_debugDraw = dd; }

    bgfx::TextureHandle sceneColorTexture() const { return m_sceneColorTex; }
    bgfx::TextureHandle gameColorTex()      const { return m_gameColorTex; }
    int sceneW() const { return m_sceneW; }
    int sceneH() const { return m_sceneH; }

private:
    RenderView    buildView(flecs::world& world, const float view[16],
                            const float proj[16], const RenderTarget& target,
                            bgfx::ViewId baseViewId);
    RenderContext makeContext();

    // Borrowed (owned by EngineRuntime)
    flecs::world*      m_editorWorld = nullptr;
    AssetRegistry*     m_assets      = nullptr;
    TextureRegistry*   m_textures    = nullptr;
    MaterialRegistry*  m_materials   = nullptr;
    SkeletonRegistry*  m_skeletons   = nullptr;
    const dbg::DebugDraw* m_debugDraw = nullptr;

    std::unique_ptr<IRenderPipeline>                  m_pipeline;
    bool                                              m_initialized = false;
    flecs::query<const Transform, const MeshRenderer> m_itemQuery;   // editor world
    flecs::query<const Transform, const Light>        m_lightQuery;  // editor world
    WorldQueryCache<const Transform, const MeshRenderer> m_gameItemQuery;  // sim world
    WorldQueryCache<const Transform, const Light>        m_gameLightQuery; // sim world
    std::vector<RenderItem> m_items;
    std::vector<LightItem>  m_lights;

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
