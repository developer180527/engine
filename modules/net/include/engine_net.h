#pragma once
// ── engine_net — the C ABI of the engine's Rust networking module ────────────
//
// THIS HEADER IS THE CONTRACT. It is hand-written, not generated: the boundary
// between C++ and Rust is the one place where a mistake is undefined behaviour
// rather than a compile error, so it is written and reviewed deliberately (and
// it keeps cbindgen out of the build).
//
// WHY RUST LIVES HERE AND NOWHERE ELSE (yet):
// Networking is the textbook case — it parses untrusted bytes off the wire,
// carries a complex connection state machine, and is brand-new code with no
// rewrite cost. That is precisely where memory safety pays. It is NOT a
// blanket migration: ozz-animation and Jolt keep the hot numeric loops, since
// those touch no untrusted input and rewriting them would buy ~nothing.
//
// ── Rules of this boundary ──────────────────────────────────────────────────
// 1. NO ALLOCATION CROSSES. The caller owns every buffer; Rust writes into it.
//    Neither side ever frees the other's memory, so the two allocators
//    (engine TLSF vs Rust's) can never meet.
// 2. NO PANIC CROSSES. The crate is built `panic = "abort"`; a Rust panic
//    unwinding into C++ is UB, so it aborts at the bug instead.
// 3. EVERY CALL RETURNS A CODE. Negative = failure, 0 = success. Nothing
//    throws, nothing longjmps, no errno.
// 4. VERSIONED INIT. engine_net_init() rejects a header/library mismatch the
//    same way the kit ABI does — a stale static lib is caught at boot.
// 5. Pointers may be null and lengths may be zero; every entry point
//    validates rather than trusting the caller.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bump when anything in this header changes shape. engine_net_init() compares
// the caller's compiled-in value against the library's own.
#define ENGINE_NET_ABI_VERSION 1u

// Return codes. 0 is success; everything else is negative so `if (rc)` reads
// as failure and `rc < 0` is unambiguous.
typedef enum EngineNetStatus {
    ENGINE_NET_OK             =  0,
    ENGINE_NET_ERR_ABI        = -1,  // header/library version mismatch
    ENGINE_NET_ERR_NULL_ARG   = -2,  // required pointer was null
    ENGINE_NET_ERR_NOT_INIT   = -3,  // call before engine_net_init()
    ENGINE_NET_ERR_ALREADY    = -4,  // engine_net_init() called twice
} EngineNetStatus;

// ABI version this library was BUILT with (vs the ENGINE_NET_ABI_VERSION the
// caller was compiled with). Safe to call before init.
uint32_t engine_net_abi_version(void);

// Human-readable form of a status code. Returns a static string, never null,
// never allocates — so it is safe to log from an error path.
const char* engine_net_status_str(int32_t status);

// Initialise the module. Pass ENGINE_NET_ABI_VERSION.
// Returns ENGINE_NET_ERR_ABI if the linked library disagrees with this header.
int32_t engine_net_init(uint32_t abi_version);

// Idempotent; safe to call without a successful init.
void engine_net_shutdown(void);

// ── Log sink ────────────────────────────────────────────────────────────────
// Rust calls OUT through this. It exists in the skeleton because the eventual
// packet-received path is the same shape (Rust → C callback with borrowed
// data), and that direction is the one most likely to get lifetimes wrong.
//
// `msg` is valid ONLY for the duration of the call — copy it if you keep it.
// The callback must not unwind (a C++ exception escaping into Rust is UB).
typedef void (*EngineNetLogFn)(void* user, const char* msg);

// Pass cb = NULL to detach. `user` is passed back untouched.
void engine_net_set_log(EngineNetLogFn cb, void* user);

// Emit a message through the installed sink. Round-trips C → Rust → C, which
// is what makes the callback path testable before there is any traffic.
int32_t engine_net_log_test(const char* msg);

// ── Buffer round-trip ───────────────────────────────────────────────────────
// FNV-1a over `len` bytes at `data`, written to *out. This is not networking:
// it is the smallest honest exercise of rule 1 — the caller's bytes go in, a
// value comes back, and nothing is allocated or freed across the boundary.
// The real transport moves packets through exactly this shape.
int32_t engine_net_checksum(const uint8_t* data, size_t len, uint64_t* out);

#ifdef __cplusplus
}  // extern "C"
#endif
