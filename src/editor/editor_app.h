#include <filesystem>
#pragma once
#include <GLFW/glfw3.h>
#include "runtime/runtime.h"
#include "scene/scene_serializer.h"
#include <imgui.h>
#include <imgui_internal.h>   // DockBuilder (Reset Layout)
#include "editor/editor_icons.h"
#include "editor/editor_prefs.h"
#include "editor/editor_theme.h"
#include "project/project_context.h"

#include "runtime/services/async_loader.h"
#include "runtime/plugin_registry.h"
#include "runtime/input/input.h"
#include "components/meta_registry.h"
#include "plugins/jolt_plugin.h"
#include "plugins/audio_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "assets/cookers/cook_service.h"
#include <assetlib/asset_registry.h>
#include "editor/engine_context.h"
#include "editor/editor_camera.h"
#include "editor/panels/hierarchy_panel.h"
#include "editor/panels/menu_bar_panel.h"
#include "editor/panels/game_view_panel.h"
#include "editor/panels/inspector_panel.h"
#include "editor/panels/asset_browser_panel.h"
#include "editor/panels/console_panel.h"
#include "editor/panels/plugins_panel.h"
#include "editor/panels/profiler_panel.h"
#include "editor/panels/project_settings_window.h"
#include "editor/gizmo.h"
#include "editor/imgui/imgui_bgfx.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <chrono>
#include <algorithm>

class EditorApp {
public:
    // The editor requires a GLFW-backed platform — it owns the window pointer
    // for ImGui glue, input callbacks, and cursor capture.
    EditorApp(EngineRuntime& rt, GLFWwindow* window)
        : m_rt(rt), m_window(window) {}
    ~EditorApp() = default;

    EditorApp(const EditorApp&)            = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    void setRegistry(assetlib::AssetRegistry* r)          { m_assetLib = r; m_loader.setRegistry(r); }
    void setProjectRoot(const std::filesystem::path& root){ m_loader.setProjectRoot(root); }
    void setCookService(CookService* cs) { m_cookService = cs; }
    void requestAssetRefresh() {
        if (m_cookService) m_cookService->requestRefresh();
    }
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
                             m_rt.ctx().materials,
                             m_rt.ctx().skeletons,
                             m_rt.ctx().clips};
        SceneSerializer::loadAsync(m_scenePath,
                                   m_rt.ctx().ecs,
                                   storage,
                                   m_loader,
                                   m_rt.ctx().importers,
                                   m_rt.ctx().primitives,
                                   m_rt.ctx().assetService,
                                   ctx.projectRoot,
                                   m_rt.ctx().assetLib);

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
        SceneSerializer::save(m_scenePath, m_rt.ctx().ecs, m_rt.ctx().assets,
                              m_assetLib, m_projectRoot);

        // Auto-cook: JSON → binary so the runtime format is always in sync.
        // Runs on the main thread (fast — just JSON parse + memcpy writes).
        {
            namespace fs = std::filesystem;
            auto cacheDir   = m_projectRoot / ".cache" / "scenes";
            auto cookedPath = cacheDir / (m_scenePath.stem().string() + ".cooked");
            if (SceneSerializer::cookScene(m_scenePath, cookedPath,
                                           m_assetLib, m_projectRoot))
                LOG_INFO("Project", "Binary scene updated → %s",
                         cookedPath.filename().string().c_str());
        }

        // Persist editor state
        std::string selName;
        if (m_editor.selected.is_alive())
            if (const Name* n = m_editor.selected.try_get<Name>())
                selName = n->value;
        EditorPrefs::save(m_projectRoot, m_cam, selName);
        LOG_SUCCESS("Project", "Scene saved");
    }

    // Requires ImGui to be initialized already (main.cpp does imguiInit +
    // applyEditorTheme before the project hub, which also draws ImGui).
    void init() {
        // Input system — register GLFW callbacks + default action bindings
        InputSystem::get().init(m_window);
        auto& map = InputMap::get();
        map.bindAction("Jump",        Key::Space);
        map.bindAxis  ("MoveForward", Key::W,    Key::S);
        map.bindAxis  ("MoveRight",   Key::D,    Key::A);
        map.bindAxis  ("MoveUp",      Key::E,    Key::Q);
        map.bindAction("Interact",    Key::F);
        map.bindAction("Sprint",      Key::LeftShift);

        // Register component meta schemas (drives Lua FFI, Blueprint, inspector)
        MetaRegistry::registerAll(m_rt.ctx().ecs);
        // Register plugins with the runtime — it owns their lifecycle.
        // A standalone game does exactly this, minus the editor.
        m_rt.plugins().add(std::make_shared<JoltPlugin>());
        m_rt.plugins().add(std::make_shared<LuaScriptPlugin>());
        m_rt.plugins().add(std::make_shared<AudioPlugin>());
        m_rt.attachPlugins();
    }

    void run() {
        // The runtime owns the loop skeleton (events, timing, resize, async
        // drain, frame flip); the editor supplies the per-frame body.
        m_rt.run([this](float dt) { frame(dt); });
    }

