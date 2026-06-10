// ── engine_host — hot-reload dev runner ─────────────────────────────────────
//
//   engine_host <project-dir> <game-module.dylib>
//
// Runs a project like a shipped game (window, scene, stock plugins,
// simulation from boot) but loads the game's logic from a shared library and
// reloads it live whenever the file changes on disk. World state survives
// reloads — entities, components, physics bodies, assets all live host-side;
// the module is pure logic (see include/engine/game_module.h for the rules).
//
// Reload loop:  edit C++  →  rebuild the module target  →  host picks it up
// in ~a second, calling onSimulationStart on the fresh code with the SAME
// running world.
#include <engine/engine.h>
#include <engine/game_module.h>
#include <engine/input.h>
#include "scene/scene_serializer.h"
#include "runtime/services/async_loader.h"

#include <dlfcn.h>
#include <cstdio>
#include <filesystem>
#include <string>

#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "plugins/audio_plugin.h"

namespace fs = std::filesystem;

// ── Module loader ────────────────────────────────────────────────────────────
// dlopen caches by path/inode; loading a COPY sidesteps any staleness and
// lets the build relink the original while the old code still runs.
class GameModule {
public:
    bool load(const fs::path& sourcePath) {
        std::error_code ec;
        m_tempPath = fs::temp_directory_path()
                   / ("engine_host_game_" + std::to_string(m_generation++)
                      + sourcePath.extension().string());
        fs::copy_file(sourcePath, m_tempPath,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("Host", "Copy failed: %s (%s)",
                      sourcePath.string().c_str(), ec.message().c_str());
            return false;
        }

        m_handle = dlopen(m_tempPath.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!m_handle) {
            LOG_ERROR("Host", "dlopen failed: %s", dlerror());
            return false;
        }

        auto create  = (EngineCreateGameFn)    dlsym(m_handle, "engineCreateGame");
        auto destroy = (EngineDestroyGameFn)   dlsym(m_handle, "engineDestroyGame");
        auto apiVer  = (EngineGameApiVersionFn)dlsym(m_handle, "engineGameApiVersion");
        auto abi     = (EngineGameAbiFingerprintFn)dlsym(m_handle, "engineGameAbiFingerprint");
        if (!create || !destroy || !apiVer || !abi) {
            LOG_ERROR("Host", "Module missing ENGINE_GAME_MODULE exports");
            unloadLibrary();
            return false;
        }
        if (apiVer() != ENGINE_GAME_API_VERSION) {
            LOG_ERROR("Host", "Module API version %d != host %d — rebuild "
                      "against the current SDK", apiVer(), ENGINE_GAME_API_VERSION);
            unloadLibrary();
            return false;
        }
        // ABI fingerprint: compiler + C++ standard + api + build mode must
        // match exactly — a debug module against a release host (or a
        // different compiler) corrupts memory in ways no version int catches.
        if (std::string(abi()) != ENGINE_ABI_FINGERPRINT) {
            LOG_ERROR("Host", "ABI mismatch:\n  host:   %s\n  module: %s",
                      ENGINE_ABI_FINGERPRINT, abi());
            unloadLibrary();
            return false;
        }
        // Optional hook: tell the module why it's being loaded.
        if (auto reason = (EngineGameLoadReasonFn)dlsym(m_handle, "engineGameLoadReason"))
            reason(m_generation <= 1 ? ENGINE_MODULE_LOAD_INITIAL
                                     : ENGINE_MODULE_LOAD_HOTRELOAD);

        m_destroy = destroy;
        m_plugin  = std::shared_ptr<IEnginePlugin>(
            create(), [destroy](IEnginePlugin* p) { if (p) destroy(p); });
        if (!m_plugin) {
            LOG_ERROR("Host", "engineCreateGame returned null");
            unloadLibrary();
            return false;
        }
        return true;
    }

    // Full reload: stop+detach old code, swap libraries, attach+start new
    // code against the SAME world. Module-internal state is rebuilt by
    // onSimulationStart; component state persists untouched.
    bool reload(const fs::path& sourcePath, EngineRuntime& engine) {
        if (m_plugin) {
            if (engine.simulating()) m_plugin->onSimulationStop();
            m_plugin->onDetach();
            engine.plugins().remove(m_plugin.get());
            m_plugin.reset();        // destroyed by module code, pre-dlclose
        }
        unloadLibrary();

        if (!load(sourcePath)) return false;
        engine.plugins().add(m_plugin);
        m_plugin->onAttach(engine.ctx());
        if (engine.simulating())
            m_plugin->onSimulationStart(engine.simWorld());
        LOG_SUCCESS("Host", "Reloaded: %s %s",
                    m_plugin->name(), m_plugin->version());
        return true;
    }

