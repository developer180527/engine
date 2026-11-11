/* eng_shared_test — the shared C API library, from a plain C program (WO-070).
 *
 * Links libeng (not the static engine_capi) the way a Python, Swift or C host
 * would: create a headless engine, step it, destroy it. The engine starts and
 * joins threads on the way, which is what aborted when global new inside the
 * library used the engine's heap and libc++ outside freed with the system's. Passing means the process exits 0. */
#include <eng/engine.h>

#include <stdio.h>

int main(void) {
    EngEngineDesc d = ENG_INIT(EngEngineDesc);
    EngEngine* e = NULL;
    EngStepInfo info = ENG_INIT(EngStepInfo);
    int i;
    d.api_version = ENG_API_VERSION;
    d.app_name = "eng_shared_test";
    if (engCreateEngine(&d, &e) != ENG_OK) {
        printf("FAIL  engCreateEngine: %s\n", engGetLastErrorMessage());
        return 1;
    }
    for (i = 0; i < 30; ++i) {
        if (engStepWithDelta(e, 1.0 / 60.0, &info) != ENG_OK) {
            printf("FAIL  step %d: %s\n", i, engGetLastErrorMessage());
            return 1;
        }
    }
    if (info.frame != 30) { printf("FAIL  frame is %llu, want 30\n", (unsigned long long)info.frame); return 1; }
    engDestroyEngine(e);
    printf("ok    libeng: 30 headless steps and a clean shutdown from C\n");
    return 0;
}
