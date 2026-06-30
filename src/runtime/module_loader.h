#pragma once
// ── Module loader — shared dlopen plumbing for game modules & kits ───────────
// The low-level half of loading a hot-reloadable C++ module: copy-and-dlopen,
// resolve the ENGINE_GAME_MODULE exports, run the compatibility gauntlet, and
// wrap the C function table as an IEnginePlugin. Orchestration (when to load,
// how it joins a registry, reload policy) lives with each caller — engine_host
// for the dev runner, KitHost for project kits.
//
// A module compiles against engine headers but links NONE of the engine; every
// engine symbol (flecs, the C API, logger) resolves from the host executable
// at load time. The host must therefore export its symbols — ENABLE_EXPORTS on
// the engine_host / editor targets.
#include <engine/game_module.h>
#include "runtime/plugin.h"
#include "runtime/runtime_context.h"
#include "core/logger.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX            // keep windows.h min/max macros out of bx/flecs/std
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace modload {

namespace fs = std::filesystem;

// ── Dynamic-library shim ─────────────────────────────────────────────────────
// The only platform-specific surface: open a shared library, look up a symbol,
// close it, report the last error. POSIX dlfcn on macOS/Linux, the Win32 loader
// on Windows. Everything above this (gauntlet, adapter, watcher) is portable.
#if defined(_WIN32)
using LibHandle = HMODULE;
inline LibHandle libOpen(const fs::path& p) { return ::LoadLibraryW(p.c_str()); }
inline void*     libSym(LibHandle h, const char* n) { return (void*)::GetProcAddress(h, n); }
inline void      libClose(LibHandle h) { if (h) ::FreeLibrary(h); }
inline std::string libError() {
    DWORD e = ::GetLastError();
    if (!e) return {};
    char* buf = nullptr;
    ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                     FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                     (char*)&buf, 0, nullptr);
    std::string s = buf ? buf : "unknown error";
    if (buf) ::LocalFree(buf);
    return s;
}
#else
using LibHandle = void*;
inline LibHandle libOpen(const fs::path& p) { return ::dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL); }
inline void*     libSym(LibHandle h, const char* n) { return ::dlsym(h, n); }
inline void      libClose(LibHandle h) { if (h) ::dlclose(h); }
inline std::string libError() { const char* e = ::dlerror(); return e ? e : ""; }
#endif

// ── Host-side adapter ────────────────────────────────────────────────────────
// Wraps the module's C function table as an IEnginePlugin for the registry.
// This object is HOST code — only the table's function pointers reach into the
// module, and every call null-checks. No C++ type crosses the boundary.
class GameModuleAdapter final : public IEnginePlugin {
public:
    explicit GameModuleAdapter(EngineGameModuleV1* t) : m_t(t) {}

    const char* name()    const override { return m_t->name    ? m_t->name    : "module"; }
    const char* version() const override { return m_t->version ? m_t->version : "?"; }

    void onAttach(RuntimeContext& c) override { if (m_t->attach) m_t->attach(m_t->user, &c); }
    void onDetach()                  override { if (m_t->detach) m_t->detach(m_t->user); }
    void onSimulationStart(flecs::world& w) override {
        if (m_t->simStart) m_t->simStart(m_t->user, w.c_ptr());
    }
    void onSimulationStop() override { if (m_t->simStop) m_t->simStop(m_t->user); }
    void onUpdate(flecs::world& w, float dt) override {
        if (m_t->update) m_t->update(m_t->user, w.c_ptr(), dt);
    }
    void onPhysicsStep(flecs::world& w, float dt) override {
        if (m_t->physicsStep) m_t->physicsStep(m_t->user, w.c_ptr(), dt);
    }
    void onPostPhysics(flecs::world& w) override {
        if (m_t->postPhysics) m_t->postPhysics(m_t->user, w.c_ptr());
    }
    void onEditorUI() override { if (m_t->editorUi) m_t->editorUi(m_t->user); }

private:
    EngineGameModuleV1* m_t;
};

// ── Module library ───────────────────────────────────────────────────────────
// One loaded shared module: the dlopen handle, the module-owned table, and the
// host-side adapter plugin. dlopen caches by path/inode, so we load a COPY —
// that sidesteps staleness and lets the build relink the original while the old
// code still runs. unload() must destroy the table (module-side) strictly
// before dlclose.
class ModuleLibrary {
public:
    ModuleLibrary() = default;
    ~ModuleLibrary() { unload(); }
    ModuleLibrary(const ModuleLibrary&)            = delete;
    ModuleLibrary& operator=(const ModuleLibrary&) = delete;

