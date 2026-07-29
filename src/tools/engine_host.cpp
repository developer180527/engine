// ── engine_host — hot-reload dev runner ─────────────────────────────────────
//
//   engine_host <project-dir> [dev-module.dylib]
//
// Runs a project like a shipped game (window, scene, stock plugins,
// simulation from boot). The project's manifest kits load automatically — the
// runtime dlopens them at simulation start and hot-reloads them on change. An
// OPTIONAL extra module path on the command line is the one you're actively
// developing: loaded the same way and watched here. World state survives every
// reload — entities, components, physics bodies, assets all live host-side; a
// module is pure logic (see include/engine/game_module.h for the rules).
//
// Reload loop:  edit C++  →  rebuild the module target  →  host picks it up
// in ~a second, calling onSimulationStart on the fresh code with the SAME
// running world.
#include <engine/engine.h>
#include <engine/input.h>
#include "scene/scene_serializer.h"
#include "runtime/services/async_loader.h"
#include "runtime/module_loader.h"   // shared dlopen + gauntlet (also used by KitHost)
#include "core/profiler.h"           // periodic frame-profile dump (dev runner)
#include "runtime/frame_stats_channel.h"  // frame-time distribution + CSV
#include "core/memory/mem.h"         // periodic tagged-heap dump

#include <cstdio>
#include <cstdlib>   // strtol — --frames
#include <filesystem>
#include <string>

#include "plugins/stock_plugins.h"

namespace fs = std::filesystem;

// ── Dev module ───────────────────────────────────────────────────────────────
// The one module passed on the command line — the thing under active edit.
// Project kits go through the runtime's KitHost; this is the host's own
// hot-reload of a single extra module, built on the same shared loader. A full
// reload stops+detaches the old code, swaps libraries, then attaches+starts the
// new code against the SAME world (module state is rebuilt by onSimulationStart;
// component state persists untouched).
class DevModule {
public:
    bool load(const fs::path& sourcePath) {
        if (!m_lib.load(sourcePath)) return false;
        m_plugin = m_lib.plugin();
        return true;
    }

    bool reload(const fs::path& sourcePath, EngineRuntime& engine) {
        if (m_plugin) {
            if (engine.simulating()) m_plugin->onSimulationStop();
            m_plugin->onDetach();
            engine.plugins().remove(m_plugin.get());
            m_plugin.reset();        // host-side adapter — safe anywhere
        }
        m_lib.unload();              // destroys the table, then dlcloses

        if (!load(sourcePath)) return false;
        engine.plugins().add(m_plugin);
        m_plugin->onAttach(engine.ctx());
        if (engine.simulating())
            m_plugin->onSimulationStart(engine.simWorld());
        LOG_SUCCESS("Host", "Reloaded: %s %s",
                    m_plugin->name(), m_plugin->version());
        return true;
    }

    // Tear down the plugin while the module code is still loaded — the adapter
    // shared_ptr must be released before unload() (the table dies there).
    void shutdown(EngineRuntime& engine) {
        if (m_plugin) {
            if (engine.simulating()) m_plugin->onSimulationStop();
            m_plugin->onDetach();
            engine.plugins().remove(m_plugin.get());
            m_plugin.reset();
        }
        m_lib.unload();
    }

    std::shared_ptr<IEnginePlugin> plugin() const { return m_plugin; }

private:
    modload::ModuleLibrary         m_lib;
    std::shared_ptr<IEnginePlugin> m_plugin;   // host-side adapter
};

int main(int argc, char** argv) {
    // Dev tool: line-buffer stdout so logs stream to pipes/files live.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: engine_host <project-dir> [dev-module.dylib]\n"
            "       [--record-input <file.irec>]\n"
            "       [--frames N]            exit after N frames\n"
            "       [--frame-csv <file>]    per-frame timings for plotting\n");
        return 2;
    }
    const fs::path projectDir = argv[1];
    // Optional: --record-input <file.irec> — tee accepted input + tick marks
    // for the determinism/replay harness (see input_manager.h Recording).
    fs::path recordPath;
    // Profiling-run knobs: --frames N exits after N frames so a measurement is
    // scriptable (no human closing a window at an arbitrary moment), and
    // --frame-csv writes the raw per-frame timings for offline plotting.
    long     frameLimit = 0;      // 0 = run until the window closes
    fs::path frameCsv;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if      (a == "--record-input" && i + 1 < argc) recordPath = argv[++i];
        else if (a == "--frames"       && i + 1 < argc) frameLimit = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--frame-csv"    && i + 1 < argc) frameCsv   = argv[++i];
    }
    // argv[2] is the optional dev module ONLY if it isn't a --flag.
    const bool     hasDevModule = argc >= 3 && argv[2][0] != '-';
    const fs::path modulePath  = hasDevModule ? fs::absolute(argv[2]) : fs::path{};
    if (hasDevModule && !fs::exists(modulePath)) {
        std::fprintf(stderr, "engine_host: module not found: %s\n",
                     modulePath.string().c_str());
        return 2;
    }

    EngineConfig cfg;
    cfg.projectRoot  = projectDir;
    cfg.defaultScene = false;
    auto  platform = makeDefaultPlatform();
    auto* plat     = platform.get();
    EngineRuntime engine;
    if (!engine.init(cfg, std::move(platform))) return 1;
    if (!engine.hasProject()) {
        std::fprintf(stderr, "engine_host: no project at %s\n",
                     projectDir.string().c_str());
        return 2;
    }

    // Host-side input — what game modules and Lua reach through the C API /
    // ScriptHost. Default WASD bindings match the editor's.
