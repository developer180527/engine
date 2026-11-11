/* eng_shared_test — the shared C API library, from a plain C program (WO-070).
 *
 * Links libeng (not the static engine_capi) the way a Python, Swift or C host
 * would: create a headless engine, step it, destroy it. The engine starts and
 * joins threads on the way. That aborted ("pointer being freed was not
 * allocated"): std::thread's bookkeeping came from the engine's heap and the
 * system's C++ library freed it. The engine's heap is still what global new
 * uses in here; only thread creation asks the system allocator
 * (engine::threads::start, mem::SystemNewScope). Passing means exit 0. */
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