    bool load(const fs::path& sourcePath) {
        std::error_code ec;
        m_tempPath = fs::temp_directory_path()
                   / ("engine_module_" + std::to_string(nextTempId())
                      + sourcePath.extension().string());
        fs::copy_file(sourcePath, m_tempPath,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("Module", "Copy failed: %s (%s)",
                      sourcePath.string().c_str(), ec.message().c_str());
            return false;
        }

        m_handle = libOpen(m_tempPath);
        if (!m_handle) {
            LOG_ERROR("Module", "load failed: %s", libError().c_str());
            return false;
        }

        auto create  = (EngineGameModuleCreateV1Fn) libSym(m_handle, "engineGameModuleCreateV1");
        auto destroy = (EngineGameModuleDestroyV1Fn)libSym(m_handle, "engineGameModuleDestroyV1");
        if (!create || !destroy) {
            LOG_ERROR("Module", "Missing ENGINE_GAME_MODULE exports "
                      "(engineGameModuleCreateV1/DestroyV1)");
            unload();
            return false;
        }

        EngineGameModuleV1* t = create();
        if (!t) {
            LOG_ERROR("Module", "engineGameModuleCreateV1 returned null");
            unload();
            return false;
        }
        auto refuse = [&]() { destroy(t); unload(); return false; };

        // ── Compatibility gauntlet ──────────────────────────────────────────
        if (t->structSize != sizeof(EngineGameModuleV1)) {
            LOG_ERROR("Module", "Table size %u != host %zu — SDK mismatch, "
                      "rebuild the module", t->structSize,
                      sizeof(EngineGameModuleV1));
            return refuse();
        }
        if (t->apiVersion != ENGINE_GAME_API_VERSION) {
            LOG_ERROR("Module", "API version %u != host %d — rebuild against "
                      "the current SDK", t->apiVersion, ENGINE_GAME_API_VERSION);
            return refuse();
        }
        // Compiler + C++ standard + build mode must match exactly — a debug
        // module against a release host corrupts memory in ways no version int
        // catches.
        if (!t->abiFingerprint ||
            std::string(t->abiFingerprint) != ENGINE_ABI_FINGERPRINT) {
            LOG_ERROR("Module", "ABI mismatch:\n  host:   %s\n  module: %s",
                      ENGINE_ABI_FINGERPRINT,
                      t->abiFingerprint ? t->abiFingerprint : "(null)");
            return refuse();
        }
        // World data SURVIVES reloads; a module built against changed component
        // layouts would misread live ECS memory. Refuse, restart.
        if (t->componentLayoutHash != engine_abi::componentLayoutHash()) {
            LOG_ERROR("Module", "Component layout changed since the host was "
                      "built — RESTART the host (live world data would be "
                      "misread by the new module)");
            return refuse();
        }

        if (t->loadReason)
            t->loadReason(t->user, m_loadCount++ == 0
                          ? ENGINE_MODULE_LOAD_INITIAL
                          : ENGINE_MODULE_LOAD_HOTRELOAD);

        m_table   = t;
        m_destroy = destroy;
        m_plugin  = std::make_shared<GameModuleAdapter>(t); // host-side object
        return true;
    }

    // Destroy the module's instance + table (module-side) before dlclose. Safe
    // to call repeatedly. The caller MUST have already released every
    // shared_ptr to plugin() — the adapter's deleter lives in the host, but the
    // table it points at dies here.
    void unload() {
        if (m_table && m_destroy) m_destroy(m_table);  // module-side delete
        m_table   = nullptr;
        m_destroy = nullptr;
        if (m_handle) { libClose(m_handle); m_handle = nullptr; }
        std::error_code ec;
        if (!m_tempPath.empty()) { fs::remove(m_tempPath, ec); m_tempPath.clear(); }
    }

    std::shared_ptr<IEnginePlugin> plugin() const { return m_plugin; }
    bool loaded() const { return m_handle != nullptr; }

private:
    static unsigned nextTempId() {
        static std::atomic<unsigned> s_counter{0};
        return s_counter.fetch_add(1);
    }

    LibHandle                      m_handle  = nullptr;
    std::shared_ptr<IEnginePlugin> m_plugin;            // host-side adapter
    EngineGameModuleV1*            m_table   = nullptr; // module-owned
    EngineGameModuleDestroyV1Fn    m_destroy = nullptr;
    fs::path                       m_tempPath;
    int                            m_loadCount = 0;
};

// ── Module watcher ───────────────────────────────────────────────────────────
// Watches a module file; reports a change only once the mtime has been stable
// for a couple of polls (the linker may still be writing).
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

} // namespace modload