#if !defined(ENGINE_WINDOW_BACKEND_SDL3)
    // The window input source is still GLFW-callback based; handing it an
    // SDL_Window* would be a crash. Inert (and safe) under sdl3 until the
    // SDL3 window input source lands.
    InputSystem::get().init(plat->backendWindowHandle());
#endif
    // (Legacy InputMap axis bindings removed: gameplay reads ACTIONS from
    // the project's input.json — see docs/engine-api.md. InputSystem remains
    // for editor/meta polling only.)

    // Stock providers first (by NAME from project.json's "providers" block —
    // see stock_plugins.h) — broadcast order puts game logic after scripts.
    addStockPlugins(engine);

    // Optional dev module joins the registry before attach so it shares the
    // lifecycle with stock plugins. The watcher baseline is captured HERE — at
    // load time — so a rebuild that lands while the engine is still booting is
    // detected, not silently adopted as the baseline. (Project kits load later,
    // inside startSimulation, via the runtime's KitHost.)
    DevModule game;
    std::unique_ptr<modload::ModuleWatcher> watcher;
    if (hasDevModule) {
        if (!game.load(modulePath)) return 1;
        watcher = std::make_unique<modload::ModuleWatcher>(modulePath);
        engine.plugins().add(game.plugin());
    }
    engine.attachPlugins();

    // Load the project's entry scene (async meshes drain in the frame loop).
    auto& ctx = engine.ctx();
    AssetStorage storage{ctx.assets, ctx.textures, ctx.materials,
                         ctx.skeletons, ctx.clips};
    AsyncLoader loader;
    loader.setRegistry(&engine.assetLib());
    loader.setProjectRoot(engine.project().projectRoot);
    SceneSerializer::loadAsync(
        engine.project().projectRoot / engine.project().lastScene,
        ctx.ecs, storage, loader, ctx.importers, ctx.primitives,
        ctx.assetService, engine.project().projectRoot, &engine.assetLib(),
        &engine.clipLibrary());

    if (!recordPath.empty())
        engine.inputManager().startRecording(recordPath);
    engine.startSimulation(); // in-place: boot = play

    // Explicit loop rather than engine.run() so --frames can stop it: a
    // measurement whose length depends on when someone closes a window is not
    // a measurement.
    long  profFrame = 0;
    float dt = 0.0f;
    while (engine.frameBegin(dt)) {
        InputSystem::get().processEvents();
        loader.drainOne(storage);
        if (watcher && watcher->changed(dt))
            game.reload(modulePath, engine);
        engine.tick(dt);
        // Dev runner: dump the frame profile every ~5s so windowed runs (the
        // only place GPU/present cost is real) leave numbers in the log.
        if (ENGINE_PROFILE && ++profFrame % 300 == 0) {
            prof::Profiler::get().timer().logLastFrame("engine_host frame");
            mem::logStats("engine_host");   // tagged heaps + map-event total
            if (engine.inputLatency())
                engine.inputLatency()->logLastFrame("engine_host");
            if (engine.frameStats())
                engine.frameStats()->logSummary("engine_host");
        }
        engine.frameEnd();
        if (frameLimit > 0 && profFrame >= frameLimit) break;
    }
    // Written before shutdown, which is where the distribution report prints.
    if (!frameCsv.empty() && engine.frameStats()) {
        if (engine.frameStats()->writeCsv(frameCsv.string()))
            std::printf("[FrameStats] wrote %s\n", frameCsv.string().c_str());
        else
            std::fprintf(stderr, "engine_host: cannot write %s\n",
                         frameCsv.string().c_str());
    }

    // Order matters: the dev plugin must be fully released (its deleter lives
    // in the dylib) before the library unloads, and both before the runtime
    // tears down the world the module may still reference. stopSimulation also
    // unloads the project's kits (KitHost) after broadcasting onSimulationStop.
    engine.stopSimulation();  // broadcasts onSimulationStop to ALL plugins
    if (hasDevModule) game.shutdown(engine);  // detach + release + dlclose
    engine.shutdown();        // stock plugins detach, world/devices die
    return 0;
}
