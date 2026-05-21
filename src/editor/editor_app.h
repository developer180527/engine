#include <filesystem>
#pragma once
#include "engine/runtime.h"
#include "io/scene_serializer.h"
#include "editor/editor_prefs.h"
#include "io/project_context.h"

#include "engine/async_loader.h"
#include "engine_context.h"
#include "editor/editor_camera.h"
#include "editor/hierarchy_panel.h"
#include "editor/inspector_panel.h"
#include "editor/asset_browser_panel.h"
#include "editor/console_panel.h"
#include "editor/gizmo.h"
#include "render/imgui_bgfx.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <chrono>
#include <algorithm>

class EditorApp {
public:
    explicit EditorApp(EngineRuntime& rt) : m_rt(rt) {}
    ~EditorApp() = default;

    EditorApp(const EditorApp&)            = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    void setProject(const ProjectContext& ctx) {
        m_projectRoot = ctx.projectRoot;
        m_scenePath   = ctx.projectRoot / ctx.lastScene;
        getTerminal().setProjectRoot(ctx.projectRoot.string());

        // Restore editor camera first (independent of scene)
        std::string selName;
        EditorPrefs::load(m_projectRoot, m_cam, selName);

        // Load scene ASYNC — JSON parsed immediately, assets stream in.
        // Engine is interactive while meshes/textures load in background.
        AssetStorage storage{m_rt.ctx().assets,
                             m_rt.ctx().textures,
                             m_rt.ctx().materials};
        SceneSerializer::loadAsync(m_scenePath,
                                   m_rt.ctx().ecs,
                                   storage,
                                   m_loader,
                                   m_rt.ctx().importers);

        // Selection restore: entities with Name exist immediately after
        // loadAsync (transform + name are set synchronously).
        if (!selName.empty()) {
            flecs::entity e = m_rt.ctx().ecs.lookup(selName.c_str());
            if (e.id() != 0 && e.is_alive())
                m_editor.selected = e;
        }
    }

    void saveScene() {
        if (m_scenePath.empty()) return;
        SceneSerializer::save(m_scenePath, m_rt.ctx().ecs, m_rt.ctx().assets);

        // Persist editor state
        std::string selName;
        if (m_editor.selected.is_alive())
            if (const Name* n = m_editor.selected.try_get<Name>())
                selName = n->value;
        EditorPrefs::save(m_projectRoot, m_cam, selName);
        LOG_SUCCESS("Project", "Scene saved");
    }

    void init() {
        imguiInit(m_rt.window(), 16.0f);
    }

