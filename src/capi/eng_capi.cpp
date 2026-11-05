// ── eng_capi — the engine's C API over today's internals (WO-070) ────────────
//
// One C API is the engine's only public boundary (DR-0028). This file is its
// first implemented part: create the engine, step it, run it, and control the
// main window. It sits over EngineRuntime and IPlatform; nothing in them is
// exposed. Every entry point catches every exception: none may cross a C
// boundary (from Rust or Zig that is undefined behaviour).
//
// This version's limits, each refused by name rather than ignored (DR-0035):
// one window, created before the first engStep; VSYNC presentation only; no
// frame limit (WO-061); no module list; no log callback.
#include <eng/engine.h>

#include "plugins/stock_plugins.h"
#include "project/project_context.h"
#include "runtime/input/input_system.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/platform/platform.h"
#include "runtime/runtime.h"
#include "runtime/module_loader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

thread_local std::string t_lastError = "no error";

EngResult fail(EngResult code, std::string msg) {
    t_lastError = std::move(msg);
    return code;
}

template <class R, class F>
R guard(R onError, F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        t_lastError = "out of memory inside the engine";
    } catch (const std::exception& e) {
        t_lastError = std::string("internal error: ") + e.what();
    } catch (...) {
        t_lastError = "internal error: an exception reached the C boundary";
    }
    return onError;
}

// Copy a struct the engine fills: the caller's struct_size says how much is theirs.
template <class T>
void writeOut(T* out, T full) {
    full.struct_size = out->struct_size;
    std::memcpy(out, &full, std::min<size_t>(out->struct_size, sizeof(T)));
}

// Read a struct the caller fills: only struct_size bytes, the rest zero.
template <class T>
bool readIn(const T* in, T& out) {
    out = T{};
    if (!in || in->struct_size < sizeof(uint32_t)) return false;
    std::memcpy(&out, in, std::min<size_t>(in->struct_size, sizeof(T)));
    return true;
}

// Before 1.0 every minor version may break (engine-api.md §6), so the exact
// version is required; an unset one (0) is refused at every version.
const char* versionMismatch(uint32_t v) {
    if (v == 0) return "api_version is not set: build the EngEngineDesc with ENG_INIT and set "
                       "api_version = ENG_API_VERSION";
    if ((v >> 16) != ENG_API_VERSION_MAJOR) return "built against a different major version of the engine API";
    if ((v & 0xFFFFu) > ENG_API_VERSION_MINOR) return "built against a newer engine API than this library";
    if (ENG_API_VERSION_MAJOR == 0 && (v & 0xFFFFu) != ENG_API_VERSION_MINOR)
        return "built against a different 0.x engine API; before 1.0 every minor version is breaking";
    return nullptr;
}

constexpr size_t kEventQueueLimit = 256;

} // namespace

struct EngEngine {
    std::string appName = "engine";
    std::string projectPath;
    std::string physics, scripting, audio;   // overrides of project.json; empty: keep
    EngPacing   pacing = ENG_PACING_ENGINE;

    std::unique_ptr<IPlatform>     platform;  // created at once: monitors are listable before a window
    IPlatform*                     plat = nullptr;
    std::unique_ptr<EngineRuntime> rt;
    bool started = false, headless = false, quit = false;

    bool     windowAlive = false;
    uint32_t windowGeneration = 0;
    uint32_t windowFlags = 0;
    bool     windowHdr = false;

    std::deque<EngEvent> events;
    uint32_t             dropped = 0;
    std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point last;
    bool     firstStep = true;
    uint64_t frame = 0;

    EngWindow windowHandle() const { return windowAlive ? ((windowGeneration << 8) | 1u) : 0; }
    bool validWindow(EngWindow w) const { return windowAlive && w == windowHandle(); }
};

