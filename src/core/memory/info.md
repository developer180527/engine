---
status: as-built
tier: hardened
verified: 2026-08-16
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


## Review findings, 2026-08-11 — six reported, three more found ✅ FIXED

A review of `mem.cpp`/`mem.h` arrived with six issues. Three were right as
stated, two were right and understated, one was half right, and verifying them
turned up three more. The corrections matter as much as the fixes:

**`regInsert` was an unbounded probe** over a 32 768-slot table with no
load-factor check. Full table = infinite loop **while holding `g_regMu`**, so
every later alloc and free that touches the registry blocks behind it — a hang
inside the allocator, with no crash and no stack naming the cause. Now bounded,
and a refused insert fails the allocation loudly. Both callers had to change too:
`Shard::grow` and `largeAlloc` now register BEFORE publishing the memory, because
a pool whose block is not in the registry hands out pointers `headerOf` cannot
resolve, and `free()` would route them to `std::free()` and corrupt the system
heap.

**Tombstones were never cleaned, and that was the likelier bug of the two — the
review only found the rarer one.** `regErase` always wrote a tombstone and
nothing rehashed, while `regHas` only stops at 0: as tombstones filled the table
every MISS walked further, toward a full 32 768-slot scan. Misses are the
foreign-pointer path in `free()`/`realloc()` — hot in exactly the mixed-allocator
case the design exists to support — and it needs only 32 768 *cumulative* ≥1 MB
alloc/free cycles, where a full table needs that many *live*. Erase now writes 0
instead of a tombstone when the next slot is already 0, which is safe (no present
key's probe chain can pass a zero) and covers the isolated-entry case large
allocations produce. Saturation is counted and warned once; if it ever fires the
answer is a double-buffered rebuild — **an in-place rehash is NOT safe** against
the lock-free readers, because a reader that stops at a freshly-zeroed slot gets a
false negative and `free()` then hands our pointer to `std::free()`.

**`magic` was written and never read.** Provenance came entirely from
`regHas(base)`, so a scribbled header was trusted completely — `h->shard->release()`
is a call through whatever those bytes now contain. `headerOf` now validates and
aborts with the block base named; returning null would route our own memory to
`std::free()` and carrying on makes a wild call, so both alternatives are worse
and neither says why.

**A non-power-of-two `align` reached mask arithmetic.** Every mask here — the
block mask, `largeAlloc`'s header offset, `tlsf_memalign`'s own contract — is only
correct for a power of two, and nothing checked. Reachable from OUTSIDE the
engine: `engineMemAlloc` forwards a kit's `align` straight through, so `align=24`
could return a pointer overlapping the very header it is later resolved through.
Refused at the choke point now, which is also what makes the `magic` check above
more than theoretical.

**`MEM_SCOPE` overflow corrupted OUTER scopes**, which is worse than the reported
"attributes to a shallower tag". A dropped push left `t_scopeTop` unchanged while
the matching `popTag` still decremented — so the pop consumed an enclosing frame,
every allocation in that scope was misattributed from then on, and the final pop
clamped at zero and hid it. Dropped pushes are counted and unwound first.
Mutation-proved: with the old pop, 40 nested scopes inside a `Tag::Assets` scope
come back as `Tag::Core`.

**The realloc claim was half right.** True only for LARGE blocks, and it
self-corrects rather than leaking: pool blocks are accounted at
`tlsf_block_size()` on both bump and drop, and a shrink inside the same TLSF block
genuinely changes nothing — touching `cur` there would make it wrong. Large blocks
are accounted at the requested size and the shrink path never updated
`userBytes`, so stats over-reported *until the free*, which then subtracted the
stale larger value and balanced out. The mapping is untouched, so this was never
memory-unsafe; it made the per-tag numbers lie, which for a telemetry-driven
budget is enough. Fixed with a `resized()` that moves `cur` **without** faking an
alloc/free pair.

**`largeFree` hand-rolled its own `cur` decrement** instead of calling `drop()` —
equivalent only by accident, and `bump`/`drop` are documented as the only two
places `cur` moves.

**And the documentation was wrong about the thing that matters most for mobile.**
mem.h promised 2 MB regions were "recycled through a free list". There is no such
list: `unmapRegion` has exactly one caller (`largeFree`), so a pool block, once
mapped into a shard, stays mapped and stays in that shard's pool list for the life
of the process. TLSF reuses memory *within* pools; whole blocks are never
reclaimed. Two consequences now stated in the header, both load-bearing for
`docs/architecture/platform-efficiency.md`: committed memory is the historical
HIGH-WATER of every (tag, shard) pair rather than the live set, and striping means
a tag allocated from by many threads holds up to `kShards` separate 2 MB blocks
even when almost nothing is live.

Not fixed, deliberately: measuring what that striping actually costs. It is a
number to collect on a device, not a claim to act on from a desktop.
