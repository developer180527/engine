#pragma once
#include "render/primitive_library.h"
#include "runtime/asset_service.h"
#include "runtime/scene_service.h"
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"
#include <memory>
#include <string>
#include <bgfx/bgfx.h>
#include "runtime/platform.h"
#include "runtime/runtime_context.h"
#include "runtime/renderer.h"
#include "runtime/plugin_registry.h"
#include "core/transform.h"
#include "components/spinner.h"
#include "systems/animator_system.h"

struct EngineConfig {
    std::string title  = "Engine";
    int         width  = 1280;
    int         height = 720;
    float       fov    = 60.0f;
};

// Frame coordinator: owns platform, the ECS world + content systems, plugin
// lifecycle, gameplay tick, and the scene. Render-side state lives in m_renderer;
// the frame entry points forward to it so callers (EditorApp) are unchanged.
class EngineRuntime {
public:
    EngineRuntime();
    ~EngineRuntime();
    EngineRuntime(const EngineRuntime&)            = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;

    // Default platform (GlfwPlatform — stock OS window). Pass your own
    // IPlatform to embed the engine elsewhere or run headless: a platform
    // whose nativeWindowHandle() is null skips renderer init entirely.
    bool init(const EngineConfig& cfg = {});
    bool init(const EngineConfig& cfg, std::unique_ptr<IPlatform> platform);
    void shutdown();

    void tick(float dt, const float view[16], const float proj[16], bool pauseSystems = false);

    void resize(int w, int h);
    void createSceneFB(int w, int h) { m_renderer.createSceneFB(w, h); }

    bgfx::TextureHandle sceneColorTexture() const { return m_renderer.sceneColorTexture(); }
    bgfx::TextureHandle gameColorTex()      const { return m_renderer.gameColorTex(); }
    void renderGameView(const float view[16], const float proj[16],
                        const float clearColor[4], flecs::world* gameWorld = nullptr) {
        m_renderer.renderGameView(view, proj, clearColor, gameWorld);
    }
    int sceneW() const { return m_renderer.sceneW(); }
    int sceneH() const { return m_renderer.sceneH(); }

    Renderer&        renderer()     { return m_renderer; }   // pipeline injection, etc.
    IPlatform&       platform()     { return *m_platform; }
    bool             headless() const { return m_headless; }
    RuntimeContext&  ctx()          { return *m_ctx; }

    // Plugins — register via plugins().add(...) after init(), then call
    // attachPlugins() once. detachAll happens automatically in shutdown().
    PluginRegistry&  plugins()      { return m_plugins; }
    void attachPlugins()            { m_plugins.attachAll(*m_ctx); }
    SkeletonRegistry& skeletons()  { return m_skeletons; }
    AnimClipRegistry& clips()      { return m_clips; }
    PrimitiveLibrary& primitives() { return m_primitives; }
    AssetService&   assetService() { return *m_assetService; }
    SceneService&   sceneService() { return *m_sceneService; }
    int             width()  const { return m_width; }
    int             height() const { return m_height; }
    float           fov()    const { return m_fov; }

private:
    // Platform — abstract; default is GlfwPlatform, swappable at init()
    std::unique_ptr<IPlatform> m_platform;
    bool  m_headless = false;
    int   m_width  = 1280;
    int   m_height = 720;
    float m_fov    = 60.0f;

    // Content systems — stable addresses, declared before m_ctx and m_renderer.
    PrimitiveLibrary m_primitives;
    flecs::world     m_ecs;
    AssetRegistry    m_assets;
    TextureRegistry  m_textures;
    MaterialRegistry m_materials;
    SkeletonRegistry m_skeletons;
    AnimClipRegistry m_clips;
    ProjectContext   m_project;
    ImporterRegistry m_importers;

    std::unique_ptr<AssetService>   m_assetService;
    std::unique_ptr<SceneService>   m_sceneService;
    std::unique_ptr<RuntimeContext> m_ctx;

    // Render subsystem — owns the device + all render state. Declared after the
    // systems it borrows, so it is destroyed before them.
    Renderer m_renderer;

    // Gameplay-tick query (editor world); NOT a render concern — stays here.
    flecs::query<Transform, const Spinner> m_spinnerQuery;
    AnimatorSystem m_animatorSystem;
    PluginRegistry m_plugins;

    bool initRenderer(const EngineConfig& cfg);
    bool initSystems();
    void buildDefaultScene();
    void tickSystems(float dt, bool paused);
};