namespace {

EngResult checkWindow(EngEngine* e, EngWindow w) {
    if (!e) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine is NULL");
    if (w == 0) return fail(ENG_ERROR_INVALID_ARGUMENT, "window is 0 (none)");
    if (!e->validWindow(w)) return fail(ENG_ERROR_STALE_HANDLE, "that window no longer exists");
    return ENG_OK;
}

// Starts the runtime: with the window described, or headless. What
// engine_player does at boot: providers by name, plugins attached, kits loaded
// in place (a C API engine never hot-reloads, DR-0032), the project's cooked
// scene loaded, simulation started.
EngResult start(EngEngine* e, const EngWindowDesc* wd) {
    EngineConfig cfg;
    // No title given: a project's name (the runtime's own default), else the app.
    cfg.title = (wd && wd->title) ? wd->title : (e->projectPath.empty() ? e->appName : std::string());
    cfg.width  = (wd && wd->width)  ? (int)wd->width  : 1280;
    cfg.height = (wd && wd->height) ? (int)wd->height : 720;
    cfg.startHidden = wd && (wd->flags & ENG_WINDOW_HIDDEN);
    cfg.hdrOutput = wd && (wd->flags & ENG_WINDOW_HDR_IF_AVAILABLE);
    cfg.defaultScene = false;
    cfg.openAssetDatabase = false;
    cfg.enableProfiler = false;
    if (e->projectPath.empty()) cfg.autoDetectProject = false;
    else cfg.projectRoot = e->projectPath;

    std::unique_ptr<IPlatform> platform;
    if (wd) platform = std::move(e->platform);
    else    platform = std::make_unique<HeadlessPlatform>();
    IPlatform* plat = platform.get();

    auto rt = std::make_unique<EngineRuntime>();
    // Any failure from here on: shut what started down, and give the engine a
    // fresh platform, so it is exactly as before the call (the window path
    // handed its platform to the runtime).
    auto abandon = [&](EngResult code, std::string why) {
        rt->shutdown();
        rt.reset();
        if (wd) { e->platform = makeDefaultPlatform(); e->plat = e->platform.get(); }
        return fail(code, std::move(why));
    };
    if (!rt->init(cfg, std::move(platform)))
        return abandon(ENG_ERROR_UNSUPPORTED, wd ? "the window could not be created on this platform"
                                                 : "the engine could not start headless");
    if (!e->projectPath.empty() && !rt->hasProject())
        return abandon(ENG_ERROR_NOT_FOUND, "no project.json at " + e->projectPath);

    auto& prov = rt->project().providers;
    if (!e->physics.empty())   prov.physics = e->physics;
    if (!e->scripting.empty()) prov.scripting = e->scripting;
    if (!e->audio.empty())     prov.audio = e->audio;

#if !defined(ENGINE_WINDOW_BACKEND_SDL3)
    if (wd) InputSystem::get().init(plat->backendWindowHandle());
#endif
    addStockPlugins(*rt);
    rt->attachPlugins();
    rt->kits().setReloadPolicy(modload::ModuleLibrary::Reload::Never);

    if (rt->hasProject()) {
        const std::string stem = fs::path(rt->project().lastScene).stem().string();
        const std::string cookedRel = "scenes/" + stem + ".cooked";
        if (rt->sceneService().loadScene(cookedRel.c_str()) == 0)
            return abandon(ENG_ERROR_NOT_FOUND, "the cooked scene " + cookedRel + " failed to load");
    }
    rt->startSimulation();

    e->plat = plat;
    e->rt = std::move(rt);
    e->started = true;
    e->headless = (wd == nullptr);
    return ENG_OK;
}

EngEvent blankEvent(EngEngine* e, EngEventType t, EngWindow w) {
    EngEvent ev{};
    ev.struct_size = sizeof(EngEvent);
    ev.type = t;
    ev.window = w;
    ev.time_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - e->created).count();
    return ev;
}

void queue(EngEngine* e, const EngEvent& ev) {
    if (e->events.size() >= kEventQueueLimit) { e->events.pop_front(); ++e->dropped; }
    e->events.push_back(ev);
}

