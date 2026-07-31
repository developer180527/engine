---
status: as-built
tier: prototype
verified: 2026-07-31
covers:
  - modules/net/
tests:
  - tests/rust_ffi_test.cpp
# `prototype` is capped by absent CAPABILITY, not absent tests: the FFI seam is
# fully covered (18 C++ assertions + 4 cargo tests), but this module does not
# do any networking yet. Raising the tier needs a transport, not more tests.
---
# net — networking core (Rust behind a C ABI)

## Purpose
The engine's networking module. Rust core, C ABI, linked as a **staticlib**
into the C++ build. Today it is the *toolchain seam only* — the transport
itself is an open design decision (see "Open decisions" below).

## Why this module is Rust
Networking parses untrusted bytes off the wire, carries a stateful connection
machine, and is brand-new code with no rewrite cost. That combination is where
memory safety actually pays: a malformed packet must not become a heap
overflow.

This is deliberately **not** a blanket migration. ozz-animation and Jolt keep
the hot numeric loops — they touch no untrusted input, so a rewrite would buy
approximately nothing while costing two mature libraries. The rule:

> Rust where untrusted data or complex ownership lives; C++ where mature
> numeric libraries already live.

## Why a staticlib, not a kit
Kits (`include/engine/game_module.h`) are hot-reloadable `.so` modules for
*gameplay*. Infrastructure should not be: dlopen adds ABI-drift risk between a
running host and a rebuilt module, and a second allocator in the process. A
staticlib links once, has no load order, and cannot drift.

(Rust **kits** are separately viable — `EngineGameModuleV1` is a plain C struct
of function pointers, so a Rust `cdylib` can implement it with no engine
changes. That is a different, gameplay-side option.)

## The boundary contract
`include/engine_net.h` is hand-written and is the source of truth. Five rules,
each enforced in `src/lib.rs`:

1. **No allocation crosses.** Callers own buffers; Rust writes into them. The
   engine's TLSF allocator and Rust's allocator never meet.
2. **No panic crosses.** Built `panic = "abort"` in both profiles — unwinding
   across `extern "C"` into C++ is undefined behaviour, so a panic aborts at
   the bug instead of corrupting the host.
3. **Every call returns a status code.** Negative = failure. Nothing throws.
4. **Versioned init.** `engine_net_init()` rejects a header/library mismatch,
   the same discipline the kit ABI uses — a stale staticlib (C++ rebuilt, Rust
   not) is caught at boot, not as a subtly wrong layout later.
5. **Pointers and lengths are validated, never trusted.** A null pointer with a
   non-zero length is rejected: `slice::from_raw_parts` on null is UB even for
   length 0.

## Build integration
`CMakeLists.txt` runs cargo via `add_custom_command` and imports the result as
a static library, so the rest of the tree sees an ordinary CMake target.

- `ENGINE_WITH_RUST` (default ON) gates it; the module also **skips itself when
  cargo is absent**, so the engine still configures and builds without Rust.
  Verified: `-DENGINE_WITH_RUST=OFF` configures clean and `rust_ffi_test`
  disappears from the test list.
- `CARGO_TARGET_DIR` is redirected into the CMake build dir; `cargo test` run
  by hand still uses `modules/net/target/` (gitignored).
- Rust staticlibs do **not** carry their std dependencies — the C++ link step
  supplies them (CoreFoundation/Security on macOS, pthread/dl/m on Linux,
  ws2_32/userenv/ntdll/bcrypt/advapi32 on Windows). A missing one appears as
  undefined symbols that never mention Rust, so they are listed explicitly.

## Testing
Two layers, deliberately overlapping:
- `cargo test` — Rust-side unit tests (pinned FNV vectors, null/length
  rejection, ABI mismatch).
- `tests/rust_ffi_test.cpp` — the **seam**: both call directions, borrowed
  buffers, binary payloads with embedded NULs, callback attach/detach,
  post-shutdown calls, double init/shutdown.

The checksum vector is asserted on **both** sides on purpose: if the linked
staticlib ever diverges from what `cargo test` exercises, that shows up as a
test failure rather than as a protocol mismatch much later.

## Open decisions (do not infer these from the code)
The skeleton is deliberately protocol-free. Deciding these *is* the next
design step, and the current entry points exist only to exercise the shapes
the real transport needs (buffers in, callbacks out, status codes):

- **Transport:** raw UDP + a hand-rolled reliability layer (sequencing, ack
  bitfields, fragmentation, ordered/unordered channels) vs QUIC (`quinn`).
  QUIC brings encryption and multiplexing for free but has no unreliable
  channel, which action games generally want for state.
- **Scope split:** what the engine owns (transport, RTT/time-sync) vs what
  kits own (replication, interest management, lobbies, rollback). Current
  intent is transport-only — APIs, not a subsystem built over them.
- **Dependencies:** `Cargo.toml` has none on purpose. Adding a crate commits
  the protocol before the choice is made.

## Known limitations
- No networking. This is a seam, not a transport.
- Multi-config generators (Xcode/MSVC) always build the debug Rust profile;
  the CMake glue needs per-config `IMPORTED_LOCATION_<CONFIG>` before Windows
  ships this module.
- Not yet linked into `engine_runtime` — nothing depends on it, which is why
  enabling it is currently risk-free.