    // Tear down the game plugin while the module code is still loaded.
    // MUST run before EngineRuntime::shutdown destroys nothing module-side:
    // the shared_ptr deleter lives inside the dylib, so every reference has
    // to drop before dlclose.
    void shutdown(EngineRuntime& engine) {
        if (m_plugin) {
            if (engine.simulating()) m_plugin->onSimulationStop();
            m_plugin->onDetach();
            engine.plugins().remove(m_plugin.get());
            m_plugin.reset();
        }
        unloadLibrary();
    }

    std::shared_ptr<IEnginePlugin> plugin() const { return m_plugin; }

private:
    void unloadLibrary() {
        if (m_handle) { dlclose(m_handle); m_handle = nullptr; }
        std::error_code ec;
        if (!m_tempPath.empty()) fs::remove(m_tempPath, ec);
    }

    void*                          m_handle = nullptr;
    std::shared_ptr<IEnginePlugin> m_plugin;
    EngineDestroyGameFn            m_destroy = nullptr;
    fs::path                       m_tempPath;
    int                            m_generation = 0;
};

// Watches the module file; reports a change only once the mtime has been
// stable for a couple of polls (the linker may still be writing).
class ModuleWatcher {
public:
    explicit ModuleWatcher(const fs::path& path) : m_path(path) {
        m_lastSeen = mtime();
    }
    bool changed(float dt) {
        m_sinceCheck += dt;
        if (m_sinceCheck < 0.25f) return false;
        m_sinceCheck = 0.0f;

        auto t = mtime();
        if (t != m_lastSeen) {
            m_hasPending  = true;
            m_stablePolls = 0;
            m_lastSeen    = t;
            return false;
        }
        if (m_hasPending && ++m_stablePolls >= 2) {
            m_hasPending = false;
            return true;
        }
        return false;
    }
private:
    fs::file_time_type mtime() const {
        std::error_code ec;
        auto t = fs::last_write_time(m_path, ec);
        return ec ? fs::file_time_type{} : t;
    }
    fs::path           m_path;
    fs::file_time_type m_lastSeen{};
    bool               m_hasPending  = false;
    int                m_stablePolls = 0;
    float              m_sinceCheck  = 0.0f;
};

int main(int argc, char** argv) {
    // Dev tool: line-buffer stdout so logs stream to pipes/files live.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: engine_host <project-dir> <game-module.dylib>\n");
        return 2;
    }
    const fs::path projectDir = argv[1];
    const fs::path modulePath = fs::absolute(argv[2]);
    if (!fs::exists(modulePath)) {
        std::fprintf(stderr, "engine_host: module not found: %s\n",
                     modulePath.string().c_str());
        return 2;
    }

    EngineConfig cfg;
    cfg.projectRoot  = projectDir;
    cfg.defaultScene = false;
    auto platform = std::make_unique<GlfwPlatform>();
    GlfwPlatform* glfwPlat = platform.get();
    EngineRuntime engine;
    if (!engine.init(cfg, std::move(platform))) return 1;
    if (!engine.hasProject()) {
        std::fprintf(stderr, "engine_host: no project at %s\n",
                     projectDir.string().c_str());
        return 2;
    }

    // Host-side input — what game modules and Lua reach through the C API /
    // ScriptHost. Default WASD bindings match the editor's.
    InputSystem::get().init(glfwPlat->glfwWindow());
    auto& imap = InputMap::get();
    imap.bindAxis("MoveForward", Key::W, Key::S);
    imap.bindAxis("MoveRight",   Key::D, Key::A);
    imap.bindAxis("MoveUp",      Key::E, Key::Q);

    // Stock plugins first — broadcast order puts game logic after scripts.
    engine.plugins().add(std::make_shared<JoltPlugin>());
    engine.plugins().add(std::make_shared<LuaScriptPlugin>());
    engine.plugins().add(std::make_shared<AudioPlugin>());

    // Game module joins the registry before attach so it shares the
    // lifecycle with stock plugins. The watcher baseline is captured HERE —
    // at load time — so a rebuild that lands while the engine is still
    // booting is detected, not silently adopted as the baseline.
    GameModule game;
    if (!game.load(modulePath)) return 1;
    ModuleWatcher watcher(modulePath);
    engine.plugins().add(game.plugin());
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
        ctx.assetService, engine.project().projectRoot, &engine.assetLib());

    engine.startSimulation(); // in-place: boot = play

    engine.run([&](float dt) {
        InputSystem::get().processEvents();
        loader.drainOne(storage);
        if (watcher.changed(dt))
            game.reload(modulePath, engine);
        engine.tick(dt);
    });

    // Order matters: the game plugin must be fully released (its deleter
    // lives in the dylib) before the library unloads, and both before the
    // runtime tears down the world the module may still reference.
    engine.stopSimulation();  // broadcasts onSimulationStop to ALL plugins
    game.shutdown(engine);    // detach + release + dlclose
    engine.shutdown();        // stock plugins detach, world/devices die
    return 0;
}