// After a pump: the platform's window events into the C queue.
void gather(EngEngine* e) {
    if (!e->started || e->headless || !e->plat) return;
    std::vector<WindowEvent> evs;
    e->plat->takeWindowEvents(evs);
    const EngWindow w = e->windowHandle();
    for (const WindowEvent& we : evs) {
        using T = WindowEvent::Type;
        EngEvent ev{};
        switch (we.type) {
        case T::CloseRequested: ev = blankEvent(e, ENG_EVENT_WINDOW_CLOSE_REQUESTED, w); break;
        case T::Closed:         ev = blankEvent(e, ENG_EVENT_WINDOW_CLOSED, w); break;
        case T::Resized:
            ev = blankEvent(e, ENG_EVENT_WINDOW_RESIZED, w);
            ev.width = (uint32_t)we.width; ev.height = (uint32_t)we.height; ev.a = we.a; ev.b = we.b;
            break;
        case T::Moved:          ev = blankEvent(e, ENG_EVENT_WINDOW_MOVED, w); ev.x = we.x; ev.y = we.y; break;
        case T::FocusChanged:   ev = blankEvent(e, ENG_EVENT_WINDOW_FOCUS_CHANGED, w); ev.a = we.a; break;
        case T::MinimisedChanged: ev = blankEvent(e, ENG_EVENT_WINDOW_MINIMISED_CHANGED, w); ev.a = we.a; break;
        case T::ModeChanged:    ev = blankEvent(e, ENG_EVENT_WINDOW_MODE_CHANGED, w); ev.a = we.a; break;
        case T::MonitorChanged: ev = blankEvent(e, ENG_EVENT_WINDOW_MONITOR_CHANGED, w); ev.monitor = we.monitor; break;
        }
        queue(e, ev);
    }
}

WindowMode toMode(EngWindowMode m) {
    return m == ENG_WINDOW_MODE_BORDERLESS_FULLSCREEN ? WindowMode::BorderlessFullscreen
         : m == ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN  ? WindowMode::ExclusiveFullscreen
                                                      : WindowMode::Windowed;
}

const char* modeName(EngWindowMode m) {
    return m == ENG_WINDOW_MODE_BORDERLESS_FULLSCREEN ? "borderless fullscreen"
         : m == ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN  ? "exclusive fullscreen" : "windowed";
}

// Pixels from a monitor's content scale; display modes cross the C API in pixels.
float monitorScale(EngEngine* e, uint32_t id) {
    for (const MonitorDesc& m : e->plat->monitors())
        if (m.id == id) return m.contentScale > 0 ? m.contentScale : 1.0f;
    return 1.0f;
}

EngResult applyMode(EngEngine* e, EngWindowMode mode, EngMonitor monitor, const EngDisplayMode* ex) {
    if (mode > ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN)
        return fail(ENG_ERROR_INVALID_ARGUMENT, "unknown window mode " + std::to_string(mode));
    if (mode == ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN && !e->plat->canExclusiveFullscreen())
        return fail(ENG_ERROR_UNSUPPORTED, "exclusive fullscreen is not available on this platform");
    DisplayModeDesc d{};
    const DisplayModeDesc* dp = nullptr;
    if (ex && ex->width_pixels > 0) {
        const float s = monitorScale(e, monitor ? monitor : 0);
        d.width  = (int)std::lround(ex->width_pixels / s);
        d.height = (int)std::lround(ex->height_pixels / s);
        d.refreshMilliHz = (int)ex->refresh_millihertz;
        dp = &d;
    }
    if (!e->plat->setWindowMode(toMode(mode), monitor, dp))
        return fail(monitor ? ENG_ERROR_NOT_FOUND : ENG_ERROR_UNSUPPORTED,
                    std::string("the window could not enter ") + modeName(mode) +
                    (monitor ? " on monitor " + std::to_string(monitor) + " (is it still connected?)" : ""));
    return ENG_OK;
}

EngResult applyFlags(EngEngine* e, EngWindowFlags flags) {
    const EngWindowFlags known = ENG_WINDOW_FIXED_SIZE | ENG_WINDOW_NO_DECORATION | ENG_WINDOW_ALWAYS_ON_TOP |
                                 ENG_WINDOW_HIDDEN | ENG_WINDOW_NO_USER_MODE_CHANGE |
                                 ENG_WINDOW_HANDLE_CLOSE | ENG_WINDOW_HDR_IF_AVAILABLE;
    if (flags & ~known) return fail(ENG_ERROR_INVALID_ARGUMENT, "unknown window flag bits");
    if (((flags & ENG_WINDOW_HDR_IF_AVAILABLE) != 0) != e->windowHdr)
        return fail(ENG_ERROR_UNSUPPORTED, "ENG_WINDOW_HDR_IF_AVAILABLE is chosen when the window is created");
    if (!e->plat->setWindowFlags(flags))
        return fail(ENG_ERROR_UNSUPPORTED, "a window flag could not be applied on this platform "
                                           "(ENG_WINDOW_NO_USER_MODE_CHANGE needs the main thread)");
    e->windowFlags = flags;
    return ENG_OK;
}

} // namespace

