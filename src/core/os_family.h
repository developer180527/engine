#pragma once
// ── os_family — which OSes are POSIX-family, said ONCE (WO-006) ───────────────
//
// Core code used to write `#if defined(_WIN32) … #else <POSIX> #endif`, which
// means "every OS that is not Windows gets mmap, pthread and dlopen". A new
// port therefore COMPILED and then did the wrong thing at run time, with no
// list of what it had to implement.
//
// Now each such site reads:
//
//     #if defined(_WIN32)
//         …
//     #elif ENGINE_OS_POSIX
//         …
//     #else
//     #  error "port: <what this OS must provide here>"
//     #endif
//
// so an unlisted OS fails to build, and every `#error "port: …"` is one line of
// its to-do list (collected into docs/process/porting.md by
// `scripts/engine_audit.py --write-porting`). Adding an OS that really is
// POSIX-family (FreeBSD, say) is one line HERE, not an edit at every site.
//
// A `#else` that genuinely suits ANY OS — a fallback, a default, "not
// supported here" — says so on the same line: `#else  // any OS: <why>`.
// Audit rule OS-01 fails on a bare `#else` after an OS check in src/core and
// src/runtime that does neither.
#if defined(__APPLE__) || defined(__linux__)
#  define ENGINE_OS_POSIX 1
#else  // any OS: not POSIX-family; each site's #error "port: …" is then its to-do
#  define ENGINE_OS_POSIX 0
#endif
