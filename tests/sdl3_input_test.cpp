// ── sdl3_input_test — the SDL3 event plumbing, deterministically ─────────────
// The failure mode this exists to catch: the editor renders perfectly and
// receives NO input. On SDL3 there is one process-wide event queue owned by the
// platform pump, so every consumer (ImGui, the window input source) has to be
// fed by IPlatform's native-event hook. If that wiring breaks, nothing crashes
// and nothing logs — the UI just stops responding, which is invisible to a boot
// smoke test and awkward to notice in a screenshot.
//
// So this drives the path with SYNTHESIZED SDL events (SDL_PushEvent) instead of
// a human: hook fires, scroll accumulates, text decodes. Only registered on
// sdl3 builds.
#include "runtime/platform/platform.h"
#include "runtime/input/input_system.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace { int g_failures = 0; }

#define CHECK(cond, ...) do {                                    \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);          \
                   std::printf("\n"); ++g_failures; }            \
    else         { std::printf("  ok    " __VA_ARGS__);          \
                   std::printf("\n"); }                         \
} while (0)

int main() {
    std::printf("=== sdl3_input_test ===\n");

    auto platform = makeDefaultPlatform();
    CHECK(std::strcmp(windowBackendName(), "sdl3") == 0,
          "built against the sdl3 backend (%s)", windowBackendName());

    PlatformConfig cfg;
    cfg.title = "sdl3_input_test";
    cfg.width = 320; cfg.height = 240;
    if (!platform->init(cfg)) {
        // No display (a bare CI container) is not a failure — but say so.
        std::printf("SKIP: platform init failed (headless?)\n");
        return 0;
    }

    // ── The hook fires for pumped events ───────────────────────────────────
    int seen = 0;
    platform->setNativeEventHook([&](const void* e) {
        if (e) ++seen;
    });

    SDL_Event ping{};
    ping.type = SDL_EVENT_USER;
    SDL_PushEvent(&ping);
    platform->pollEvents();
    CHECK(seen > 0, "native events reach the hook (%d seen)", seen);

    // ── Scroll: no polling API in SDL, so it MUST come from events ──────────
    InputSystem& in = InputSystem::get();
    in.init(platform->backendWindowHandle());

    SDL_Event wheel{};
    wheel.type    = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.x = 0.0f;
    wheel.wheel.y = 3.0f;
    in.processNativeEvent(&wheel);
    in.processEvents();                 // flushes the accumulator
    CHECK(in.scrollDeltaY() == 3.0f, "mouse wheel -> scrollDeltaY (%g)",
          (double)in.scrollDeltaY());

    // Accumulates within a frame, then clears — two notches in one frame must
    // not be reported as one, and must not leak into the next frame.
    SDL_Event w1 = wheel, w2 = wheel;
    w1.wheel.y = 1.0f; w2.wheel.y = 2.0f;
    in.processNativeEvent(&w1);
    in.processNativeEvent(&w2);
    in.processEvents();
    CHECK(in.scrollDeltaY() == 3.0f, "two wheel events in one frame sum (%g)",
          (double)in.scrollDeltaY());
    in.processEvents();
    CHECK(in.scrollDeltaY() == 0.0f, "scroll clears the next frame (%g)",
          (double)in.scrollDeltaY());

    // ── Text input: SDL gives UTF-8, the engine's stream is codepoints ──────
    // The decoder is hand-written, so exercise 1-, 2- and 3-byte sequences and
    // a malformed one. Getting this wrong mangles every non-ASCII keystroke.
    {
        SDL_Event te{};
        te.type = SDL_EVENT_TEXT_INPUT;
        const char* utf8 = "aé€";           // U+0061, U+00E9, U+20AC
        te.text.text = utf8;
        in.processNativeEvent(&te);
        in.processEvents();
        const std::vector<uint32_t>& cps = in.textInput();
        CHECK(cps.size() == 3, "3 codepoints from \"aé€\" (%zu)", cps.size());
        if (cps.size() == 3) {
            CHECK(cps[0] == 0x0061, "1-byte  -> U+0061 (got U+%04X)", cps[0]);
            CHECK(cps[1] == 0x00E9, "2-byte  -> U+00E9 (got U+%04X)", cps[1]);
            CHECK(cps[2] == 0x20AC, "3-byte  -> U+20AC (got U+%04X)", cps[2]);
        }
    }
    {
        // A truncated multi-byte sequence must be DROPPED, not emitted as a
        // bogus codepoint — a text field should ignore garbage, not insert it.
        SDL_Event te{};
        te.type = SDL_EVENT_TEXT_INPUT;
        const char bad[] = { (char)0xE2, (char)0x82, 'Z', '\0' };  // truncated + 'Z'
        te.text.text = bad;
        in.processNativeEvent(&te);
        in.processEvents();
        const std::vector<uint32_t>& cps = in.textInput();
        bool sane = true;
        for (uint32_t c : cps) if (c > 0x10FFFF || c == 0) sane = false;
        CHECK(sane, "malformed UTF-8 produces no out-of-range codepoints");
    }

    // ── Focus query works through the SDL path ─────────────────────────────
    // (Value depends on the WM, so assert only that it does not crash/assert.)
    (void)in.windowFocused();
    CHECK(true, "windowFocused() is callable on the SDL path");

    platform->shutdown();

    if (g_failures) {
        std::printf("sdl3_input_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("sdl3_input_test: PASS\n");
    return 0;
}
