#pragma once
// ── stdoutLive — logs reach a pipe or file as they are written ──────────────
//
// `setvbuf(stdout, nullptr, _IOLBF, 0)` is fine on POSIX and an invalid
// parameter on Windows: MSVC's CRT wants a size of 2 or more for _IOLBF, and a
// Debug build answers 0 with an assertion dialog. Under ctest nobody closes it,
// so every tool that made this call hung before its first line (WO-038).
// MSVC has no line buffering anyway (_IOLBF is full buffering there), so
// Windows gets none: the logs stay live, which was the point.
#include <cstdio>

inline void stdoutLive() {
#if defined(_WIN32)
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#else
    std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
#endif
}
