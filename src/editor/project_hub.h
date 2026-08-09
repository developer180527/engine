#pragma once
#include "editor/window_chrome.h"   // shared title-bar band
#include <imgui.h>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "runtime/runtime.h"
#include "project/known_projects.h"
#include "project/project_scaffold.h"
#include "editor/editor_icons.h"

// ── Project hub ─────────────────────────────────────────────────────────────
// The screen shown BEFORE the editor when no project was passed on the
// command line (think Unity Hub, but a boot phase of the same executable —
// no second app, no IPC, instant transition into the editor).
//
//   ProjectHub hub;
//   auto path = hub.run(runtime);   // blocks until pick / create / quit
//   if (path.empty()) return 0;     // user closed the window
//   runtime.openProject(path);
//
// Lists known projects (~/.engine/projects.json via engine_core), creates
// new ones through project::create (same code path as the engine_project
// CLI), and lets the user open any folder by path.
class ProjectHub {
public:
    // Runs its own frame loop on the runtime. Requires ImGui to be
    // initialized already (imguiInit + theme). Returns the chosen project
    // root, or empty if the user closed the window.
    std::filesystem::path run(EngineRuntime& rt) {
        m_projects = project::KnownProjects::load();
        std::strncpy(m_location, defaultProjectsDir().c_str(),
                     sizeof(m_location) - 1);

        std::filesystem::path picked;
        float dt = 0.0f;
        while (picked.empty() && !m_quit && rt.frameBegin(dt)) {
            imguiNewFrame();
            picked = drawHub(rt.platform().nativeWindowHandle());
            imguiRender();
            imguiRenderViewports();
            rt.frameEnd();
        }
        if (!picked.empty()) {
            auto ctx = ProjectContext::load(picked);
            project::KnownProjects::touch(ctx.name, picked);
        }
        return picked;
    }

private:
    std::vector<project::KnownProject> m_projects;
    int   m_selected     = -1;
    int   m_templateIdx  = 0;
    bool  m_creating     = false;
    bool  m_quit         = false;
    char  m_name[128]    = "";
    char  m_location[512] = "";
    char  m_openPath[512] = "";
    std::string m_error;

    static std::string defaultProjectsDir() {
        const char* home = getenv("HOME");
        return (std::filesystem::path(home ? home : ".") / "Projects").string();
    }

    // Returns the picked project root, or empty while still browsing.
    std::filesystem::path drawHub(void* nativeWindow) {
        std::filesystem::path picked;

        // Fullscreen page — covers the whole window, no chrome behind it.
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##ProjectHub", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ── Title band ───────────────────────────────────────────────────
        // The hub is a different PAGE of the same window, so it needs the same
        // band as the editor's menu bar: the window has no title bar here
        // either. Without this the header sat jammed under the traffic lights
        // and there was nowhere left to grab the window — the hub could not be
        // moved at all.
        const float inset = platwin::titleBarInset(nativeWindow);
        const float band  = platwin::titleBarHeight(nativeWindow);
        const float rowH  = band > 0.0f ? band : ImGui::GetFrameHeight() + 8.0f;
        const float winBtns = edchrome::windowButtonsWidth();
        const float newBtnW = 130.0f;
        const float pad     = ImGui::GetStyle().WindowPadding.x;

        const float y0 = ImGui::GetCursorPosY();
        const char* title = ICON_FA_CUBES "  Projects";
        const float titleW = ImGui::CalcTextSize(title).x;

        // Label, inset past the window buttons and vertically centred in the
        // band rather than pinned to its top edge.
        ImGui::SetCursorPos({ inset + pad,
                              y0 + (rowH - ImGui::GetTextLineHeight()) * 0.5f });
        ImGui::TextUnformatted(title);

        // Drag strip: everything between the label and the New Project button.
        const float stripX = inset + pad + titleW + 8.0f;
        const float stripW = ImGui::GetWindowWidth() - stripX - newBtnW
                           - winBtns - pad * 2.0f;
        ImGui::SetCursorPos({ stripX, y0 });
        edchrome::dragStrip("##hub_titlebar_drag", stripW, rowH, nativeWindow);

        ImGui::SetCursorPos({ ImGui::GetWindowWidth() - newBtnW - winBtns - pad,
                              y0 + (rowH - ImGui::GetFrameHeight()) * 0.5f });
        if (ImGui::Button(m_creating ? "Back to list" : ICON_FA_PLUS " New Project",
                          {newBtnW, 0}))
            { m_creating = !m_creating; m_error.clear(); }

        if (winBtns > 0.0f) {
            ImGui::SetCursorPos({ ImGui::GetWindowWidth() - winBtns, y0 });
            edchrome::drawWindowButtons(rowH,
                [nativeWindow]{ platwin::minimizeWindow(nativeWindow); },
                [nativeWindow]{ platwin::toggleWindowZoom(nativeWindow); },
                [this]{ m_quit = true; });
        }

        ImGui::SetCursorPosY(y0 + rowH);
        ImGui::Separator();
        ImGui::Dummy({0, 6});

        if (m_creating) drawCreateForm(picked);
        else            drawProjectList(picked);

        if (!m_error.empty()) {
            ImGui::Dummy({0, 6});
            ImGui::TextColored({1.0f, 0.35f, 0.35f, 1.0f}, "%s", m_error.c_str());
        }

        ImGui::End();
        return picked;
    }

