/* eng_window_test: the main window through the C API, from C (WO-070).
 *
 * Skips (exit 77) where there is no display. The window is created HIDDEN, so
 * running the suite does not flash a window on a developer's screen.
 *   1. capabilities and monitors are readable before any window exists
 *   2. a window with limits, aspect and flags is created; a second is refused
 *   3. resize and move are reported as events after a pump
 *   4. size limits are enforced by the platform
 *   5. steps present (or skip presenting) without pumping
 *   6. the handle goes stale when the window is destroyed, and the run ends */
#include <eng/engine.h>

#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL  " __VA_ARGS__); printf("\n"); ++failures; } \
                           else { printf("  ok    " __VA_ARGS__); printf("\n"); } } while (0)

/* Pump until an event of `type` arrives, or give up after a few frames. */
static int waitFor(EngEngine* e, EngEventType type, EngEvent* got) {
    for (int i = 0; i < 60; ++i) {
        engWaitEvents(e, 0.02);
        EngEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.struct_size = sizeof ev;
        while (engNextEvent(e, &ev)) {
            if (ev.type == type) { *got = ev; return 1; }
            ev.struct_size = sizeof ev;
        }
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("eng_window_test\n");

    EngEngineDesc d = ENG_INIT(EngEngineDesc);
    d.api_version = ENG_API_VERSION;
    d.app_name = "eng_window_test";
    EngEngine* e = NULL;
    if (engCreateEngine(&d, &e) != ENG_OK) { printf("FAIL engCreateEngine: %s\n", engGetLastErrorMessage()); return 1; }

    /* 1. Before a window */
    EngWindowCapabilities caps = ENG_INIT(EngWindowCapabilities);
    engGetWindowCapabilities(e, &caps);
    if (caps.max_windows == 0) {
        printf("SKIP no display: this platform reports max_windows = 0\n");
        engDestroyEngine(e);
        return 77;
    }
    CHECK(caps.max_windows == 1, "this version reports one window (%u)", caps.max_windows);
    CHECK(caps.can_borderless_fullscreen && caps.can_resize && caps.can_list_monitors,
          "borderless fullscreen, resize and monitor listing are available");

    uint32_t n = 0;
    CHECK(engGetMonitors(e, NULL, &n) == ENG_OK && n >= 1, "monitors listed before a window (%u)", n);
    EngMonitor mons[16];
    uint32_t cap = n < 16 ? n : 16;
    EngResult r = engGetMonitors(e, mons, &cap);
    CHECK(r == ENG_OK || r == ENG_ERROR_BUFFER_TOO_SMALL, "monitor ids read");
    int primaries = 0;
    for (uint32_t i = 0; i < cap; ++i) {
        EngMonitorInfo mi;
        memset(&mi, 0, sizeof mi);
        mi.struct_size = sizeof mi;
        CHECK(engGetMonitorInfo(e, mons[i], &mi) == ENG_OK && mi.width > 0 && mi.current_mode.refresh_millihertz > 0,
              "monitor %u: '%s' %ux%u at %.2f Hz", mons[i], mi.name, mi.width, mi.height,
              mi.current_mode.refresh_millihertz / 1000.0);
        primaries += mi.is_primary ? 1 : 0;
        uint32_t modes = 0;
        CHECK(engGetMonitorModes(e, mons[i], NULL, &modes) == ENG_OK && modes >= 1, "  and %u display modes", modes);
    }
    CHECK(primaries == 1, "exactly one primary monitor");
    {
        EngMonitorInfo mi = ENG_INIT(EngMonitorInfo);
        CHECK(engGetMonitorInfo(e, 9999, &mi) == ENG_ERROR_NOT_FOUND, "an unknown monitor is refused by name: %s",
              engGetLastErrorMessage());
    }

    /* 2. The window */
    EngWindowDesc wd = ENG_INIT(EngWindowDesc);
    wd.title = "eng_window_test";
    wd.flags = ENG_WINDOW_HIDDEN | ENG_WINDOW_HANDLE_CLOSE | ENG_WINDOW_NO_USER_MODE_CHANGE;
    wd.width = 640; wd.height = 400;
    wd.min_width = 320; wd.min_height = 200;
    wd.max_width = 1000; wd.max_height = 800;
    EngWindow w = 0;
    r = engCreateWindow(e, &wd, &w);
    CHECK(r == ENG_OK && w != 0, "a hidden window with limits and flags (%s)", r ? engGetLastErrorMessage() : "ok");
    if (r != ENG_OK) { engDestroyEngine(e); return 1; }
    EngWindow w2 = 0;
    CHECK(engCreateWindow(e, &wd, &w2) == ENG_ERROR_UNSUPPORTED && w2 == 0, "a second window is refused by name: %s",
          engGetLastErrorMessage());
    CHECK(engSetPrimaryWindow(e, w) == ENG_OK, "the window is the primary one");

    EngWindowInfo info = ENG_INIT(EngWindowInfo);
    CHECK(engGetWindowInfo(e, w, &info) == ENG_OK && info.width == 640 && info.height == 400 && !info.is_visible &&
          info.mode == ENG_WINDOW_MODE_WINDOWED && info.monitor != 0,
          "info: %ux%u (%ux%u pixels), hidden, windowed, on monitor %u", info.width, info.height,
          info.width_pixels, info.height_pixels, info.monitor);

    /* 3. Changes are reported as events after a pump */
    engPumpEvents(e);
    { EngEvent ev = ENG_INIT(EngEvent); while (engNextEvent(e, &ev)) ev.struct_size = sizeof ev; }  /* drain */
    CHECK(engSetWindowSize(e, w, 700, 450) == ENG_OK, "engSetWindowSize");
    EngEvent got = ENG_INIT(EngEvent);
    CHECK(waitFor(e, ENG_EVENT_WINDOW_RESIZED, &got) && got.width == 700 && got.height == 450 && got.window == w,
          "WINDOW_RESIZED reports 700x450 (got %ux%u, %d x %d pixels)", got.width, got.height, got.a, got.b);
    if (caps.can_position) {
        CHECK(engSetWindowPosition(e, w, 120, 140) == ENG_OK, "engSetWindowPosition");
        CHECK(waitFor(e, ENG_EVENT_WINDOW_MOVED, &got), "WINDOW_MOVED reported (%d, %d)", got.x, got.y);
    }

    /* 4. Limits hold: a size past the maximum is clamped by the platform */
    CHECK(engSetWindowSize(e, w, 2000, 2000) == ENG_OK, "a size past the limits is requested");
    for (int i = 0; i < 10; ++i) engWaitEvents(e, 0.02);
    CHECK(engGetWindowInfo(e, w, &info) == ENG_OK && info.width <= 1000 && info.height <= 800,
          "the window stays within its limits (%ux%u)", info.width, info.height);
    CHECK(engSetWindowSizeLimits(e, w, 900, 0, 800, 0) == ENG_ERROR_INVALID_ARGUMENT, "min > max is refused: %s",
          engGetLastErrorMessage());
    CHECK(engSetWindowAspect(e, w, 16, 0) == ENG_ERROR_INVALID_ARGUMENT, "a half-set aspect is refused");
    CHECK(engSetWindowFlags(e, w, ENG_WINDOW_HIDDEN | ENG_WINDOW_HDR_IF_AVAILABLE) == ENG_ERROR_UNSUPPORTED,
          "HDR cannot be switched on after creation: %s", engGetLastErrorMessage());
    CHECK(engSetWindowTitle(e, w, "renamed") == ENG_OK, "engSetWindowTitle");

    /* 5. Steps: no pump inside, and a frame counted */
    int steps = 0;
    for (int i = 0; i < 5; ++i) {
        EngStepInfo si = ENG_INIT(EngStepInfo);
        if (engStepWithDelta(e, 1.0 / 60.0, &si) == ENG_OK && si.frame == (uint64_t)(i + 1)) ++steps;
    }
    CHECK(steps == 5, "five steps with a window");
    CHECK(engIsRunning(e), "still running");

    /* 6. Destroy: the handle goes stale and the run ends */
    engDestroyWindow(e, w);
    CHECK(engSetWindowSize(e, w, 640, 400) == ENG_ERROR_STALE_HANDLE, "the destroyed window's handle is stale");
    CHECK(!engIsRunning(e), "destroying the only window ends the run");

    engDestroyEngine(e);
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
