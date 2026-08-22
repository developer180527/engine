---
status: reference
---
# Audio Provider — implementing `EngineAudioProviderV1`

**Header:** `include/engine/engine_audio_provider.h`
**Conformance suite:** `tests/audio_conformance/` (Rust)

This is the contract a **replacement audio engine** implements: miniaudio (the
intended default), an FMOD or Wwise adapter, a Rust spatial engine, someone's
ray-traced audio experiment.

The engine calls it. The provider calls back **only** through pointers the engine
handed it explicitly — `EngineAudioHostServices` and `EngineAudioStreamSource` —
never into engine globals, never by linking an engine symbol. That distinction is
what lets a provider be built by a different team, in a different language,
against a different engine version.

> **Status:** the seam is live. `src/audio/miniaudio_provider.cpp` implements
> this interface and `src/plugins/audio_plugin.h` consumes it — the engine's
> only remaining reference to miniaudio is the one line in
> `audio_host_services.h` that names the entry point.
>
> The **same source** builds twice: linked into the engine, and as a standalone
> `.so` (`engine_audio_miniaudio_module`) that the Rust suite dlopens. So the
> conformance lane tests the code the game actually runs, and the module's
> refusal to link against `engine_runtime` mechanically proves the provider is
> engine-independent.
>
> Two Rust reference points remain: `reference.rs` (a correct provider that
> makes no sound, so the suite has something known-good to validate itself
> against) and `run_playback`, which drives real WAV audio through decode,
> F_STREAM and pull-streaming.
>
> Not yet done: emitters are not driven from the scene — `updateEmitters` and
> `setListener` are implemented on both sides but the engine still plays sounds
> at fixed positions with the listener at the origin.

---

## 1. The one decision everything follows from

**The provider owns the device, the real-time thread, and the mixer. It uses the
engine's jobs and allocators for everything else.**

The engine never fills an audio buffer and never runs code on the audio thread.

That second sentence is as load-bearing as the first. A provider that brings its
own thread pool and its own allocator is not independent of the engine — it is
*invisible* to it. Four audio workers beside the engine's pool beside a physics
provider's pool oversubscribes the cores: those threads do not run in parallel,
they context-switch, evict each other's cache lines, and each idle pool burns
power spinning. A provider calling `malloc` sits outside the tagged-heap
telemetry, so nobody can answer "what does audio cost" — which is the entire
content of a memory budget review.

So the split is:

| | Owner |
|---|---|
| device, real-time thread, mixer | **provider** |
| decode, streaming refill, propagation, ray tracing, reverb solve | **engine's `parallelFor`** |
| every allocation | **engine's tagged heap** |
| the clock `hostTimeNs` is sampled from | **engine's `nowNs`** |

The real-time thread is the one exception, and it is not negotiable: the audio
callback is scheduled by the OS driver at elevated priority against a hard
deadline, while a pool worker runs arbitrary queued work and gets preempted. A
mixer on a pool thread is a mixer that eventually clicks.

This is not a stylistic choice. If the engine owned the device and called the
provider to fill buffers, this ABI would sit *inside* the real-time callback,
and allocation, locking and unwinding rules would become joint problems on both
sides of it. Instead:

