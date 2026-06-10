#pragma once
#include "render/primitive_library.h"
#include "runtime/asset_service.h"
#include "runtime/scene_service.h"
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"
#include <chrono>
#include <filesystem>
#include <functional>
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
    // Empty title => use the project name from project.json.
    std::string title;
    int         width  = 1280;
    int         height = 720;
    float       fov    = 60.0f;

    // Empty => auto-detect (walk up from cwd looking for project.json).
    std::filesystem::path projectRoot;

    // Open <projectRoot>/.cache/registry.db and scan the assets folder at
    // startup. Disable for tools that bring their own database connection.
    bool openAssetDatabase = true;
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

    // ── Frame loop ──────────────────────────────────────────────────────────
    // The runtime owns the loop skeleton; the app supplies the per-frame body.
    //
    //   engine.run([&](float dt) {
    //       engine.tick(dt, view, proj);   // + whatever the app does
    //   });
    //
    // Apps that need finer control (the editor wraps the body with ImGui
    // begin/end) call the building blocks directly:
    //
    //   float dt;
    //   while (engine.frameBegin(dt)) { ...body...; engine.frameEnd(); }
    //
    // frameBegin: polls platform events, computes clamped dt, handles window
    // resize, drains async asset uploads. Returns false when the platform
    // requests close. frameEnd: flips the frame (bgfx::frame).
    bool frameBegin(float& dt);
    void frameEnd();
    void run(const std::function<void(float dt)>& frame);

    // ── Simulation lifecycle ────────────────────────────────────────────────
    // Owns plugin sim broadcasts + the optional snapshot game world. A game
    // boots straight into simulation; the editor's Play button does the same
    // with Snapshot mode so editing state survives Stop.
    //
    //   InPlace  — simulate the runtime's own world (game usage: boot = play)
    //   Snapshot — serialize the world, simulate a fresh copy, restore on stop
    enum class SimMode { InPlace, Snapshot };

    bool startSimulation(SimMode mode = SimMode::InPlace);
    void stopSimulation();
    // Plugin phases (update -> physics -> post-physics) on the sim world.
    // No-op unless simulating — the host decides pause by not calling it.
    void tickSimulation(float dt);

    bool          simulating() const { return m_simulating; }
    // The world being simulated: the snapshot game world if one exists,
    // otherwise the runtime's own world. Always safe to call.
    flecs::world& simWorld() { return m_gameWorld ? *m_gameWorld : m_ecs; }

    // ── Per-frame work ──────────────────────────────────────────────────────
    // Game-facing tick: gameplay systems + simulation + primary-camera render
    // to the backbuffer. Returns false if no primary Camera entity was found
    // (systems still ticked; nothing rendered).
    bool tick(float dt);
    // Editor/custom-camera tick: systems + render with explicit matrices into
    // the offscreen scene framebuffer. Does NOT tick simulation.
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
    ProjectContext&  project()      { return m_project; }
    assetlib::AssetRegistry& assetLib() { return m_assetLib; }

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

    // Frame-loop timing
    std::chrono::steady_clock::time_point m_lastFrameTime;
    bool m_firstFrame = true;

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
    // Asset database (SQLite, WAL) — read connection for the main thread.
    // CookService and other writers open their own connections.
    assetlib::AssetRegistry m_assetLib;

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

    // Simulation state — game world only exists in Snapshot mode.
    bool                          m_simulating = false;
    std::unique_ptr<flecs::world> m_gameWorld;
    std::string                   m_simSnapshot;

    bool initRenderer(const EngineConfig& cfg);
    bool initSystems(const EngineConfig& cfg);
    void buildDefaultScene();
    void tickSystems(float dt, bool paused);
};
