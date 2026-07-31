---
status: as-built
tier: hardened
verified: 2026-07-31
covers:
  - src/core/memory/
tests:
  - tests/mem_test.cpp
  - tests/arena_test.cpp
  - tests/stress_swarm.cpp
  - tests/stress_churn.cpp
---
# mem:: — the engine memory manager

Tagged heaps over a 2MB-block OS backend (mem.h has the full layer diagram).
Same philosophy as engine::jobs: **the OS is a boot/growth-time concern,
never a frame concern**. MemoryChannel's `map events` per frame is the
enforcement metric — steady state must be 0 (verified live: 49 lifetime map
events at boot, unchanged across minutes of play).

## How provenance works (the 2MB trick)
Every region we map is 2MB-aligned, so `ptr & ~(2MB-1)` lands on a
`BlockHeader` naming the owning heap — O(1) free/realloc routing with zero
per-allocation metadata. A lock-free-read registry of live block bases
answers "is this pointer ours at all"; a miss means FOREIGN memory
(allocated before routing, or by a dylib's system new) and forwards to
std::free/std::realloc.

## Allocators
- **TLSF** (third_party/tlsf, vendored) per tag heap — O(1) malloc/free,
  bounded fragmentation; grows 2MB at a time; blocks recycle, never unmap.
- **Large path** (>= 1MB): dedicated aligned mapping, munmap'd on free.
- **FrameArena** (core/frame_arena.h) stays the per-frame linear allocator.

## Routing — who lands where
| Source                     | Mechanism                                | Tag |
|----------------------------|------------------------------------------|-----|
| C++ new/delete (all TUs)   | global override (mem_counters.cpp)       | MEM_SCOPE, default Core |
| Jolt                       | JPH::Allocate/… fn ptrs (jolt_plugin.h)  | Physics |
| bgfx/bx                    | bx::AllocatorI (renderer.cpp)            | Rendering |
| Lua                        | lua_newstate alloc fn                    | Scripting |
| flecs                      | ecs_os_set_api at static init (runtime.cpp) | ECS |
| ozz                        | SetDefaulAllocator [sic] at runtime init | Animation |
| enkiTS                     | TaskSchedulerConfig.customAllocator      | Jobs |
| miniaudio                  | engine config allocationCallbacks        | Audio |
| ImGui                      | SetAllocatorFunctions (imgui_bgfx.cpp)   | Editor |
| AsyncLoader thread         | MEM_SCOPE(Assets) around the worker loop | Assets |

`MEM_SCOPE(tag)` pushes a thread-local tag; every untagged allocation under
it (std containers included) is attributed. Scopes must not straddle a
jobs::wait() (fiber-backend rule — see runtime/jobs/jobs.h).

## The switch
`ENGINE_MEM_ROUTE=1` (default) routes global new/delete through mem::.
Build with `-DENGINE_MEM_ROUTE=0` for AddressSanitizer / leaks / Instruments
runs — allocations keep system provenance, counters keep counting.

## Kit boundary rule (until the shared-core fix)
Kit dylibs keep the SYSTEM operator new. Kit-allocated memory freed engine-
side is safe (registry miss → std::free). The reverse — an engine-allocated
std::string/vector resized or destroyed inside a kit — would hand a TLSF
pointer to libc++ free() and corrupt: kits must mutate engine state through
the C API, never by resizing engine-owned containers in components. Fixed
properly by making the engine core a shared library (Future Work).

## Testing
`mem_test` (src/tools/) is the gauntlet: alignment, scope attribution, large
mappings, realloc, foreign pointers, cross-thread, 64k-alloc churn with zero
map events, budget warnings, routed-vector proof. Run it after ANY change
here.

## Future Work
- Windows backend: VirtualAlloc2 with MEM_ADDRESS_REQUIREMENTS for aligned
  reservations (mmap trick is POSIX-only) — fold into the Windows port.
- Engine core as dylib so kits share the allocator (removes the boundary rule).
- Per-tag budgets set from project config; editor Memory panel over
  MemoryChannel (numbers already exposed).
- Frame-tag linear heap for jobs-produced per-frame data (FrameArena is
  main-thread only).
- Dev forensics: guard pages / canaries on a debug switch; leak report by tag
  diffing boot vs shutdown snapshots.
