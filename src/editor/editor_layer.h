#pragma once

#include "engine/engine.h"
#include "editor/hierarchy_panel.h"
#include "editor/inspector_panel.h"
#include "editor/asset_browser_panel.h"
#include "editor/gizmo.h"
#include "render/imgui_bgfx.h"
#include "render/imgui_impl_glfw.h"
#include <imgui.h>
#include <ImGuizmo.h>

// Forward-declare the stats panel helper (defined below)
inline void drawStatsPanel(EngineContext& ctx);

class EditorLayer : public IEditorLayer {
public:
    // engine_ is kept so the layer can call viewMatrix()/projMatrix().
    void init(EngineContext& ctx) override { (void)ctx; }

    void setEngine(Engine* e) { m_engine = e; }

    void handleHotkeys(GLFWwindow* window, EngineContext& ctx) override {
        gizmoHandleHotkeys(window, ctx);
    }

    void render(EngineContext& ctx) override {
        imguiNewFrame();
        gizmoBeginFrame();

        drawStatsPanel();
        bool show = true; ImGui::ShowDemoWindow(&show);
        drawHierarchyPanel(ctx);
        drawInspectorPanel(ctx);
        drawAssetBrowserPanel(ctx);

        if (m_engine)
            drawGizmo(ctx, m_engine->viewMatrix(), m_engine->projMatrix());

        imguiRender();
    }

    static void drawStatsPanel() {
        ImGui::Begin("Stats");
        ImGuiIO& io = ImGui::GetIO();
        ImGui::Text("FPS: %.1f",    io.Framerate);
        ImGui::Text("Frame: %.2f ms", 1000.0f / io.Framerate);
        ImGui::Text("Renderer: Metal");
        ImGui::End();
    }

    void shutdown() override {}

private:
    Engine* m_engine = nullptr;
};