    void run() {
        using clock = std::chrono::steady_clock;
        auto   prev  = clock::now();
        int    lastW = m_rt.width(), lastH = m_rt.height();

        while (!glfwWindowShouldClose(m_rt.window())) {
            // ---- Timing ----
            auto  now = clock::now();
            float dt  = std::min(
                std::chrono::duration<float>(now - prev).count(), 0.05f);
            prev = now;

            // ---- Events ----
            glfwPollEvents();

            // ---- Resize (runtime handles bgfx::reset) ----
            int fbw, fbh;
            glfwGetFramebufferSize(m_rt.window(), &fbw, &fbh);
            if (fbw <= 0 || fbh <= 0) {
                glfwWaitEventsTimeout(0.1);
                continue;
            }
            if (fbw != lastW || fbh != lastH) {
                m_rt.resize(fbw, fbh);
                lastW = fbw; lastH = fbh;
            }

            // ---- Drain async asset uploads (main thread GPU upload)
            {
                AssetStorage storage{m_rt.ctx().assets,
                                     m_rt.ctx().textures,
                                     m_rt.ctx().materials};
                m_loader.drainOne(storage); // one per frame — keeps frame time smooth
            }

            // ---- ImGui frame start (must come before any ImGui calls) ----
            imguiNewFrame();
            // BeginFrame MUST be outside any Begin/End block —
            // it creates an internal transparent overlay window.
            gizmoBeginFrame();
            { auto ctx = buildCtx(); gizmoHandleHotkeys(m_rt.window(), ctx); }

            // ---- Editor camera ----
            // Use the GLFW window that currently hosts the Scene View panel.
            // When docked: main window. When detached: the OS child window.
            GLFWwindow* camWin = m_sceneGLFWWindow ? m_sceneGLFWWindow : m_rt.window();
            updateEditorCamera(m_cam, m_input, camWin, dt, m_sceneViewHovered);

            // ---- Camera matrices ----
            float view[16], proj[16];
            m_cam.getViewMatrix(view);
            bx::mtxProj(proj, m_rt.fov(),
                        float(m_rt.sceneW()) / float(m_rt.sceneH()),
                        0.1f, 1000.0f,
                        bgfx::getCaps()->homogeneousDepth);

            // ---- Runtime tick (ECS systems + scene render) ----
            // Resize scene FB before rendering — avoids race condition
            // where tick() renders to old FB while panel shows new empty FB
            // Always render at >= full window resolution.
            // If the panel is smaller, ImGui downscales the texture —
            // downscaling multiple rendered pixels into one display pixel
            // gives free SSAA and eliminates the aliasing regression.
            const int renderW = std::max(m_desiredSceneW, m_rt.width());
            const int renderH = std::max(m_desiredSceneH, m_rt.height());
            // Hysteresis: only recreate FB when size differs by > 8px.
            // Prevents GPU texture thrash during window resize animations.
            const int dw = std::abs(renderW - m_rt.sceneW());
            const int dh = std::abs(renderH - m_rt.sceneH());
            if (dw > 8 || dh > 8)
                m_rt.createSceneFB(renderW, renderH);

            m_rt.tick(dt, view, proj, ImGuizmo::IsUsing());

            // ---- Editor UI (all ImGui panel draws) ----
            renderUI(view, proj);

            // ---- Submit ImGui draw data then flip ----
            imguiRender();
            imguiRenderViewports();
            bgfx::frame();
        }
    }

    void shutdown() {
        imguiShutdown();
    }

private:
    EngineRuntime&  m_rt;
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_scenePath;
    AsyncLoader      m_loader;
    EditorCamera   m_cam;
    EditorInput    m_input;
    EditorState    m_editor;
    GizmoState     m_gizmo;
    bool           m_sceneViewHovered = true;
    float          m_sceneAspect      = 16.0f / 9.0f;
    GLFWwindow*    m_sceneGLFWWindow  = nullptr; // GLFW window currently hosting Scene View
    int            m_desiredSceneW    = 1280;
    int            m_desiredSceneH    = 720;

    // Build a fresh EngineContext for this frame's panels.
    // Stack-allocated — valid only for the duration of renderUI().
    EngineContext buildCtx() {
        auto& rc = m_rt.ctx();
        return EngineContext{
            rc.ecs, rc.assets, rc.textures,
            rc.materials, rc.project, rc.importers,
            m_editor, m_gizmo
        };
    }

    void drawSceneViewPanel(const float view[16], const float proj[16],
                            EngineContext& ctx) {
        // Set position/size on first run (no ini file = always first run)
        ImGuiViewport* _vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(
            ImVec2(_vp->WorkPos.x + 250, _vp->WorkPos.y + 30),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
            ImVec2(_vp->WorkSize.x - 500, _vp->WorkSize.y - 60),
            ImGuiCond_FirstUseEver);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("Scene View");
        ImGui::PopStyleVar();

        // Track which GLFW window hosts this panel for camera input.
        if (ImGuiViewport* vp = ImGui::GetWindowViewport())
            m_sceneGLFWWindow = (GLFWwindow*)vp->PlatformHandle;

        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 16.0f) avail.x = 16.0f;
        if (avail.y < 16.0f) avail.y = 16.0f;

