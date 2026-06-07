#pragma once
#include "render/primitive_library.h"
#include "engine/asset_service.h"
#include <memory>
#include <string>
#include <GLFW/glfw3.h>
#include <bgfx/bgfx.h>
#include "engine/runtime_context.h"
#include "engine/renderer.h"
#include "core/transform.h"
#include "components/spinner.h"

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

    bool init(const EngineConfig& cfg = {});
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

    Renderer&       renderer()     { return m_renderer; }   // pipeline injection, etc.
    GLFWwindow*     window() const { return m_window; }
    RuntimeContext& ctx()          { return *m_ctx; }
    PrimitiveLibrary& primitives() { return m_primitives; }
    AssetService&   assetService() { return *m_assetService; }
    int             width()  const { return m_width; }
    int             height() const { return m_height; }
    float           fov()    const { return m_fov; }

private:
    // Platform
    GLFWwindow* m_window = nullptr;
    int   m_width  = 1280;
    int   m_height = 720;
    float m_fov    = 60.0f;

    // Content systems — stable addresses, declared before m_ctx and m_renderer.
    PrimitiveLibrary m_primitives;
    flecs::world     m_ecs;
    AssetRegistry    m_assets;
    TextureRegistry  m_textures;
    MaterialRegistry m_materials;
    ProjectContext   m_project;
    ImporterRegistry m_importers;

    std::unique_ptr<AssetService>   m_assetService;
    std::unique_ptr<RuntimeContext> m_ctx;

    // Render subsystem — owns the device + all render state. Declared after the
    // systems it borrows, so it is destroyed before them.
    Renderer m_renderer;

    // Gameplay-tick query (editor world); NOT a render concern — stays here.
    flecs::query<Transform, const Spinner> m_spinnerQuery;

    bool initPlatform(const EngineConfig& cfg);
    bool initRenderer(const EngineConfig& cfg);
    bool initSystems();
    void buildDefaultScene();
    void tickSystems(float dt, bool paused);
    void shutdownPlatform();
};