extern "C" {

const char* engGetLastErrorMessage(void) { return t_lastError.c_str(); }
uint32_t engGetApiVersion(void) { return ENG_API_VERSION; }

// ── Engine ──────────────────────────────────────────────────────────────────

EngResult engCreateEngine(const EngEngineDesc* in, EngEngine** out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        *out = nullptr;
        EngEngineDesc d;
        if (!readIn(in, d)) return fail(ENG_ERROR_INVALID_ARGUMENT, "desc is NULL or has no struct_size");
        if (const char* why = versionMismatch(d.api_version)) return fail(ENG_ERROR_ABI_MISMATCH, why);
        if (d.reserved0) return fail(ENG_ERROR_INVALID_ARGUMENT, "reserved0 must be zero");
        if (d.pacing > ENG_PACING_HOST) return fail(ENG_ERROR_INVALID_ARGUMENT, "unknown pacing value");
        if (d.module_count)
            return fail(ENG_ERROR_UNSUPPORTED, "EngEngineDesc.modules is not implemented in this version "
                                               "(kits load from the project's manifest)");
        if (d.log_fn) return fail(ENG_ERROR_UNSUPPORTED, "EngEngineDesc.log_fn is not implemented in this version");

        auto e = std::make_unique<EngEngine>();
        if (d.app_name) e->appName = d.app_name;
        e->pacing = d.pacing;
        for (uint32_t i = 0; i < d.provider_count; ++i) {
            const EngProviderChoice& c = d.providers[i];
            const std::string kind = c.kind ? c.kind : "", name = c.name ? c.name : "";
            auto known = [&](std::initializer_list<const char*> names) {
                for (const char* n : names) if (name == n) return true;
                return false;
            };
            if (kind == "physics" && known({"jolt", "none"})) e->physics = name;
            else if (kind == "scripting" && known({"lua", "none"})) e->scripting = name;
            else if (kind == "audio" && known({"miniaudio", "none"})) e->audio = name;
            else if (kind == "physics" || kind == "scripting" || kind == "audio")
                return fail(ENG_ERROR_NOT_FOUND, "no " + kind + " provider named '" + name + "' in this build (" +
                    (kind == "physics" ? "jolt, none" : kind == "scripting" ? "lua, none" : "miniaudio, none") + ")");
            else
                return fail(ENG_ERROR_NOT_FOUND, "no provider kind '" + kind +
                                                 "' can be chosen in this build (physics, scripting, audio)");
        }
        if (d.project_path) {
            e->projectPath = d.project_path;
            // Refused here, by name, rather than at the first step. Existence
            // first: ProjectContext::load creates the project's folders, which
            // must never happen for a path that is not a project.
            if (!fs::exists(fs::path(d.project_path) / "project.json"))
                return fail(ENG_ERROR_NOT_FOUND, std::string("no project.json at ") + d.project_path);
            const ProjectContext p = ProjectContext::load(d.project_path);
            const std::string stem = fs::path(p.lastScene).stem().string();
            const fs::path cooked = fs::path(d.project_path) / ".cache" / "scenes" / (stem + ".cooked");
            if (!fs::exists(cooked))
                return fail(ENG_ERROR_NOT_FOUND, "cooked scene missing: " + cooked.string() +
                                                 " (package the project with engine_build, or open it in the editor once)");
        }
        e->platform = makeDefaultPlatform();
        e->plat = e->platform.get();
        *out = e.release();
        return ENG_OK;
    });
}

void engDestroyEngine(EngEngine* e) {
    guard<int>(0, [&] {
        if (!e) return 0;
        if (e->rt) {
            e->rt->stopSimulation();
            e->rt->shutdown();
        }
        delete e;
        return 0;
    });
}

EngBool engIsRunning(const EngEngine* e) {
    return guard<EngBool>(ENG_FALSE, [&]() -> EngBool {
        if (!e || e->quit) return ENG_FALSE;
        if (e->started && !e->headless && e->plat && e->plat->shouldClose()) return ENG_FALSE;
        return ENG_TRUE;
    });
}

void engRequestQuit(EngEngine* e) {
    guard<int>(0, [&] { if (e) e->quit = true; return 0; });
}

