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
#include "render/imgui_impl_glfw.h"
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
            gizmoBeginFrame();
            { auto ctx = buildCtx(); gizmoHandleHotkeys(m_rt.window(), ctx); }

            // ---- Editor camera ----
            updateEditorCamera(m_cam, m_input, m_rt.window(), dt);

            // ---- Camera matrices ----
            float view[16], proj[16];
            m_cam.getViewMatrix(view);
            bx::mtxProj(proj, m_rt.fov(),
                        float(fbw) / float(fbh),
                        0.1f, 1000.0f,
                        bgfx::getCaps()->homogeneousDepth);

            // ---- Runtime tick (ECS systems + scene render) ----
            m_rt.tick(dt, view, proj, ImGuizmo::IsUsing());

            // ---- Editor UI (all ImGui panel draws) ----
            renderUI(view, proj);

            // ---- Submit ImGui draw data then flip ----
            imguiRender();
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
                ImGuiDockNodeFlags_PassthruCentralNode);

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
        drawGizmo(ctx, view, proj);
    }
};