private:
    void frame(float dt) {
            // ---- Events (platform polled by runtime in frameBegin) ----
            InputSystem::get().processEvents(); // drain queue → update double-buffer

            // ---- Drain legacy async loader (runtime drains AssetService) ----
            {
                AssetStorage storage{m_rt.ctx().assets,
                                     m_rt.ctx().textures,
                                     m_rt.ctx().materials,
                                     m_rt.ctx().skeletons,
                                     m_rt.ctx().clips};
                m_loader.drainOne(storage); // legacy loader — one per frame
            }

            // ---- ImGui frame start (must come before any ImGui calls) ----
            imguiNewFrame();
            // Gate gameplay input when ImGui owns the keyboard/mouse. EXCEPT
            // when the game has locked the cursor (FPS capture): the game owns
            // the mouse exclusively, so don't let ImGui's WantCaptureMouse
            // (which flickers true on the frozen cursor) eat clicks — that was
            // silently disabling shooting mid-play. Freeing the cursor (Esc)
            // restores normal ImGui gating so panels stay clickable.
            { auto& io = ImGui::GetIO();
              const bool __playing = (m_editor.simState == SimState::Playing);
              const bool __cursorLocked =
                  glfwGetInputMode(m_window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED;
              InputSystem::get().setUICapture(
                  __playing ? io.WantTextInput : io.WantCaptureKeyboard,
                  __cursorLocked ? false : io.WantCaptureMouse); }
            // BeginFrame MUST be outside any Begin/End block —
            // it creates an internal transparent overlay window.
            gizmoBeginFrame();
            { auto ctx = buildCtx(); gizmoHandleHotkeys(m_window, ctx); }

            // ---- Editor camera ----
            // Use the GLFW window that currently hosts the Scene View panel.
            // When docked: main window. When detached: the OS child window.
            GLFWwindow* camWin = m_sceneGLFWWindow ? m_sceneGLFWWindow : m_window;
            updateEditorCamera(m_cam, m_input, camWin, dt, m_sceneViewHovered);
            // ---- Play-mode cursor capture (FPS mouse-look) ----
            {
                GLFWwindow* gw = m_window;
                const bool playing = (m_editor.simState == SimState::Playing);
                if (playing && !m_wasPlaying) m_playCursorLocked = true;
                if (playing) {
                    const bool escNow = glfwGetKey(gw, GLFW_KEY_ESCAPE) == GLFW_PRESS;
                    if (escNow && !m_escPrev) {
                        if (m_playCursorLocked) m_playCursorLocked = false; // 1st Esc: free mouse
                        else onStop();                                       // 2nd Esc: stop play
                    }
                    m_escPrev = escNow;
                    const int want = m_playCursorLocked ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL;
                    if (glfwGetInputMode(gw, GLFW_CURSOR) != want)
                        glfwSetInputMode(gw, GLFW_CURSOR, want);
                } else {
                    m_escPrev = false;
                    if (m_playCursorLocked) { m_playCursorLocked = false;
                        glfwSetInputMode(gw, GLFW_CURSOR, GLFW_CURSOR_NORMAL); }
                }
                m_wasPlaying = playing;
            }

            // ---- Camera matrices ----
            float view[16], proj[16];
            m_cam.getViewMatrix(view);
            bx::mtxProj(proj, m_rt.fov(),
                        float(m_rt.sceneW()) / float(m_rt.sceneH()),
                        0.1f, 1000.0f,
                        bgfx::getCaps()->homogeneousDepth);

            // ---- Runtime tick (ECS systems + scene render) ----
            // Resize scene FB before rendering — avoids race condition
            // where tick() renders to old FB while panel shows new empty FB.
            // The FB aspect MUST equal the panel aspect: the Scene View
            // stretches the texture to fill the panel, so any mismatch
            // distorts the image (a strip-shaped panel squishes the scene).
            // Supersample by a UNIFORM factor (capped 2x) for free SSAA —
            // per-axis max() would change the aspect.
            const float ssaa = std::min(2.0f, std::max(1.0f,
                std::min((float)m_rt.width()  / (float)m_desiredSceneW,
                         (float)m_rt.height() / (float)m_desiredSceneH)));
            const int renderW = (int)(m_desiredSceneW * ssaa);
            const int renderH = (int)(m_desiredSceneH * ssaa);
            // Debounce: recreate the FB only once the panel size has been
            // stable for a few frames (a live drag changes it every frame —
            // recreating per frame churns 6 GPU resources each time), and
            // only when it differs by > 8px (resize-animation hysteresis).
            if (m_desiredSceneW == m_lastDesiredW &&
                m_desiredSceneH == m_lastDesiredH) {
                if (m_desiredStableFrames < 1000) ++m_desiredStableFrames;
            } else {
                m_desiredStableFrames = 0;
                m_lastDesiredW = m_desiredSceneW;
                m_lastDesiredH = m_desiredSceneH;
            }
            const int dw = std::abs(renderW - m_rt.sceneW());
            const int dh = std::abs(renderH - m_rt.sceneH());
            if ((dw > 8 || dh > 8) && m_desiredStableFrames >= 5)
                m_rt.createSceneFB(renderW, renderH);

            m_rt.tick(dt, view, proj, ImGuizmo::IsUsing());

            // ---- Game world update (plugins tick during play) ----
            // Paused skips this — the runtime no-ops only when not simulating,
            // so pause is purely "don't call tickSimulation".
            if (m_editor.simState == SimState::Playing)
                m_rt.tickSimulation(dt);

            // ---- Editor UI (all ImGui panel draws) ----
            renderUI(view, proj);

            // ---- Submit ImGui draw data (runtime flips in frameEnd) ----
            imguiRender();
            imguiRenderViewports();
    }

public:
    void shutdown() {
        // Plugins detach in EngineRuntime::shutdown() — runtime owns them.
        imguiShutdown();
    }

private:
    EngineRuntime&  m_rt;
    GLFWwindow*     m_window = nullptr; // owned by the runtime's GlfwPlatform
    std::filesystem::path m_projectRoot;
    std::filesystem::path m_scenePath;
    AsyncLoader      m_loader;
    assetlib::AssetRegistry* m_assetLib    = nullptr;
    CookService*             m_cookService = nullptr;
    EditorCamera   m_cam;
    EditorInput    m_input;
    PrimaryCameraFinder m_cameraFinder; // game-view camera (cached query)
    EditorState    m_editor;
    GizmoState     m_gizmo;
    bool           m_sceneViewHovered = true;
    bool           m_playCursorLocked = false;
    bool           m_escPrev          = false;
    bool           m_wasPlaying       = false;
    float          m_sceneAspect      = 16.0f / 9.0f;
    GLFWwindow*    m_sceneGLFWWindow  = nullptr; // GLFW window currently hosting Scene View
    int            m_desiredSceneW    = 1280;
    int            m_desiredSceneH    = 720;
    int            m_lastDesiredW     = 0;    // FB-recreate debounce
    int            m_lastDesiredH     = 0;
    int            m_desiredStableFrames = 0;
    // Play mode — sim state/world owned by the runtime (m_rt.simWorld()).
    bool                          m_showProjectSettings = false;
    bool                          m_showProfiler        = false;
    bool                          m_showPlugins         = true;
    bool                          m_focusPlugins        = false;
    bool                          m_showKitErrorModal   = false;
    PluginWindows                 m_pluginWindows;   // per-plugin dockable windows
    PanelVisibility               m_panels;          // View > Panels toggles
    bool                          m_resetLayout      = false;
    ProjectSettingsState          m_projectSettingsState;

    // Build a fresh EngineContext for this frame's panels.
    // Stack-allocated — valid only for the duration of renderUI().
    EngineContext buildCtx() {
        auto& rc = m_rt.ctx();
        return EngineContext{
            rc.ecs, rc.assets, rc.textures,
            rc.materials, rc.project, rc.importers,
            m_editor, m_gizmo, rc.assetLib, rc.primitives,
            rc.assetService, rc.sceneService,
            rc.skeletons, rc.clips
        };
    }

    void onPlay() {
        if (m_editor.simState == SimState::Paused) {
            m_editor.simState = SimState::Playing; return;
        }
        if (m_editor.simState != SimState::Editing) return;
        // Assets still streaming in snapshot WITHOUT their meshes — the game
        // world is populated instantly and won't receive late async fixups.
        if (m_loader.pendingCount() > 0)
            LOG_WARN("Sim", "%d asset(s) still loading — they will be missing "
                     "from this play session", m_loader.pendingCount());
        // Snapshot mode: the runtime serializes the editor world and
        // simulates a fresh copy, so Stop restores editing state untouched.
        if (m_rt.startSimulation(EngineRuntime::SimMode::Snapshot)) {
            m_editor.simState = SimState::Playing;
            // Surface any kit that failed to load (bad path, ABI mismatch).
            if (m_rt.kits().anyFailed()) m_showKitErrorModal = true;
        }
    }
    void onPause() {
        if (m_editor.simState == SimState::Playing)
            m_editor.simState = SimState::Paused;
        else if (m_editor.simState == SimState::Paused)
            m_editor.simState = SimState::Playing;
    }
    void onStop() {
        if (m_editor.simState == SimState::Editing) return;
        m_rt.stopSimulation();
        m_cameraFinder.reset(); // sim world died — drop its cached query
        m_editor.simState = SimState::Editing;
    }
    // Rebuild the dockspace to a sane default (View > Reset Layout). Also
    // un-hides every panel, so this recovers a lost/broken layout in one click.
    void buildDefaultLayout(ImGuiID root) {
        ImGui::DockBuilderRemoveNode(root);
        ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

        ImGuiID center = root, left, right, bottom, bottomR;
        left    = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left,  0.18f, nullptr, &center);
        right   = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f, nullptr, &center);
        bottom  = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,  0.30f, nullptr, &center);
        bottomR = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.40f, nullptr, &bottom);

        ImGui::DockBuilderDockWindow(ICON_FA_SITEMAP " Hierarchy",  left);
        ImGui::DockBuilderDockWindow(ICON_FA_EYE " Scene View",     center);
        ImGui::DockBuilderDockWindow(ICON_FA_GAMEPAD " Game View",  center);
        ImGui::DockBuilderDockWindow(ICON_FA_WRENCH " Inspector",   right);
        ImGui::DockBuilderDockWindow(ICON_FA_CHART_LINE " Stats",   right);
        ImGui::DockBuilderDockWindow(ICON_FA_TERMINAL " Console",   bottom);
        ImGui::DockBuilderDockWindow(ICON_FA_FOLDER_OPEN " Assets", bottom);
        ImGui::DockBuilderDockWindow(ICON_FA_SCREWDRIVER_WRENCH " Plug-in Manager", bottomR);
        ImGui::DockBuilderFinish(root);

        m_panels = PanelVisibility{};   // reveal everything again
        m_showPlugins = true;
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
        ImGui::Begin(ICON_FA_EYE " Scene View", &m_panels.sceneView);
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
            if (m_resetLayout) { buildDefaultLayout(dsid); m_resetLayout = false; }
            ImGui::DockSpace(dsid, ImVec2(0,0),
                ImGuiDockNodeFlags_None);

            ImGui::End();
        }

        // ── Menu bar ─────────────────────────────────────────────
        drawMenuBar({
            [this]{ saveScene(); },
            [this]{ setProject(m_rt.ctx().project); },
            [this]{ glfwSetWindowShouldClose(m_window, true); },
            [this]{ m_showProjectSettings = true; },
            [this]{ m_editor.undoStack.undo(m_rt.ctx().ecs); },
            [this]{ m_editor.undoStack.redo(m_rt.ctx().ecs); },
            m_editor.undoStack.canUndo(),
            m_editor.undoStack.canRedo(),
            m_editor.undoStack.undoDescription(),
            m_editor.undoStack.redoDescription(),
            m_projectRoot.empty() ? std::string{}
                : m_projectRoot.filename().string(),
            &m_showProfiler,
            &m_showPlugins,
            &m_focusPlugins,
            &m_panels,
            [this]{ m_resetLayout = true; }
        });

        // Cmd+S: save  |  Cmd+P: play/pause  |  Escape: stop
        if (ImGui::IsKeyDown(ImGuiKey_LeftSuper) &&
            ImGui::IsKeyPressed(ImGuiKey_S, false))
            saveScene();
        if (ImGui::IsKeyDown(ImGuiKey_LeftSuper) &&
            !ImGui::IsKeyDown(ImGuiKey_LeftShift) &&
            ImGui::IsKeyPressed(ImGuiKey_Z, false))
            m_editor.undoStack.undo(m_rt.ctx().ecs);
        if (ImGui::IsKeyDown(ImGuiKey_LeftSuper) &&
            ImGui::IsKeyDown(ImGuiKey_LeftShift) &&
            ImGui::IsKeyPressed(ImGuiKey_Z, false))
            m_editor.undoStack.redo(m_rt.ctx().ecs);
        if (ImGui::IsKeyDown(ImGuiKey_LeftSuper) &&
            ImGui::IsKeyPressed(ImGuiKey_Comma, false)) // Cmd+,
            m_showProjectSettings = true;
        if (ImGui::IsKeyDown(ImGuiKey_LeftSuper) &&
            ImGui::IsKeyPressed(ImGuiKey_P, false)) {
            if (m_editor.simState == SimState::Editing) onPlay();
            else onPause();
        }

        auto ctx = buildCtx();

        if (m_panels.sceneView) drawSceneViewPanel(view, proj, ctx);

        // ── Game view ────────────────────────────────────────────
        float gameView[16], gameProj[16], gameClear[4];
        float aspect = m_rt.sceneW() > 0
            ? (float)m_rt.sceneW() / (float)m_rt.sceneH() : 16.0f/9.0f;
        // During play: use sim world camera + render sim world entities
        // During edit: use editor world camera (simWorld() == editor world)
        const bool inSim = m_editor.simState != SimState::Editing
                           && m_rt.simulating();
        flecs::world& camWorld = inSim ? m_rt.simWorld() : m_rt.ctx().ecs;
        bool hasCam = m_cameraFinder.find(camWorld, gameView, gameProj,
                                          aspect, gameClear);
        if (hasCam) {
            flecs::world* renderWorld = inSim ? &m_rt.simWorld() : nullptr;
            m_rt.renderGameView(gameView, gameProj, gameClear, renderWorld);
        }
        drawGameViewPanel(m_rt.gameColorTex(), hasCam,
                         m_editor.simState,
                         m_rt.sceneW(), m_rt.sceneH(),
                         [this]{ onPlay(); },
                         [this]{ onPause(); },
                         [this]{ onStop(); },
                         &m_panels.gameView);

        // Stats
        if (m_panels.stats) {
        ImGui::Begin(ICON_FA_CHART_LINE " Stats", &m_panels.stats);
        ImGui::Text("FPS: %.1f",      ImGui::GetIO().Framerate);
        ImGui::Text("Frame: %.2f ms", 1000.0f / std::max(ImGui::GetIO().Framerate, 1.0f));
        ImGui::Text("Renderer: Metal");
        ImGui::Text("Camera: (%.2f, %.2f, %.2f)",
                    m_cam.position.x, m_cam.position.y, m_cam.position.z);
        ImGui::Text("Yaw: %.2f  Pitch: %.2f", m_cam.yaw, m_cam.pitch);
        ImGui::Separator();
        ImGui::TextDisabled("Hold RMB + WASD/QE to fly. Shift = faster.");
        if (m_cookService) {
            auto cs = m_cookService->stats();
            ImGui::Separator();
            if (cs.active) {
                ImGui::TextColored({1.0f,0.8f,0.2f,1.0f},
                    "Cooking: %s", cs.currentAsset.c_str());
                ImGui::Text("Progress: %d / %d", cs.cooked, cs.total);
            } else {
                ImGui::TextColored({0.4f,1.0f,0.4f,1.0f}, "Assets ready");
            }
            if (cs.failed > 0)
                ImGui::TextColored({1.0f,0.3f,0.3f,1.0f},
                    "Failed: %d", cs.failed);
        }
        ImGui::End();
        }

        drawHierarchyPanel(ctx, &m_panels.hierarchy);
        drawInspectorPanel(ctx, &m_panels.inspector);
        drawAssetBrowserPanel(ctx, m_loader, m_cookService, &m_panels.assets);
        drawConsolePanel(&m_panels.console);
        drawPluginsPanel(&m_showPlugins, &m_focusPlugins, m_rt.plugins(),
                         m_rt.project(), m_rt.kits(), m_rt.simulating(),
                         m_pluginWindows);
        drawPluginWindows(m_rt.plugins(), m_pluginWindows, m_rt.simulating());
        drawKitErrorModal(&m_showKitErrorModal, m_rt.kits());
        drawProfilerPanel(&m_showProfiler, m_rt.frameArena());
        drawProjectSettings(&m_showProjectSettings,
                            m_editor.simState,
                            m_projectSettingsState);
    }
};