| Rate | What happens | Crosses this ABI? |
|---|---|---|
| 48 kHz | mixing, filtering, HRTF convolution | **never** |
| 60–120 Hz | emitter + listener updates | one batched call per frame |
| 5–30 Hz | propagation, ray tracing, reverb solving | **never** (provider's own workers) |

At 48 kHz with a 128-frame buffer the mixer runs **375 times a second** and
crosses this interface **zero** times. What crosses is *control*, at frame rate:
a few nanoseconds per call against a 16,600 µs budget.

That is why adding a provider indirection costs nothing measurable, and why
"the ABI will hurt audio latency" is a misconception worth correcting early.

---

## 2. Implementing a provider

### 2.1 Export the entry point

Your module is a shared library exporting one symbol:

```c
const EngineAudioProviderV1* engineAudioProviderV1(void);
```

It returns a **static** table. The engine never frees it, and never unloads the
module while pointers into it are live.

### 2.2 Fill the table

Fourteen function pointers. **All are mandatory** — a null entry turns a missing
feature into a crash at first call, when `ENGINE_AUDIO_E_UNSUPPORTED` would have
been a clean refusal.

| Group | Functions |
|---|---|
| Lifecycle | `create`, `destroy`, `suspend` |
| Resources | `createSound`, `destroySound`, `createStream`, `findSound` |
| Playback | `play`, `stop` |
| Per-frame scene | `updateEmitters`, `setListener` |
| Acoustic scene | `setGeometry` |
| Passthrough | `setParam` |
| Diagnostics | `getStats` |

Set `version = ENGINE_AUDIO_PROVIDER_V` and
`structSize = sizeof(EngineAudioProviderV1)`.

### 2.3 The minimum viable provider

You can conform while doing almost nothing:

- `createSound` → `ENGINE_AUDIO_E_BAD_DATA` if you decode nothing
- `createStream` → `ENGINE_AUDIO_E_UNSUPPORTED` if you cannot pull
- `findSound` → `ENGINE_AUDIO_E_UNSUPPORTED` if you have no name registry
- `setGeometry` → `ENGINE_AUDIO_E_UNSUPPORTED` if you do no propagation
- `setParam` → ignore every hash
- `getStats` → report the device's real sample rate and `callbackOverruns = 0`

What you may **not** skip is the host services: allocate through
`services->alloc` and sample `services->nowNs` for `hostTimeNs`. The conformance
suite passes a counting implementation and asserts both, so "took the struct and
called `malloc`" is a detectable failure rather than a silent one.

`tests/audio_conformance/src/reference.rs` is exactly this.

### 2.4 Sound identity: bytes, a reader, or a name

Three ways to get an `EngineSoundId`, because providers differ in kind:

| Call | For | Who holds the data |
|---|---|---|
| `createSound(bytes, …)` | a cooked asset in memory | engine, until `destroySound` if `F_STREAM` |
| `createStream(src, …)` | audio in an archive, over the network, decrypted lazily | engine serves reads on demand |
| `findSound(nameHash, …)` | an event inside a loaded bank | provider |

`findSound` is what makes this interface expressible for **Wwise and FMOD at
all**. Those are event systems: a title ships banks and posts `Play_Gunshot`,
and the designer's graph decides what actually plays — layers, randomisation,
RTPCs. There is no buffer of gunshot bytes to hand over, and addressing content
by *name* is the only thing sample-based and event-based providers have in
common. Load the bank with `createSound(…, ENGINE_AUDIO_F_BANK, …)`; the
returned id owns the container, and `findSound` resolves the events inside it.

Hash names with **`engineAudioHashName`**, defined inline in the header. Engine
and provider each compile their own copy, so both must be the same function —
FNV-1a 64, pinned by literal in both `tests/audio_abi_check.c` and the Rust
suite. A divergence here produces no error at all: every lookup simply misses
and the sound never plays.

---

## 3. The call sequence

**At startup**

```
create(desc, services)      → void* self          (E_NO_DEVICE is normal!)
createSound(bytes, count, flags) → EngineSoundId  (per asset, game thread)
createStream(src, flags)    → EngineSoundId       (pull-based resources)
findSound(nameHash)         → EngineSoundId       (events inside a bank)
```

Copy `services` **by value** at `create`: the engine promises the services
outlive the instance, not that the pointer does.

**Every frame**

```
setListener(&listener)                       // camera pose
updateEmitters(array, count, stride)         // ONE call, everything that moved
play(&desc) / stop(voice, fadeMs)            // as gameplay demands
```

**Occasionally**

```
setGeometry(&geom)          // level load, door opens, wall destroyed
setParam(obj, hash, value)  // provider-specific control
suspend(self, 1|0)          // focus loss / OS interruption
```

**At shutdown**

```
destroySound(id) …
destroy(self)
```

### Batching is not optional

`updateEmitters` takes an **array** — and a **stride**. Walk it with the stride
the engine gave you, never with `sizeof(EngineAudioEmitterUpdate)`: a newer
engine may send WIDER rows, and indexing with your own struct size would read
every row after the first at the wrong offset, silently, with values that look
plausible. Ignore trailing bytes you do not recognise, and refuse a stride
smaller than your own view of the struct (that means the engine is older than
you are and the fields you expect are absent). A busy scene has hundreds of moving
emitters, and sending them as hundreds of calls is the per-object anti-pattern
this interface exists to avoid. Static emitters — ambience, machinery — send no
row at all; only what moved is in the array.

---

## 4. Constraints

### 4.1 Threading

Every function is called from the **game thread** (the engine's main/sim
thread). **Nothing is called from the audio thread.**

- The engine guarantees it will not call two functions concurrently for the
  same instance.
- **You** must guarantee none of them block on the audio thread. A mutex shared
  with your mixer turns a `play()` call into a frame hitch — a priority
  inversion that shows up as inexplicable stutter far from its cause.

`createSound` may block and decode; it is explicitly not real-time.

Two callbacks run the *other* way — the engine's `read` in
`EngineAudioStreamSource`, and everything in `EngineAudioHostServices`. Both are
called **by you**, and neither may be called from the audio thread: `read` can
block on a file, `alloc` can block on a lock, `parallelFor` waits for workers.
All three are deadline violations there.

Call `read` from your own streaming worker. The engine's implementation is safe
against concurrent calls for different sounds.

### 4.2 Real-time safety inside your callback

No locks, no allocation, no syscalls, no file I/O, no unbounded work. Decoding
and streaming belong off the real-time thread — on `parallelFor` for bulk work,
or on your own worker when you need tighter latency than the pool promises.

**The pool has no priority classes.** Decode work queues behind gameplay work.
That is fine for buffers of hundreds of milliseconds and a real constraint for
tight streaming; a private thread for *that specific case* is legitimate, and a
provider that needs one should say so in its documentation.

`callbackOverruns` in `getStats` is the number that matters: a missed deadline
is an audible click, and it is the only audio failure a player notices
instantly. The conformance suite asserts it stays zero under a 2,000-command
burst.

### 4.3 Buffer lifetime — the subtle one

`createSound` takes bytes, and how long you may hold them depends on the
`flags` IT WAS GIVEN — not on how the sound is later played:

- **`flags == 0`** (fully decoded) — decode during the call; the engine may free
  the buffer the moment you return.
- **`ENGINE_AUDIO_F_STREAM`** — you may read from the buffer for as long as the
  sound exists, so **the engine keeps it alive until `destroySound`**. Music is
  tens of megabytes; decoding it up front would cost more resident memory than
  the file does, and a memory-mapped file keeps it virtual.

F_STREAM used to be a PLAY-time flag, which made this contract unsatisfiable by
either side: you had to choose retain-or-decode during `createSound`, before any
`play()` said which, and the engine had to decide whether to keep the buffer
alive on the strength of a call that had not happened yet. Streaming is a
property of the resource, so it is decided where the resource is created.

A provider that cannot stream from memory may decode instead — correct, just
heavier. It must **not** retain a pointer it was not promised.

- **`createStream`** — no engine buffer at all. You call `read(offset, dst, n)`
  when you need bytes, so an archive, a network source or lazy decryption all
  work. `read` and its `userData` stay valid until `destroySound`.

`setGeometry`'s arrays are the opposite: copy or build your acceleration
structure during the call; they do not outlive it.

### 4.4 Stats is an out-parameter, so its size check runs backwards

For every other struct here the *producer* sets `structSize` and the consumer
adapts. `getStats` inverts that: the **engine** sets `outStats->structSize`, and
you write only the fields that fit inside it.

Skipping this check is a stack smash the day a provider is newer than the engine
calling it — and it is the one direction frozen layout cannot protect you from,
because the writer is the newer party.

### 4.5 ABI rules

- Fixed-width types only. No `bool`, no `size_t`, no plain `enum`.
- Structs are append-only and carry `structSize`.
- No C++ types cross the boundary.
- **No exception or panic may escape any function.** In Rust: `panic = "abort"`
  or `catch_unwind` at every entry. Unwinding out of the audio thread is the
  worst possible place to discover otherwise.

### 4.6 Failure is normal

`create` returning `ENGINE_AUDIO_E_NO_DEVICE` is an expected outcome, not an
error to abort on — dedicated servers and CI have no output device, and the
engine simply runs silent.

---

## 5. What is deliberately absent

There is no `setHRTF`, no `setAtmosBed`, no `enableRayTracedOcclusion`.

**The engine models the scene. The provider models the audio.** Spatial audio,
object-based rendering, Dolby Atmos, binaural/HRTF and acoustic propagation all
need the *same three things* from the engine:

1. emitter poses (`updateEmitters`)
2. a listener pose (`setListener`)
3. the world's acoustic geometry (`setGeometry`)

Whether that becomes stereo panning, an object bed, ambisonics or a path-traced
impulse response is entirely yours, configured through **your own** config file.

This is what keeps the interface from growing. A feature-shaped interface grows
with every audio technology ever invented, and every provider has to implement
all of it.

### Doing provider-specific things anyway

```c
setParam(objectId, nameHash, value)   // objectId = a voice, or 0 for global
```

The engine forwards this **uninterpreted**. A Wwise RTPC, a "switch to the
expensive HRTF dataset" toggle, an experimental knob that exists for one
afternoon — all of it goes here, and the engine never learns what `"rt_bounces"`
means. Wwise and FMOD both work this way in practice.

### The test before adding a function

> **Could an FMOD or Wwise adapter implement it?**

Both are event/parameter systems with C APIs. If the answer is no, the proposed
function is describing *our implementation* rather than *the scene*, and it
belongs behind `setParam`.

`findSound` is the one function added *because* of that test rather than despite
it. Without name-based addressing a Wwise or FMOD adapter could not be written at
all, and event names would have had to be smuggled through `setParam` — which is
what an abstraction looks like when it is wrong.

---

## 6. Testing your provider

The conformance suite is the specification. A provider that compiles against
the header and fails the suite is not a provider.

```bash
cd tests/audio_conformance
ENGINE_AUDIO_PROVIDER=/path/to/libyour_provider.so cargo test -- --nocapture
```

Without the environment variable it runs the Rust reference provider only.

It is written in Rust **on purpose**: the claim this ABI makes is that a
non-C++ language can implement it, and a suite plus a reference provider in
Rust tests that claim mechanically. If the header had smuggled in a C++ layout
assumption, name mangling or an exception, none of it would link.

What it checks — every item is something a real provider gets wrong at least
once:

- garbage bytes rejected, never decoded into a wild pointer
- invalid sound/voice ids refused rather than dereferenced
- **256 stale voice ids in a bulk update ignored** — the engine legitimately
  sends them, because a voice can finish between frames
- an empty update `(null, 0)` accepted
- `setGeometry` returning `OK` or `E_UNSUPPORTED`, never crashing
- `setParam` swallowing unknown hashes and NaN
- **zero callback overruns** under a command burst
- `samplesPlayed` monotonic — it is the clock `startSampleTime` is computed
  against
- **allocations actually go through host services** — a counting allocator, so a
  provider that ignores them and calls `malloc` fails here rather than silently
  escaping the memory budget
- **the sample clock and the host clock agree on elapsed time**, which is what
  proves the `samplesPlayed`/`hostTimeNs` pair was sampled together
- `findSound` refusing an unknown name and the reserved hash `0`
- `createStream` refusing a null reader, and not minting a live id from a reader
  that fails immediately

33 assertions in all. Each new one is mutation-verified: breaking the reference
provider in that specific way makes exactly that line fail, and restoring it
passes again.

### The two halves of the ABI freeze

The Rust suite asserts **its own** struct sizes at compile time. That catches
Rust-side drift and is **blind to the C side moving underneath it**.
`tests/audio_abi_check.c` is the other half: a C11 translation unit that includes
the header — which is what makes its `ENGINE_AUDIO_FROZEN` static asserts compile
at all — and pins the field **offsets** and the name hash, both invisible to a
`sizeof` check. Swapping two `uint64` fields keeps every size identical and
silently rebinds every read; only the offset check sees it.

Both are `ctest` lanes (`audio_abi_check`, `audio_abi_conformance`, label
`unit`). The numbers in the two files are the same numbers, so neither side can
move alone.

---

## 7. Sample-accurate scheduling

`EngineAudioPlayDesc::startSampleTime` is `0` for "as soon as possible", or an
absolute value on your clock (`getStats().samplesPlayed`).

Honour it by starting the voice at that exact sample *within* a buffer rather
than at the buffer boundary. Without it, a sound triggered mid-frame quantises
to the next callback — audible on rapid fire and anything rhythmic.

### It needs two numbers, not one

`samplesPlayed` alone cannot be scheduled against. The game thread polls
`getStats` and learns a sample *count*, but not **when** that count was true —
between you publishing it and the engine reading it, an arbitrary and variable
amount of time passed. Placing a future sound on the engine's timeline requires
mapping its clock onto yours, and one number is not a mapping.

So report **`hostTimeNs`** alongside it: `services->nowNs()`, read at the same
instant as `samplesPlayed`, adjacently, published together. Then:

```
samplesAtHostTime(t) ≈ samplesPlayed + (t - hostTimeNs) * sampleRate / 1e9
```

Two readings apart in time also give the **drift** between the clocks, which is
real — an audio device's crystal is not the host's, and over a few minutes they
separate by milliseconds.

Report `0` if you do not track it; the engine falls back to buffer-boundary
quantisation. Do not substitute a private `clock_gettime`: two monotonic clocks
share no epoch, and the conformance suite checks that `nowNs` was the source.

---

## 8. Why swapping providers is safe

Because the expensive work is structurally isolated. A developer experimenting
with ray-traced audio swaps one `.so`; the engine does not change, is not
recompiled, and cannot be broken by the experiment. If their tracer is too slow,
it is *their* worker threads that suffer — the frame thread and the mixer are on
different clocks and neither waits for it.

That isolation is the feature.