EngResult engPumpEvents(EngEngine* e) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine is NULL");
        if (e->started && e->plat) e->plat->pollEvents();
        gather(e);
        return ENG_OK;
    });
}

EngResult engWaitEvents(EngEngine* e, double timeout) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine is NULL");
        if (e->started && !e->headless && e->plat) {
            e->plat->waitEvents(timeout);
            e->plat->pollEvents();   // gathers what the wait delivered
        }
        gather(e);
        return ENG_OK;
    });
}

EngResult engStepWithDelta(EngEngine* e, double dt, EngStepInfo* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine is NULL");
        if (!(dt >= 0.0)) return fail(ENG_ERROR_INVALID_ARGUMENT, "delta_seconds must be zero or positive");
        if (e->quit) return fail(ENG_ERROR_REFUSED, "the engine has quit (engRequestQuit)");
        if (!e->started) {
            if (EngResult r = start(e, nullptr); r != ENG_OK) return r;
        }
        EngineRuntime& rt = *e->rt;
        const uint64_t before = rt.simulationTicks();
        // No pump and no wait here (DR-0030). A minimised window still ticks
        // the simulation; only drawing is skipped.
        rt.frameHousekeeping();
#if !defined(ENGINE_WINDOW_BACKEND_SDL3)
        if (!e->headless) InputSystem::get().processEvents();
#endif
        const bool present = rt.canPresent();
        if (present) rt.tick((float)dt);
        else         rt.tickNoRender((float)dt);
        rt.frameEnd(present);
        ++e->frame;
        if (out) {
            EngStepInfo info{};
            info.simulation_steps = (uint32_t)(rt.simulationTicks() - before);
            info.frame = e->frame;
            info.delta_seconds = dt;
            info.interpolation_alpha = 0.0f;   // not exposed by the runtime yet
            info.windows_presented = (!e->headless && present) ? 1u : 0u;
            writeOut(out, info);
        }
        return ENG_OK;
    });
}

EngResult engStep(EngEngine* e, EngStepInfo* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine is NULL");
        // The clock, as frameBegin's: the first frame is 0 so a long start does
        // not become one giant step, and a stall is clamped to 50 ms.
        const auto now = std::chrono::steady_clock::now();
        double dt = e->firstStep ? 0.0 : std::chrono::duration<double>(now - e->last).count();
        if (dt > 0.05) dt = 0.05;
        e->last = now;
        e->firstStep = false;
        return engStepWithDelta(e, dt, out);
    });
}

EngResult engRun(EngEngine* e, EngFrameFn frame_fn, void* user) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine is NULL");
        while (engIsRunning(e)) {
            if (EngResult r = engPumpEvents(e); r != ENG_OK) return r;
            // ENG_PACING_ENGINE: today's behaviour, block while minimised. The
            // frame limiter goes here (WO-061). Host pacing never blocks.
            if (e->pacing == ENG_PACING_ENGINE && e->started && !e->headless) {
                WindowState s;
                if (e->plat->windowState(s) && s.minimised) {
                    e->plat->waitEvents(0.1);
                    continue;
                }
            }
            if (frame_fn) frame_fn(user, e);
            if (!engIsRunning(e)) break;
            if (EngResult r = engStep(e, nullptr); r != ENG_OK) return r;
        }
        return ENG_OK;
    });
}

// ── Monitors ───────────────────────────────────────────────────────────────

void engGetWindowCapabilities(EngEngine* e, EngWindowCapabilities* out) {
    guard<int>(0, [&] {
        if (!e || !out) return 0;
        EngWindowCapabilities c{};
        const bool display = e->plat && e->plat->windowControl() && !e->plat->monitors().empty() &&
                             !(e->started && e->headless);
        c.max_windows = display ? 1u : 0u;   // this version: one window
        c.can_exclusive_fullscreen = display && e->plat->canExclusiveFullscreen();
        c.can_borderless_fullscreen = display;
        c.can_resize = display;
        c.can_position = display && e->plat->canPositionWindow();
        c.can_list_monitors = display;
        c.can_capture_cursor = ENG_FALSE;    // no cursor call in this version
        writeOut(out, c);
        return 0;
    });
}

