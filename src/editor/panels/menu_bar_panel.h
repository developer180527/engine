#pragma once
#include <imgui.h>
#include <functional>
#include <string>

// Callbacks for menu items that require engine-level actions.
// Everything else is either demo (no-op) or self-contained.
struct MenuBarCallbacks {
    std::function<void()> saveScene;
    std::function<void()> reloadScene;
    std::function<void()> quit;
    std::function<void()> openProjectSettings;
    std::function<void()> onUndo;
    std::function<void()> onRedo;
    bool canUndo = false;
    bool canRedo = false;
    std::string undoDesc;
    std::string redoDesc;
    std::string           projectName;   // shown after the separator
    bool* showProfiler = nullptr;        // toggled by Profiler > Frame Profiler
    bool* showPlugins  = nullptr;        // Plugins > Plugin Manager (visibility)
    bool* focusPlugins = nullptr;        // set true to bring the panel to front
};

inline void drawMenuBar(const MenuBarCallbacks& cb) {
    if (!ImGui::BeginMainMenuBar()) return;

    // ── engine ───────────────────────────────────────────────────────
    if (ImGui::BeginMenu("engine")) {
        ImGui::TextDisabled("  v0.1.0-dev");
        ImGui::Separator();
        if (ImGui::MenuItem("Settings..."))         {} // TODO: settings panel
        if (ImGui::MenuItem("Preferences..."))      {} // TODO: editor prefs panel
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Cmd+Q"))       cb.quit();
        ImGui::EndMenu();
    }

    // ── File ─────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Scene",   "Cmd+S")) cb.saveScene();
        if (ImGui::MenuItem("Reload Scene"))          cb.reloadScene();
        ImGui::Separator();
        if (ImGui::MenuItem("New Scene"))             {} // TODO
        if (ImGui::MenuItem("Open Scene..."))         {} // TODO
        ImGui::Separator();
        if (ImGui::MenuItem("Build Project..."))      {} // TODO: launcher
        ImGui::EndMenu();
    }

    // ── Edit ─────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Edit")) {
        if (!cb.canUndo) ImGui::BeginDisabled();
        { std::string label = cb.undoDesc.empty() ? "Undo" : "Undo " + cb.undoDesc;
          if (ImGui::MenuItem(label.c_str(), "Cmd+Z") && cb.onUndo) cb.onUndo(); }
        if (!cb.canUndo) ImGui::EndDisabled();
        if (!cb.canRedo) ImGui::BeginDisabled();
        { std::string label = cb.redoDesc.empty() ? "Redo" : "Redo " + cb.redoDesc;
          if (ImGui::MenuItem(label.c_str(), "Cmd+Shift+Z") && cb.onRedo) cb.onRedo(); }
        if (!cb.canRedo) ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(); // TODO: clipboard ops
        ImGui::MenuItem("Cut",   "Cmd+X");
        ImGui::MenuItem("Copy",  "Cmd+C");
        ImGui::MenuItem("Paste", "Cmd+V");
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate",  "Cmd+D")) {} // TODO
        if (ImGui::MenuItem("Delete",     "Del"))    {} // TODO: wires to hierarchy
        ImGui::Separator();
        if (ImGui::MenuItem("Select All", "Cmd+A")) {} // TODO
        ImGui::EndMenu();
    }

    // ── View ─────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("View")) {
        if (ImGui::BeginMenu("Panels")) {
            // TODO: toggle panel visibility
            static bool sSceneView = true, sInspector = true;
            static bool sHierarchy = true, sAssets   = true;
            static bool sConsole   = true, sStats     = true;
            ImGui::MenuItem("Scene View",  nullptr, &sSceneView);
            ImGui::MenuItem("Hierarchy",   nullptr, &sHierarchy);
            ImGui::MenuItem("Inspector",   nullptr, &sInspector);
            ImGui::MenuItem("Assets",      nullptr, &sAssets);
            ImGui::MenuItem("Console",     nullptr, &sConsole);
            ImGui::MenuItem("Stats",       nullptr, &sStats);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Renderer")) {
            static bool sWireframe = false, sNormals = false, sUVs = false;
            ImGui::MenuItem("Wireframe",        nullptr, &sWireframe); // TODO
            ImGui::MenuItem("Show Normals",     nullptr, &sNormals);   // TODO
            ImGui::MenuItem("Show UVs",         nullptr, &sUVs);       // TODO
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) {} // TODO: reset dockspace
        ImGui::EndMenu();
    }

    // ── Tools ────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Tools")) {
        if (ImGui::MenuItem("Recook All Assets")) {} // TODO: trigger cook service
        if (ImGui::MenuItem("Shader Compiler...")) {} // TODO
        ImGui::Separator();
        if (ImGui::MenuItem("Asset Importer Settings...")) {} // TODO
        if (ImGui::MenuItem("Project Settings...", "Cmd+,"))
            if (cb.openProjectSettings) cb.openProjectSettings();
        ImGui::Separator();
        if (ImGui::MenuItem("Generate Project Files"))     {} // TODO: CMake
        ImGui::EndMenu();
    }

    // ── Profiler ─────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Profiler")) {
        if (cb.showProfiler)
            ImGui::MenuItem("Frame Profiler", "", cb.showProfiler); // real panel
        ImGui::Separator();
        if (ImGui::BeginMenu("CPU")) {
            if (ImGui::MenuItem("ECS Query Cost"))    {} // TODO
            if (ImGui::MenuItem("Physics Step"))      {} // TODO
            if (ImGui::MenuItem("Render Prepare"))    {} // TODO
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("GPU")) {
            if (ImGui::MenuItem("Draw Call Count"))   {} // TODO: bgfx stats
            if (ImGui::MenuItem("Texture Memory"))    {} // TODO
            if (ImGui::MenuItem("Buffer Memory"))     {} // TODO
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Memory Overview"))       {} // TODO
        ImGui::Separator();
        if (ImGui::MenuItem("Start Recording"))       {} // TODO
        if (ImGui::MenuItem("Stop & Export"))         {} // TODO
        ImGui::EndMenu();
    }

    // ── Plugins ──────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Plugins")) {
        // Always-on panel — clicking reveals + focuses it (it may be hiding as a
        // background dock tab), rather than toggling a checkbox that's always on.
        if (ImGui::MenuItem("Plugin Manager")) {
            if (cb.showPlugins)  *cb.showPlugins  = true;
            if (cb.focusPlugins) *cb.focusPlugins = true;
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Physics Backend")) {
            static int sPhysics = 0; // 0=None
            ImGui::RadioButton("None",  &sPhysics, 0);
            ImGui::RadioButton("Jolt",  &sPhysics, 1); // TODO
            ImGui::RadioButton("PhysX", &sPhysics, 2); // TODO
            ImGui::RadioButton("Box2D", &sPhysics, 3); // TODO
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Audio Backend")) {
            static int sAudio = 0;
            ImGui::RadioButton("None",      &sAudio, 0);
            ImGui::RadioButton("miniaudio", &sAudio, 1); // TODO
            ImGui::RadioButton("FMOD",      &sAudio, 2); // TODO
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scripting")) {
            static int sScript = 0;
            ImGui::RadioButton("None", &sScript, 0);
            ImGui::RadioButton("Lua",  &sScript, 1); // TODO
            ImGui::RadioButton("Wren", &sScript, 2); // TODO
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    // ── Window ───────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Window")) {
        if (ImGui::MenuItem("Maximise Scene View"))  {} // TODO
        ImGui::Separator();
        if (ImGui::MenuItem("New Viewport"))         {} // TODO: multi-viewport
        if (ImGui::MenuItem("Split View"))           {} // TODO
        ImGui::EndMenu();
    }

    // ── Help ─────────────────────────────────────────────────────────
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Documentation"))        {} // TODO: open browser
        if (ImGui::MenuItem("Keyboard Shortcuts"))   {} // TODO: shortcuts panel
        ImGui::Separator();
        if (ImGui::MenuItem("Report Bug"))           {} // TODO: open GitHub
        ImGui::Separator();
        if (ImGui::MenuItem("About"))                {} // TODO: about dialog
        ImGui::EndMenu();
    }

    // ── Project name ─────────────────────────────────────────────────
    if (!cb.projectName.empty()) {
        ImGui::SetNextItemWidth(0);
        // Right-align by pushing spacing
        float menuWidth = ImGui::GetCursorPosX();
        float barWidth  = ImGui::GetContentRegionAvail().x;
        (void)menuWidth; (void)barWidth;
        ImGui::TextDisabled(" |  %s", cb.projectName.c_str());
    }

    ImGui::EndMainMenuBar();
}
