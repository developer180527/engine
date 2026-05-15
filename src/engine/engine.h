#pragma once

#include <memory>
#include <string>

#include <GLFW/glfw3.h>
#include <bgfx/bgfx.h>
#include <bx/math.h>

#include "engine_context.h"

// Configuration passed to Engine::init().
struct EngineConfig {
    std::string title  = "Engine";
    int         width  = 1280;
    int         height = 720;
    float       fov    = 60.0f;
};

// Camera state — lives in Engine, not on the main() stack.
struct CameraState {
    bx::Vec3 position {0.0f, 6.0f, 18.0f};
    float    yaw      = 0.0f;
    float    pitch    = 0.0f;
};

// Engine: owns every runtime system and runs the main loop.
//
// Usage:
//   Engine engine;
//   if (!engine.init(config)) return 1;
//   engine.run();   // blocks until window closes
//   engine.shutdown();
//
// The editor is an optional layer injected via setEditorLayer().
// Without it the engine still renders the scene — this is the game
// runtime path used for shipping builds.
class IEditorLayer {
public:
    virtual ~IEditorLayer() = default;
    virtual void init(EngineContext& ctx)           = 0;
    virtual void render(EngineContext& ctx)         = 0;
    virtual void handleHotkeys(GLFWwindow* window,
                               EngineContext& ctx)  = 0;
    virtual void shutdown()                        = 0;
};

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    bool init(const EngineConfig& config = {});
    void run();
    void shutdown();

    // Inject the editor layer before calling run().
    // Without this, only the scene renders (game build mode).
    void setEditorLayer(std::unique_ptr<IEditorLayer> layer);

    EngineContext& ctx() { return *m_ctx; }

    // Camera matrices for the current frame — valid after renderScene().
    const float* viewMatrix() const { return m_view; }
    const float* projMatrix() const { return m_proj; }

private:
    // ---- Platform ----
    GLFWwindow* m_window = nullptr;
    int         m_width  = 1280;
    int         m_height = 720;
    float       m_fov    = 60.0f;

    // ---- Runtime systems (stable addresses — must be declared before m_ctx) ----
    flecs::world     m_ecs;
    AssetRegistry    m_assets;
    TextureRegistry  m_textures;
    MaterialRegistry m_materials;
    ProjectContext   m_project;
    ImporterRegistry m_importers;

    // ---- Context (references into the members above) ----
    std::unique_ptr<EngineContext> m_ctx;

    // ---- Rendering ----
    bgfx::ProgramHandle m_program      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sBaseColor   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uParams      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uColorFactor = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_whiteTex     = BGFX_INVALID_HANDLE;

    // ---- Camera + cached matrices ----
    float m_view[16] = {};
    float m_proj[16] = {};
    CameraState m_camera;
    bool        m_rightMouseHeld = false;
    double      m_lastMouseX     = 0.0;
    double      m_lastMouseY     = 0.0;

    // ---- Editor layer (optional) ----
    std::unique_ptr<IEditorLayer> m_editorLayer;

    // ---- Private helpers ----
    bool initPlatform(const EngineConfig& cfg);
    bool initRenderer(const EngineConfig& cfg);
    bool initSystems();
    void buildDefaultScene();
    void processInput(float dt);
    void tickSystems(float dt);
    void renderScene();
    void shutdownRenderer();
    void shutdownPlatform();
};