EngResult engGetMonitors(EngEngine* e, EngMonitor* out, uint32_t* count) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e || !count) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine or count is NULL");
        const std::vector<MonitorDesc> ms = e->plat ? e->plat->monitors() : std::vector<MonitorDesc>{};
        if (!out) { *count = (uint32_t)ms.size(); return ENG_OK; }
        if (*count < ms.size()) { *count = (uint32_t)ms.size(); return fail(ENG_ERROR_BUFFER_TOO_SMALL, "monitor array too small"); }
        for (size_t i = 0; i < ms.size(); ++i) out[i] = ms[i].id;
        *count = (uint32_t)ms.size();
        return ENG_OK;
    });
}

EngResult engGetMonitorInfo(EngEngine* e, EngMonitor monitor, EngMonitorInfo* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e || !out) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine or out is NULL");
        for (const MonitorDesc& m : e->plat->monitors()) {
            if (m.id != monitor) continue;
            EngMonitorInfo i{};
            i.is_primary = m.primary ? ENG_TRUE : ENG_FALSE;
            i.x = m.x; i.y = m.y;
            i.width = (uint32_t)m.width; i.height = (uint32_t)m.height;
            i.content_scale = m.contentScale;
            i.hdr_headroom = 0.0f;   // unknown per monitor in this version
            i.current_mode.width_pixels  = (uint32_t)std::lround(m.currentMode.width * m.contentScale);
            i.current_mode.height_pixels = (uint32_t)std::lround(m.currentMode.height * m.contentScale);
            i.current_mode.refresh_millihertz = (uint32_t)m.currentMode.refreshMilliHz;
            std::snprintf(i.name, sizeof i.name, "%s", m.name.c_str());
            writeOut(out, i);
            return ENG_OK;
        }
        return fail(ENG_ERROR_NOT_FOUND, "no monitor " + std::to_string(monitor) + " (it may have been disconnected)");
    });
}

EngResult engGetMonitorModes(EngEngine* e, EngMonitor monitor, EngDisplayMode* out, uint32_t* count) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e || !count) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine or count is NULL");
        const std::vector<DisplayModeDesc> ms = e->plat->monitorModes(monitor);
        if (ms.empty()) return fail(ENG_ERROR_NOT_FOUND, "no monitor " + std::to_string(monitor) + ", or it lists no modes");
        if (!out) { *count = (uint32_t)ms.size(); return ENG_OK; }
        if (*count < ms.size()) { *count = (uint32_t)ms.size(); return fail(ENG_ERROR_BUFFER_TOO_SMALL, "mode array too small"); }
        const float s = monitorScale(e, monitor);
        for (size_t i = 0; i < ms.size(); ++i) {
            out[i] = EngDisplayMode{};
            out[i].width_pixels  = (uint32_t)std::lround(ms[i].width * s);
            out[i].height_pixels = (uint32_t)std::lround(ms[i].height * s);
            out[i].refresh_millihertz = (uint32_t)ms[i].refreshMilliHz;
        }
        *count = (uint32_t)ms.size();
        return ENG_OK;
    });
}

// ── The window ─────────────────────────────────────────────────────────────

