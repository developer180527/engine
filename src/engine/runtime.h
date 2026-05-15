#pragma once
#include <memory>
#include <string>
#include <GLFW/glfw3.h>
#include <bgfx/bgfx.h>
#include "engine/runtime_context.h"

struct EngineConfig {
    std::string title  = "Engine";
    int         width  = 1280;
    int         height = 720;
    float       fov    = 60.0f;
};

class EngineRuntime {
public:
    EngineRuntime();
    ~EngineRuntime();
    EngineRuntime(const EngineRuntime&)            = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;

    bool init(const EngineConfig& cfg = {});
    void shutdown();

    // Called once per frame by EditorApp.
    // view/proj: camera matrices from the editor camera (never owned here).
    // pauseSystems: true while ImGuizmo is being dragged — editor tells us.
    void tick(float dt,
              const float view[16],
              const float proj[16],
              bool pauseSystems = false);

    // Framebuffer resize — called by EditorApp when GLFW reports a size change.
    void resize(int w, int h);

    GLFWwindow*     window() const { return m_window; }
    RuntimeContext& ctx()          { return *m_ctx; }
    int             width()  const { return m_width; }
    int             height() const { return m_height; }
    float           fov()    const { return m_fov; }

private:
    // Platform
    GLFWwindow* m_window = nullptr;
    int   m_width  = 1280;
    int   m_height = 720;
    float m_fov    = 60.0f;

    // Systems — stable addresses, declared before m_ctx
    flecs::world     m_ecs;
    AssetRegistry    m_assets;
    TextureRegistry  m_textures;
    MaterialRegistry m_materials;
    ProjectContext   m_project;
    ImporterRegistry m_importers;

    std::unique_ptr<RuntimeContext> m_ctx;

    // Rendering
    bgfx::ProgramHandle m_program      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_sBaseColor   = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uParams      = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle m_uColorFactor = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle m_whiteTex     = BGFX_INVALID_HANDLE;

    static constexpr bgfx::ViewId kSceneView = 0;

    bool initPlatform(const EngineConfig& cfg);
    bool initRenderer(const EngineConfig& cfg);
    bool initSystems();
    void buildDefaultScene();
    void tickSystems(float dt, bool paused);
    void renderScene(const float view[16], const float proj[16]);
    void shutdownRenderer();
    void shutdownPlatform();
};
