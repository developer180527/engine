---
status: reference
---
# Audio Provider — implementing `EngineAudioProviderV1`

**Header:** `include/engine/engine_audio_provider.h`
**Conformance suite:** `tests/audio_conformance/` (Rust)

This is the contract a **replacement audio engine** implements: miniaudio (the
intended default), an FMOD or Wwise adapter, a Rust spatial engine, someone's
ray-traced audio experiment. The engine calls it; nothing in it calls back into
the engine.

> **Status, honestly:** the interface and its conformance suite exist and are
> verified. **No provider is wired into the engine yet** — audio still runs
> through `src/plugins/audio_plugin.h` (miniaudio, C++, `IEnginePlugin`).
> Porting that behind this interface is the next step, and until it happens the
> seam is unproven in production. A reference provider in Rust
> (`tests/audio_conformance/src/reference.rs`) proves the interface is
> implementable; it makes no sound.

---

## 1. The one decision everything follows from

**The provider owns the device, the real-time thread, and the mixer.**

The engine never fills an audio buffer and never runs code on the audio thread.

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

Twelve function pointers. **All are mandatory** — a null entry turns a missing
feature into a crash at first call, when `ENGINE_AUDIO_E_UNSUPPORTED` would have
been a clean refusal.

| Group | Functions |
|---|---|
| Lifecycle | `create`, `destroy`, `suspend` |
| Resources | `createSound`, `destroySound` |
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
- `setGeometry` → `ENGINE_AUDIO_E_UNSUPPORTED` if you do no propagation
- `setParam` → ignore every hash
- `getStats` → report the device's real sample rate and `callbackOverruns = 0`

`tests/audio_conformance/src/reference.rs` is exactly this, in ~200 lines.

---

## 3. The call sequence

**At startup**

```
create(desc)                → void* self          (E_NO_DEVICE is normal!)
createSound(bytes, count, flags) → EngineSoundId  (per asset, game thread)
```

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

### 4.2 Real-time safety inside your callback

No locks, no allocation, no syscalls, no file I/O, no unbounded work. Decoding
and streaming belong on your own worker thread.

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

`setGeometry`'s arrays are the opposite: copy or build your acceleration
structure during the call; they do not outlive it.

### 4.4 ABI rules

- Fixed-width types only. No `bool`, no `size_t`, no plain `enum`.
- Structs are append-only and carry `structSize`.
- No C++ types cross the boundary.
- **No exception or panic may escape any function.** In Rust: `panic = "abort"`
  or `catch_unwind` at every entry. Unwinding out of the audio thread is the
  worst possible place to discover otherwise.

### 4.5 Failure is normal

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

It also asserts Rust's struct sizes equal the C header's at **compile time**
(104 / 64 / 48 / 40 bytes). If those ever diverge the crate does not build.

---

## 7. Sample-accurate scheduling

`EngineAudioPlayDesc::startSampleTime` is `0` for "as soon as possible", or an
absolute value on your clock (`getStats().samplesPlayed`).

Honour it by starting the voice at that exact sample *within* a buffer rather
than at the buffer boundary. Without it, a sound triggered mid-frame quantises
to the next callback — audible on rapid fire and anything rhythmic.

It ships in v1 with nothing behind it yet, because it is one `uint64` now and an
ABI break later.

---

## 8. Why swapping providers is safe

Because the expensive work is structurally isolated. A developer experimenting
with ray-traced audio swaps one `.so`; the engine does not change, is not
recompiled, and cannot be broken by the experiment. If their tracer is too slow,
it is *their* worker threads that suffer — the frame thread and the mixer are on
different clocks and neither waits for it.

That isolation is the feature.
