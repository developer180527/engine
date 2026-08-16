---
status: plan
covers:
  - src/core/memory/
---
# Efficiency, platform budgets, and shipping on phones

> **Status: analysis and plan.** The memory findings are read from the tree; the
> mobile numbers are platform facts, not measurements from this engine — nothing
> here has been run on a phone, and that is the first thing to fix.

## 0. The blocker to deal with before any of this matters

**Every texture this engine cooks is unusable on iOS and Android.**

`texture_encode.cpp` emits BC1, BC3, BC5 and BC7 (`kTexBC7`, `kTexBC5`, …). Those
are desktop formats. Apple GPUs support ASTC, ETC2 and PVRTC and **no** BC
variant; Adreno and Mali support ASTC and ETC2, and where BC appears at all it is
an unreliable extension. So a mobile build would either fail to create every
texture or fall back to uncompressed RGBA8 — 4× the memory and bandwidth on the
platform that has the least of both.

What that costs to fix, roughly in order:

1. **ASTC as the mobile codec**, ETC2 as the floor for very old GLES 3.0 devices.
   6×6 is the usual size/quality compromise; 4×4 for UI and anything with hard
   edges. An encoder needs vendoring (`astcenc` is the obvious choice, and it is
   slow — which the DDC makes tolerable because nobody re-cooks twice).
2. **Normal maps do not port directly.** BC5's two-channel XY trick has no ASTC
   equivalent; the common answer is ASTC with a swizzle and Z reconstructed
   in-shader, at a quality cost that has to be looked at rather than assumed.
3. **The cook becomes per-TARGET, not per-project.** `kTexBC7` etc. are a format
   enum in the asset header, so a cooked texture is already tagged — what is
   missing is a target axis in the cook key so `.cache` can hold both, and a
   packaging step that ships only one. The DDC already keys on cooker version and
   settings fingerprint, so this is an extra input to an existing hash, not a new
   mechanism.
4. **`shader_cooker`'s profile gating is the precedent to copy.** It already
   refuses a D3D profile on a macOS host (`profileCookableOnThisHost`). Textures
   need the same idea in reverse: a target that requires ASTC must not silently
   receive BC.

Until this exists, "runs on iOS like Resident Evil" is not a roadmap item, it is a
compile error waiting to happen. Everything below is worth less than this.

## 1. Manual page management — you already have the hard part

`src/core/memory/` is not a malloc wrapper. It is a two-layer allocator: TLSF
arenas over **2 MB-aligned regions obtained from `mmap`/`VirtualAlloc(MEM_RESERVE)
+ MEM_COMMIT`**, with the page size read from `getpagesize()` rather than assumed,
and `mapEventCount()` exposed as "THE syscall metric". That is the foundation most
engines never build, and it means the remaining work is specific and small.

What is missing, in the order it pays:

### 1.1 Reserve big, commit late — and know the difference

Reserving address space is free; committing pages is what the OS charges for. The
useful pattern is one large reservation per heap with pages committed as the heap
grows, and the **committed high-water tracked per tag**. Today `mem::stats(tag)`
reports `currentBytes` — live bytes, not committed pages, and not the peak. Those
three numbers answer three different questions and only one exists.

This is not a micro-optimisation. See §2: on a phone, committed bytes are what
gets you killed.

### 1.2 Actually return pages

Verify — and fix if it does not — whether a heap ever gives a 2 MB block back
after a spike. Grow-only is invisible on a desktop with swap and fatal on a phone
without it. The calls are not symmetric and the difference matters:

| | reserve | commit | decommit | release |
|---|---|---|---|---|
| Windows | `VirtualAlloc(MEM_RESERVE)` | `MEM_COMMIT` | `MEM_DECOMMIT` | `MEM_RELEASE`, length must be 0 |
| Linux | `mmap(PROT_NONE, MAP_NORESERVE)` | `mprotect(RW)` | `madvise(MADV_DONTNEED)` — **zeroes, immediate** | `munmap` |
| macOS / iOS | same as Linux | same | `madvise(MADV_FREE)` — **lazy, keeps contents until reclaimed** | `munmap` |
| Android | as Linux | as Linux | `MADV_DONTNEED`, or `MADV_FREE` on newer kernels | `munmap` |

`MADV_DONTNEED` and `MADV_FREE` are not interchangeable: one loses the data now,
the other maybe never. A decommit path written against one and shipped on the
other is a heisenbug. And decommit is **page-granular** — trimming half a page
does nothing, which is the argument for keeping arenas page-aligned and
page-sized rather than byte-packed.

### 1.3 Page size is not 4096

Apple Silicon and iOS use **16 KB** pages. Android 15 introduces 16 KB support.
x86 Linux/Windows use 4 KB. `mem.cpp` already calls `getpagesize()`; the risk is
elsewhere — any arena sizing, alignment constant or "round to 4096" that creeps in
later will waste 75% of a trim on the platforms you are targeting. Worth a
`static_assert`-adjacent runtime check at boot rather than a comment.

### 1.4 Huge pages: a server story, not a game story

Tempting and mostly a dead end for a shipped client:

- **Windows large pages require `SeLockMemoryPrivilege`** — administrator or group
  policy. Not something a consumer game can rely on.
- **macOS** superpages are x86-only; Apple Silicon's 16 KB base page already gets
  much of the TLB benefit.
- **iOS: none.** **Android: no reliable control**, transparent huge pages vary by
  vendor.
- Linux is the only place `MADV_HUGEPAGE` is straightforwardly worth it, which
  makes it a dedicated-server optimisation.

