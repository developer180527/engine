#pragma once
// ── Setting an environment variable, portably, for tests ─────────────────────
//
// Four test files drive the engine through env vars — COOK_TEX_TARGET,
// COOK_TEX_HQ, ENGINE_COOK_DETERMINISM_CHECK, ENGINE_AUDIO_NO_HARDWARE — and all
// four called `::setenv` / `::unsetenv`. Those are POSIX. MSVC has neither:
// "error C3861: 'setenv': identifier not found", in every one of them.
//
// Windows spells it `_putenv_s`, and UNSETTING is where the two genuinely differ
// rather than merely renaming: POSIX has `unsetenv(name)`, while `_putenv_s(name,
// "")` REMOVES the variable on Windows (it does not set it to empty). Code that
// reads `getenv(x) && *x` cannot tell those apart, but code doing `getenv(x) !=
// nullptr` very much can — which is exactly what `harness::skips_are_failures`
// and `resolveTexTarget` do. Getting that wrong would make an env-var test pass
// for the wrong reason on one platform, which is worse than not compiling.
//
// One helper rather than an ifdef in each test: what a test wants is "make the
// engine see this setting", which is one capability with two spellings.
#include <cstdlib>

namespace testenv {

// Set `name` to `value`, overwriting any existing definition.
inline void set(const char* name, const char* value) {
#if defined(_WIN32)
    (void)::_putenv_s(name, value);
#else
    (void)::setenv(name, value, 1);
#endif
}

// Remove `name` entirely, so `std::getenv(name)` returns nullptr afterwards.
inline void unset(const char* name) {
#if defined(_WIN32)
    // Empty value DELETES the variable on Windows — it does not define it as "".
    (void)::_putenv_s(name, "");
#else
    (void)::unsetenv(name);
#endif
}

} // namespace testenv
