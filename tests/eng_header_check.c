/* eng_header_check: the C API's headers compile as strict C99, alone and
 * together, and (with -Wpadded on GCC and Clang) no struct has implicit
 * padding, which every language binding would otherwise have to guess
 * (engine-api.md §3 rule 8). Compiled only; never run. WO-070.
 *
 * Every struct is named below: Clang lays out (and so reports padding in) only
 * the structs a translation unit uses. the eng_exports test
 * (scripts/check_eng_exports.py) fails if a header gains a struct this list lacks. */
#include <eng/eng_base.h>
#include <eng/eng_engine.h>
#include <eng/eng_window.h>
#include <eng/engine.h>

int eng_header_check_anchor(void);
int eng_header_check_anchor(void) {
    return (int)(0
        + sizeof(EngVec3)
        + sizeof(EngQuat)
        + sizeof(EngTransform)
        + sizeof(EngProviderChoice)
        + sizeof(EngEngineDesc)
        + sizeof(EngStepInfo)
        + sizeof(EngWindowCapabilities)
        + sizeof(EngDisplayMode)
        + sizeof(EngMonitorInfo)
        + sizeof(EngWindowDesc)
        + sizeof(EngWindowInfo)
        + sizeof(EngEvent));
}