    void drawProjectList(std::filesystem::path& picked) {
        const float footerH = ImGui::GetFrameHeightWithSpacing() * 2.5f;
        ImGui::BeginChild("##list", {0, -footerH}, true);
        if (m_projects.empty())
            ImGui::TextDisabled("No projects yet — create one, or open an "
                                "existing folder below.");
        for (int i = 0; i < (int)m_projects.size(); ++i) {
            auto& kp = m_projects[i];
            ImGui::PushID(i);
            const bool missing = !kp.exists();

            std::string label = kp.name;
            if (ImGui::Selectable(("##row" + std::to_string(i)).c_str(),
                                  m_selected == i,
                                  ImGuiSelectableFlags_AllowDoubleClick,
                                  {0, 38})) {
                m_selected = i;
                if (ImGui::IsMouseDoubleClicked(0) && !missing)
                    picked = kp.path;
            }
            ImGui::SameLine(8); ImGui::BeginGroup();
            if (missing) ImGui::TextDisabled("%s   (missing)", kp.name.c_str());
            else         ImGui::Text("%s", kp.name.c_str());
            ImGui::TextDisabled("%s", kp.path.string().c_str());
            ImGui::EndGroup();
            ImGui::PopID();
        }
        ImGui::EndChild();

        // Footer: open selected / remove / open-by-path
        const bool haveSel = m_selected >= 0 &&
                             m_selected < (int)m_projects.size();
        ImGui::BeginDisabled(!haveSel || !m_projects[m_selected].exists());
        if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open", {120, 0}))
            picked = m_projects[m_selected].path;
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!haveSel);
        if (ImGui::Button("Remove from list", {140, 0})) {
            project::KnownProjects::remove(m_projects[m_selected].path);
            m_projects = project::KnownProjects::load();
            m_selected = -1;
        }
        ImGui::EndDisabled();

        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130);
        ImGui::InputTextWithHint("##openpath", "path to an existing project...",
                                 m_openPath, sizeof(m_openPath));
        ImGui::SameLine();
        if (ImGui::Button("Open path", {120, 0})) {
            std::filesystem::path p = m_openPath;
            if (project::isProject(p)) { m_error.clear(); picked = p; }
            else m_error = "No project.json found at: " + p.string();
        }
    }

    void drawCreateForm(std::filesystem::path& picked) {
        ImGui::Text("Create a new project");
        ImGui::Dummy({0, 4});

        ImGui::SetNextItemWidth(360);
        ImGui::InputTextWithHint("Name", "MyGame", m_name, sizeof(m_name));
        ImGui::SetNextItemWidth(360);
        ImGui::InputText("Location", m_location, sizeof(m_location));
        ImGui::TextDisabled("Project folder: %s",
            (std::filesystem::path(m_location) /
             (m_name[0] ? m_name : "MyGame")).string().c_str());
        ImGui::Dummy({0, 6});

        const auto& tpls = project::templates();
        for (int i = 0; i < (int)tpls.size(); ++i) {
            if (ImGui::RadioButton(tpls[i].name.c_str(), m_templateIdx == i))
                m_templateIdx = i;
            ImGui::SameLine();
            ImGui::TextDisabled("— %s", tpls[i].description.c_str());
        }
        ImGui::Dummy({0, 10});

        ImGui::BeginDisabled(m_name[0] == '\0');
        if (ImGui::Button(ICON_FA_PLUS " Create", {120, 0})) {
            std::filesystem::path dir =
                std::filesystem::path(m_location) / m_name;
            std::string err;
            if (project::create(dir, m_name, tpls[m_templateIdx].id, err)) {
                m_error.clear();
                picked = dir;
            } else {
                m_error = err;
            }
        }
        ImGui::EndDisabled();
    }
};