EngResult engCreateWindow(EngEngine* e, const EngWindowDesc* in, EngWindow* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (!e || !out) return fail(ENG_ERROR_INVALID_ARGUMENT, "engine or out is NULL");
        *out = 0;
        EngWindowDesc d;
        if (!readIn(in, d)) return fail(ENG_ERROR_INVALID_ARGUMENT, "desc is NULL or has no struct_size");
        if (d.reserved) return fail(ENG_ERROR_INVALID_ARGUMENT, "reserved must be zero");
        if (e->windowAlive || (e->started && !e->headless))
            return fail(ENG_ERROR_UNSUPPORTED, "this version has one window, and it exists (max_windows = 1)");
        if (e->started)
            return fail(ENG_ERROR_UNSUPPORTED, "the window must be created before the first engStep in this version "
                                               "(the engine started headless)");
        if (d.present_mode != ENG_PRESENT_VSYNC)
            return fail(ENG_ERROR_UNSUPPORTED, "only ENG_PRESENT_VSYNC is implemented in this version");
        if (d.max_frames_per_second)
            return fail(ENG_ERROR_UNSUPPORTED, "max_frames_per_second is not implemented yet (WO-061)");
        if (!e->plat || !e->plat->windowControl())
            return fail(ENG_ERROR_UNSUPPORTED, "this build's window backend has no window control");
        if (e->plat->monitors().empty())
            return fail(ENG_ERROR_UNSUPPORTED, "no display is available (max_windows = 0)");
        if (d.use_position && !e->plat->canPositionWindow())
            return fail(ENG_ERROR_UNSUPPORTED, "this platform does not let a window choose its position");

        if (EngResult r = start(e, &d); r != ENG_OK) return r;
        e->windowAlive = true;
        ++e->windowGeneration;
        e->windowHdr = (d.flags & ENG_WINDOW_HDR_IF_AVAILABLE) != 0;
        // Everything the platform applies after creation. A refusal tears the
        // window down again, so a failed create leaves no half-made window.
        EngResult r = ENG_OK;
        if (d.min_width || d.min_height || d.max_width || d.max_height)
            if (!e->plat->setWindowSizeLimits((int)d.min_width, (int)d.min_height, (int)d.max_width, (int)d.max_height))
                r = fail(ENG_ERROR_UNSUPPORTED, "size limits could not be applied");
        if (r == ENG_OK && d.aspect_numerator && d.aspect_denominator)
            if (!e->plat->setWindowAspect((int)d.aspect_numerator, (int)d.aspect_denominator))
                r = fail(ENG_ERROR_UNSUPPORTED, "the aspect ratio could not be applied");
        if (r == ENG_OK && d.use_position && !e->plat->setWindowPosition(d.x, d.y))
            r = fail(ENG_ERROR_UNSUPPORTED, "the position could not be applied");
        if (r == ENG_OK) r = applyFlags(e, d.flags);
        if (r == ENG_OK && (d.mode != ENG_WINDOW_MODE_WINDOWED || d.monitor))
            r = applyMode(e, d.mode, d.monitor, &d.exclusive_mode);
        if (r != ENG_OK) {
            const std::string why = t_lastError;
            e->rt->stopSimulation();
            e->rt->shutdown();
            e->rt.reset();
            e->started = false;
            e->windowAlive = false;
            e->platform = makeDefaultPlatform();
            e->plat = e->platform.get();
            return fail(r, why);
        }
        *out = e->windowHandle();
        return ENG_OK;
    });
}

void engDestroyWindow(EngEngine* e, EngWindow w) {
    guard<int>(0, [&] {
        if (!e || !e->validWindow(w)) return 0;
        // This version has one window: closing it ends the engine's run.
        e->windowAlive = false;
        e->quit = true;
        if (e->plat) e->plat->showWindow(false);
        return 0;
    });
}

EngResult engSetPrimaryWindow(EngEngine* e, EngWindow w) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult { return checkWindow(e, w); });
}

EngResult engGetWindowInfo(EngEngine* e, EngWindow w, EngWindowInfo* out) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        if (!out) return fail(ENG_ERROR_INVALID_ARGUMENT, "out is NULL");
        WindowState s;
        if (!e->plat->windowState(s)) return fail(ENG_ERROR_UNSUPPORTED, "this platform cannot report the window's state");
        EngWindowInfo i{};
        i.mode = s.mode == WindowMode::BorderlessFullscreen ? ENG_WINDOW_MODE_BORDERLESS_FULLSCREEN
               : s.mode == WindowMode::ExclusiveFullscreen  ? ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN
                                                            : ENG_WINDOW_MODE_WINDOWED;
        i.monitor = s.monitor;
        i.x = s.x; i.y = s.y;
        i.width = (uint32_t)s.width; i.height = (uint32_t)s.height;
        i.width_pixels = (uint32_t)s.widthPixels; i.height_pixels = (uint32_t)s.heightPixels;
        i.content_scale = s.contentScale;
        i.hdr_headroom = e->plat->hdrHeadroom();
        i.is_focused = s.focused; i.is_minimised = s.minimised; i.is_visible = s.visible;
        writeOut(out, i);
        return ENG_OK;
    });
}

EngResult engSetWindowMode(EngEngine* e, EngWindow w, EngWindowMode mode, EngMonitor monitor,
                           const EngDisplayMode* ex) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        return applyMode(e, mode, monitor, ex);
    });
}