The 2 MB block alignment already in `mem.cpp` is the right call regardless — it
makes the pointer→header lookup a mask, and it is THP-friendly where THP exists.
Do not spend a week chasing explicit huge pages for the client.

### 1.5 The two techniques that actually move mobile numbers

**Memory-map read-only assets.** Clean, file-backed pages can be evicted by the
kernel and re-read; dirty anonymous pages cannot. Mapping cooked blobs instead of
reading them into heap turns footprint into cache, and it is what lets a game hold
far more content than its kill limit. This also lands exactly where the audio ABI
already went: `ENGINE_AUDIO_F_STREAM` promises the engine keeps `bytes` alive for
the life of the sound, and the sane way to honour that for a 40 MB music track is
a mapping, not a heap block.

**Mark caches purgeable/volatile.** Tell the OS "you may take this back" instead
of being killed for holding it: `vm_purgable_control` with `VM_PURGABLE_VOLATILE`
on macOS/iOS, `MADV_FREE` on Android. Texture and mesh caches are the natural
candidates, and this engine already has the hook — `AssetService::evictOverBudget`
is the eviction policy; it just has no notion of the OS asking.

## 2. Mobile is a survival problem before it is a performance problem

This is the part that reframes everything above. On desktop, over-committing is
paid for in paging. On a phone there is no paging to speak of:

- **iOS jetsam** enforces a hard per-process footprint. The limit is device
  dependent (roughly 1.3–1.4 GB on 2–3 GB devices, more on 6 GB+ hardware) and
  exceeding it **terminates the process** — a game may request
  `com.apple.developer.kernel.increased-memory-limit`, which raises the ceiling and
  does not remove it. There is no swap.
- **Android lmkd** kills by priority under pressure, plus per-app heap limits;
  `onTrimMemory` is the warning and it is not generous.

So the metric that matters on mobile is **peak committed bytes**, not average, not
live-object bytes. One 400 MB spike during level load kills the app even if the
steady state is 300 MB. Which means:

1. Track and report **peak committed per tag**, and make it a CI-visible number.
2. Wire the OS pressure callbacks (`didReceiveMemoryWarning`, `onTrimMemory`) into
   `evictOverBudget` so the engine sheds caches instead of dying.
3. Prefer mapped and purgeable memory for anything that can be re-derived.
4. Treat the loading path's transient peak as a first-class budget. The texture
   cooker's own header already warns an 8K source costs ~256 MB RGBA and ~1 GB in
   the BC7 float working buffer — that is a cooker-side peak today, and the same
   class of spike on-device is fatal rather than slow.

## 3. "Efficient" is a different scoreboard from "fast"

The distinction in the question is the right one, and it deserves its own set of
numbers because frame time hides all of them. Every measurement in this repo today
is frame time, on a plugged-in Mac.

| Metric | Why it is not frame time | Where it bites |
|---|---|---|
| **Joules per frame** | a frame rendered twice as fast at twice the power is not better | battery, and thermals → sustained fps |
| **Sustained fps after 20 min** | thermal throttling means peak fps is a lie on mobile | the only fps a player experiences |
| **Peak committed bytes** | §2 — this is a kill threshold, not a slowdown | iOS/Android |
| **Install and download size** | ASTC vs RGBA8 is 4×; a 4 GB mobile download is a lost install | store conversion |
| **Time to first frame** | cold start, not steady state | retention |
| **Bytes/sec on the wire** | a mobile FPS runs on cellular | player data cost |

Three consequences worth acting on:

**Race to idle, then stop.** On mobile the goal is to hit the frame deadline
consistently and then let the SoC clock down — not to render as many frames as
possible. Frame pacing (CADisplayLink / Android's Choreographer or Swappy) with a
target cadence beats an uncapped loop on both battery and *sustained* frame rate.
An engine built for "motion-to-photon latency" has to hold both ideas at once:
minimal latency within the frame, and no work beyond the frame's budget.

**Look hard at the job pool's idle behaviour.** Spinning workers are close to free
on a desktop core and expensive on a phone — they burn power and steal thermal
headroom from the cores doing real work. Whether `jobs::` parks or spins when idle
is a power decision with no frame-time signal, which is exactly the kind that goes
unnoticed. Same for any busy-wait.

**Big.LITTLE scheduling is not automatic.** A render or audio thread migrated onto
a little core produces a hitch with no code change to blame. Thread affinity and
QoS hints (`pthread_set_qos_class_self_np` on Apple, `sched_setaffinity` /
priority on Android) are part of shipping, not a nicety.

## 4. What to do, in order

1. **ASTC/ETC2 in the texture cooker, and a target axis in the cook key.** Nothing
   mobile is real until this lands. §0.
2. **Add committed and peak-committed per tag** to `mem::stats`, and print them
   beside the existing residency dump. Cheap, and it is the number §2 turns on.
3. **A device lane in the farm** — one iPhone, one mid-range Android — reporting
   sustained fps, peak footprint and time-to-first-frame. The RHI plan already
   argues the farm needs real GPUs; this is the same argument for the same reason,
   and both are blocked on it.
4. **Wire OS memory-pressure callbacks into `evictOverBudget`.**
5. **Map cooked assets read-only** instead of reading them into heap, starting
   with the biggest: audio streams and textures.
6. **Decommit path**, with the `MADV_FREE`/`MADV_DONTNEED` distinction handled per
   platform and a test that asserts a heap actually shrinks.
7. **Purgeable caches**, once 4 and 5 exist and there is a number to improve.

Explicitly NOT on this list: a custom malloc (there is one, it is TLSF over
mmap'd blocks, it is fine), and explicit huge pages on client platforms (§1.4).
