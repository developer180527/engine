/* eng_window_manual: what eng_window_test cannot check without a person
 * (WO-070). Run it, watch, then close the window twice.
 *
 *   build/eng_window_manual
 *
 *   - the window goes borderless fullscreen, then back to windowed, then
 *     exclusive fullscreen (if offered), then windowed, 2 s apart; each change
 *     prints the WINDOW_MODE_CHANGED event the platform reported;
 *   - the macOS green button does nothing (ENG_WINDOW_NO_USER_MODE_CHANGE);
 *   - closing the window the FIRST time is declined (ENG_WINDOW_HANDLE_CLOSE):
 *     it prints "close requested: declined" and stays open; the second close
 *     is accepted and the program exits 0. */
#include <eng/engine.h>

#include <stdio.h>
#include <string.h>

static const char* modeName(int m) {
    return m == ENG_WINDOW_MODE_BORDERLESS_FULLSCREEN ? "borderless fullscreen"
         : m == ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN  ? "exclusive fullscreen" : "windowed";
}

int main(void) {
    EngEngineDesc d = ENG_INIT(EngEngineDesc);
    d.api_version = ENG_API_VERSION;
    d.app_name = "eng_window_manual";
    EngEngine* e = NULL;
    if (engCreateEngine(&d, &e) != ENG_OK) { printf("%s\n", engGetLastErrorMessage()); return 1; }
    EngWindowCapabilities caps = ENG_INIT(EngWindowCapabilities);
    engGetWindowCapabilities(e, &caps);
    if (!caps.max_windows) { printf("no display\n"); return 77; }

    EngWindowDesc wd = ENG_INIT(EngWindowDesc);
    wd.title = "eng_window_manual: close me twice";
    wd.flags = ENG_WINDOW_HANDLE_CLOSE | ENG_WINDOW_NO_USER_MODE_CHANGE;
    wd.width = 800; wd.height = 500;
    EngWindow w = 0;
    if (engCreateWindow(e, &wd, &w) != ENG_OK) { printf("%s\n", engGetLastErrorMessage()); return 1; }

    const int plan[] = { ENG_WINDOW_MODE_BORDERLESS_FULLSCREEN, ENG_WINDOW_MODE_WINDOWED,
                         ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN, ENG_WINDOW_MODE_WINDOWED };
    int step = 0, closes = 0;
    long frame = 0;
    printf("the green button should do nothing; close the window twice to finish\n");
    while (engIsRunning(e)) {
        engPumpEvents(e);
        EngEvent ev = ENG_INIT(EngEvent);
        while (engNextEvent(e, &ev)) {
            if (ev.type == ENG_EVENT_WINDOW_MODE_CHANGED) printf("  mode is now %s\n", modeName(ev.a));
            if (ev.type == ENG_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (++closes == 1) printf("  close requested: declined (close again to quit)\n");
                else { printf("  close requested: accepted\n"); engDestroyWindow(e, w); }
            }
            ev.struct_size = sizeof ev;
        }
        if (!engIsRunning(e)) break;
        if (step < 4 && frame > 0 && frame % 120 == 0) {   /* about 2 s at 60 Hz */
            int m = plan[step++];
            if (m == ENG_WINDOW_MODE_EXCLUSIVE_FULLSCREEN && !caps.can_exclusive_fullscreen) continue;
            printf("asking for %s\n", modeName(m));
            if (engSetWindowMode(e, w, (EngWindowMode)m, 0, NULL) != ENG_OK)
                printf("  refused: %s\n", engGetLastErrorMessage());
        }
        engStep(e, NULL);
        ++frame;
    }
    engDestroyEngine(e);
    printf("done\n");
    return 0;
}