        // Free aspect — FB and projection both adapt to panel dimensions.
        // Projection matrix uses sceneW/sceneH so circles are always circles.
        // A portrait panel shows less horizontal content (narrower hFOV),
        // a wide panel shows more. No distortion, just different FOV coverage.
        m_desiredSceneW = std::max((int)avail.x, 64);
        m_desiredSceneH = std::max((int)avail.y, 64);

        ImTextureID tid = (ImTextureID)(uintptr_t)m_rt.sceneColorTexture().idx;
        ImVec2 cursorPos = ImGui::GetCursorScreenPos();
        ImGui::Image(tid, avail);
        const ImVec2 dispSize = avail;

        // Gizmo overlays the scene image — SetRect here overrides the one
        // set in gizmoBeginFrame(), giving it the correct panel coordinates
        ImGuizmo::SetDrawlist(); // use Scene View draw list, not gizmo overlay
        ImGuizmo::SetRect(cursorPos.x, cursorPos.y, dispSize.x, dispSize.y);
        drawGizmo(ctx, view, proj);
        m_sceneViewHovered = ImGui::IsWindowHovered() || m_input.rightMouseHeld;

        // Camera is active when this panel is hovered OR while right-dragging
        // (dragging keeps hover true so camera doesn't cut out mid-drag)

        ImGui::End();
    }

    void renderUI(const float view[16], const float proj[16]) {
        // ── Fullscreen DockSpace host ────────────────────────────
        // Covers the entire window. PassthruCentralNode lets the bgfx
        // 3D scene show through the undocked center area.
        {
            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::SetNextWindowViewport(vp->ID);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0,0));

            constexpr ImGuiWindowFlags kHostFlags =
                ImGuiWindowFlags_NoDocking         |
                ImGuiWindowFlags_NoTitleBar         |
                ImGuiWindowFlags_NoCollapse         |
                ImGuiWindowFlags_NoResize           |
                ImGuiWindowFlags_NoMove             |
                ImGuiWindowFlags_NoBringToFrontOnFocus |
                ImGuiWindowFlags_NoNavFocus         |
                ImGuiWindowFlags_NoBackground;

            ImGui::Begin("##DockHost", nullptr, kHostFlags);
            ImGui::PopStyleVar(3);

            ImGuiID dsid = ImGui::GetID("MainDockSpace");
            ImGui::DockSpace(dsid, ImVec2(0,0),
                ImGuiDockNodeFlags_None);

            ImGui::End();
        }

        // ── File menu ────────────────────────────────────────────
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Save Scene", "Cmd+S"))
                    saveScene();
                ImGui::Separator();
                if (ImGui::MenuItem("Reload Scene"))
                    setProject(m_rt.ctx().project);
                ImGui::EndMenu();
            }
            if (!m_projectRoot.empty()) {
                ImGui::TextDisabled(" |  %s",
                    m_projectRoot.filename().string().c_str());
            }
            ImGui::EndMainMenuBar();
        }

        // Cmd+S shortcut
        if (ImGui::IsKeyDown(ImGuiKey_LeftSuper) &&
            ImGui::IsKeyPressed(ImGuiKey_S, false))
            saveScene();

        auto ctx = buildCtx();

        drawSceneViewPanel(view, proj, ctx);

        // Stats
        ImGui::Begin("Stats");
        ImGui::Text("FPS: %.1f",      ImGui::GetIO().Framerate);
        ImGui::Text("Frame: %.2f ms", 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));
        ImGui::Text("Renderer: Metal");
        ImGui::Text("Camera: (%.2f, %.2f, %.2f)",
                    m_cam.position.x, m_cam.position.y, m_cam.position.z);
        ImGui::Text("Yaw: %.2f  Pitch: %.2f", m_cam.yaw, m_cam.pitch);
        ImGui::Separator();
        ImGui::TextDisabled("Hold RMB + WASD/QE to fly. Shift = faster.");
        ImGui::End();

        drawHierarchyPanel(ctx);
        drawInspectorPanel(ctx);
        drawAssetBrowserPanel(ctx, m_loader);
        drawConsolePanel();
    }
};
