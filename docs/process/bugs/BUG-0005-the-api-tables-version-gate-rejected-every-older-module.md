## BUG-0005 — the API table's version gate rejected every older module
- found:     2026-08-19
- status:    fixed
- class:     abi
- where:     include/engine/engine_api_client.h
- symptom:   appending three API groups invalidated all three Kits and the game module wholesale — not degraded, rejected.
- cause:     the client shim gated on `structSize != sizeof(...)` and `have == want`. The first rejects a module because the table got BIGGER; the second means improving a group to v2 locks out every module asking for v1.
- pinned-by: tests/api_abi_compat_test.cpp
- lane:      unit
- proof:     the test lays an older, shorter table definition over the live table and asserts the function pointers resolve identically, and asserts the one direction that must keep failing (older host, newer module).