EngResult engSetWindowTitle(EngEngine* e, EngWindow w, const char* title) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        if (!title) return fail(ENG_ERROR_INVALID_ARGUMENT, "title is NULL");
        e->plat->setTitle(title);
        return ENG_OK;
    });
}

EngResult engSetWindowSize(EngEngine* e, EngWindow w, uint32_t width, uint32_t height) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        if (!width || !height) return fail(ENG_ERROR_INVALID_ARGUMENT, "width and height must be non-zero");
        if (!e->plat->setWindowSize((int)width, (int)height))
            return fail(ENG_ERROR_UNSUPPORTED, "the window could not be resized");
        return ENG_OK;
    });
}

EngResult engSetWindowPosition(EngEngine* e, EngWindow w, int32_t x, int32_t y) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        if (!e->plat->setWindowPosition(x, y))
            return fail(ENG_ERROR_UNSUPPORTED, "this platform does not let a window choose its position");
        return ENG_OK;
    });
}

EngResult engSetWindowSizeLimits(EngEngine* e, EngWindow w, uint32_t minW, uint32_t minH, uint32_t maxW,
                                 uint32_t maxH) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        if ((maxW && minW > maxW) || (maxH && minH > maxH))
            return fail(ENG_ERROR_INVALID_ARGUMENT, "a minimum is larger than its maximum");
        if (!e->plat->setWindowSizeLimits((int)minW, (int)minH, (int)maxW, (int)maxH))
            return fail(ENG_ERROR_UNSUPPORTED, "size limits could not be applied");
        return ENG_OK;
    });
}

EngResult engSetWindowAspect(EngEngine* e, EngWindow w, uint32_t numerator, uint32_t denominator) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        if ((numerator == 0) != (denominator == 0))
            return fail(ENG_ERROR_INVALID_ARGUMENT, "aspect: both zero (free) or both non-zero");
        if (!e->plat->setWindowAspect((int)numerator, (int)denominator))
            return fail(ENG_ERROR_UNSUPPORTED, "the aspect ratio could not be applied");
        return ENG_OK;
    });
}

EngResult engSetWindowFlags(EngEngine* e, EngWindow w, EngWindowFlags flags) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        const bool hiddenChanged = ((flags ^ e->windowFlags) & ENG_WINDOW_HIDDEN) != 0;
        if (EngResult r = applyFlags(e, flags); r != ENG_OK) return r;
        if (hiddenChanged) e->plat->showWindow((flags & ENG_WINDOW_HIDDEN) == 0);
        return ENG_OK;
    });
}

EngResult engShowWindow(EngEngine* e, EngWindow w, EngBool visible) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        if (!e->plat->showWindow(visible != 0)) return fail(ENG_ERROR_UNSUPPORTED, "the window could not be shown or hidden");
        if (visible) e->windowFlags &= ~(EngWindowFlags)ENG_WINDOW_HIDDEN;
        else         e->windowFlags |= ENG_WINDOW_HIDDEN;
        return ENG_OK;
    });
}

EngResult engFocusWindow(EngEngine* e, EngWindow w) {
    return guard<EngResult>(ENG_ERROR_INTERNAL, [&]() -> EngResult {
        if (EngResult r = checkWindow(e, w); r != ENG_OK) return r;
        if (!e->plat->focusWindow()) return fail(ENG_ERROR_UNSUPPORTED, "the window could not be focused");
        return ENG_OK;
    });
}

EngBool engNextEvent(EngEngine* e, EngEvent* out) {
    return guard<EngBool>(ENG_FALSE, [&]() -> EngBool {
        if (!e || !out) return ENG_FALSE;
        if (e->dropped) {   // the oldest news first: something was lost
            EngEvent ev = blankEvent(e, ENG_EVENT_OVERFLOWED, 0);
            ev.a = (int32_t)e->dropped;
            e->dropped = 0;
            writeOut(out, ev);
            return ENG_TRUE;
        }
        if (e->events.empty()) return ENG_FALSE;
        writeOut(out, e->events.front());
        e->events.pop_front();
        return ENG_TRUE;
    });
}

} // extern "C"

#include "capi/eng_internal.h"

EngineRuntime* engInternalRuntime(EngEngine* e) {
    if (!e) return nullptr;
    if (!e->started && start(e, nullptr) != ENG_OK) return nullptr;
    return e->rt.get();
}
