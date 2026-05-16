#pragma once
#include "engine/runtime.h"
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
