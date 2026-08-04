---
status: unreviewed
---
# Crash reporting for desktop games — design note

A `modules/`-level, engine-independent crash reporting kit: OS-level capture behind
a thin predictable abstraction, tiered attribution (game / engine / platform), a
developer-supplied report model, and the boring pipeline tooling that makes reports
actually arrive and actually be readable.

Written as a design note, not a plan of record. Nothing here is built.

## Why this is worth doing

The value is not the capture — Crashpad already captures. The value is everything
around it that every studio rebuilds badly:

- **symbol archiving keyed by build id**, so a stack from a six-month-old build still
  resolves;
- **attribution**, so "1 400 crashes" becomes "1 200 are one driver bug, 150 are our
  kit, 50 are real engine bugs";
- **a queue that survives the crash** and uploads on next launch with backoff and a
  kill switch;
- **redaction and consent**, which is legally required and always bolted on last;
- **triage tooling** — dedup by signature, group, diff against last build.

That is the same shape as the DDC work in `modules/assetlib`: unglamorous
infrastructure whose absence quietly costs more than any feature.

## The one architectural law

**A crashing process cannot be trusted to report its own crash.** The heap may be
corrupt, the stack may be exhausted, a signal handler may not allocate or take a
lock, and the thread that faulted may hold the lock the reporter needs.

Therefore: a separate **handler process**, started at game launch, outliving the
crash, which reads the dying process's state from outside. In-process code does the
absolute minimum — publish annotations into shared memory *continuously* (never at
crash time), and at most signal the handler.

Everything below follows from this. The user's instinct that desktop makes this
tractable is correct: process creation is cheap and permitted, which is exactly why
this design is desktop-only and would need rethinking on console.

## Recommendation: borrow the capture, own the abstraction

The honest cost breakdown of writing capture from scratch:

| Platform | Correct mechanism | Why hand-rolling hurts |
|---|---|---|
| Windows | Handler process + `MiniDumpWriteDump` **called from the handler** on the crashed process | `SetUnhandledExceptionFilter` alone is bypassed by `__fastfail`, stack overflow, and heap corruption. Also needs `_set_invalid_parameter_handler`, `set_terminate`, `set_purecall_handler`, and a guard-page story. |
| macOS | **Mach exception ports** (`task_set_exception_ports`) | POSIX signals are lossy on macOS and miss `EXC_BAD_ACCESS` detail. Hardened runtime / notarization affect whether you may attach at all (`com.apple.security.get-task-allow`). Apple silicon needs arm64 thread state. |
| Linux | Signals on an **alternate stack** (`sigaltstack`) + handler process reading via ptrace | Stack overflow is uncatchable without `sigaltstack`. Yama `ptrace_scope` needs `prctl(PR_SET_PTRACER)`. Core-pattern interaction and container namespaces are their own tarpit. |

Each cell is months of platform-specific bug-hunting on OS versions you do not own.
**Crashpad already did it**, is BSD-licensed, and is the handler Chrome and a lot of
shipped games rely on.

So: **`CaptureBackend` is an interface with Crashpad as the first implementation**, a
native backend as an optional later one, and a `null` backend for tests. The thin
predictable module the idea calls for is real — it is just thin over a proven
capture layer rather than thin over raw OS APIs. That inverts the effort into the
parts that are actually differentiated.

## Tiering: more categories than three, and classify LATE

The game / engine / OS split is the right idea, and shipped desktop games produce
more than three kinds of failure. Proposed set:

| Tier | What it is | Notes |
|---|---|---|
| `Game` | innermost owned frame is in a game module / kit `.dll`/`.so` | the studio's bug |
| `Engine` | innermost owned frame is `engine_runtime` / `engine_core` | our bug |
| `Platform` | OS or system library | usually not actionable, but the *count* is |
| `Gpu` | device removed/lost, TDR, driver hang, shader compile failure | **often not a process crash at all** — the process survives; needs its own hook |
| `Assert` | controlled abort: engine assert, `panic`, allocator invariant | cleanest data, richest capture possible |
| `Hang` | no crash — watchdog saw no frame for N seconds | needs a heartbeat in shared memory; the handler decides |
| `Resource` | OOM, disk full, GPU VRAM exhaustion, asset missing in a shipped build | reads as a crash to users, is not a code bug |

Two rules that matter more than the taxonomy:

1. **Attribution is a heuristic, so record the inputs and classify server-side.**
   Capture stores loaded-module ranges, the full stack, and the fault address. The
   tier is *derived*, never a field written at crash time — otherwise a wrong
   precedence rule you shipped last year is unfixable.
2. **Attribute to the innermost frame owned by a known module, not the innermost
   frame.** A crash in `memcpy` called from game code is a `Game` crash. Requires an
   explicit, versioned ownership table (module name → tier), registered at startup.

## The developer's report model

The dev supplies their own payload; the module supplies the crash-safe channel.

- **Annotations** — a fixed-capacity key/value table in shared memory. Set anytime,
  read by the handler after the crash. No allocation, no locks: fixed slots with an
  atomic sequence counter per slot (torn reads are detectable and reported as such
  rather than silently trusted).
- **Breadcrumbs** — a lock-free ring buffer of small fixed-size records (level,
  frame number, monotonic timestamp, short text). Last N survive; the ring is
  pre-allocated at init.
- **Attachments** — file paths registered up front (log file, `project.json`,
  graphics settings). The handler reads them *after* the crash; the game never
  serializes anything at crash time.
- **Sinks** — `IReportSink` is what the studio implements to point at their backend.
  Default implementations: `DiskQueueSink` (always on, first in the chain) and
  `HttpSink` (multipart POST + retry). The disk queue is not optional: it is what
  makes reports survive a crash during upload.

Deliberately **no** in-process serialization API. If a call could allocate at crash
time it does not belong in this module.

## Symbols, and where the engine already helps

A report without symbols is a receipt for a crash you cannot fix.

- Build step extracts symbols and archives them keyed by **build id** — PDB
  GUID+age on Windows, `.note.gnu.build-id` on ELF, dSYM UUID on Mach-O.
- Symbolize **server-side or in tooling**, never on the player's machine: shipping
  symbols to clients hands out your source structure and bloats the download.
- The store is content-addressed by build id, which is exactly `modules/assetlib`'s
  DDC shape — hash-keyed immutable blobs with a GC and a budget. That is real reuse,
  not a coincidence, and it is the strongest argument for building this inside this
  repo rather than as a standalone project.

## Delivery: assume the network is hostile

- Write the report to a **local queue first**, always. Upload attempts are separate
  from capture.
- Upload from the **handler process** if it survives, otherwise on **next launch**.
- Client-side dedup by stack signature, exponential backoff, and a **rate limit** —
  a bad patch will otherwise turn your own player base into a DDoS against your
  endpoint.
- A server-controlled **kill switch** and sample rate, fetched with the game's
  existing config. A crash reporter that cannot be turned off remotely is a liability.

## Privacy is a requirement, not a feature

Minidumps contain memory: usernames in paths, clipboard contents, tokens, save data.

- **Consent** before the first upload, with a persisted answer.
- **Minimize by default**: stack + registers + module list + annotations. Full-heap
  dumps only behind an explicit opt-in, and never as the default.
- A **redaction pass** on paths and known-sensitive annotation keys before upload.
- A stated **retention policy**, because GDPR/CCPA make this someone's problem
  eventually.

## Module shape

Engine-independent, per `modules/manifest.md` — this must be usable outside this
engine, which is also the cleanest way to keep it honest.

```
modules/crashkit/
  include/crashkit/crashkit.h        the entire public surface, C-compatible
  src/capture/                       CaptureBackend: crashpad, native_*, null
  src/annotations/                   shared-memory KV + breadcrumb ring
  src/classify/                      module ownership table -> tier (versioned)
  src/sink/                          DiskQueueSink, HttpSink, IReportSink
  handler/                           the out-of-process handler executable
  tools/                             symbol archive, symbolize, triage/dedup
```

Engine side stays thin and separate: a small adapter that registers module ranges,
feeds breadcrumbs from the frame counter and profiler phases, attaches a `mem::`
heap census, and hooks the GPU device-lost path.

## Phasing, each step independently useful

1. **Annotations + breadcrumbs + disk queue, `null` capture.** No OS work at all.
   Testable immediately, and already useful for the `Assert` tier.
2. **Crashpad backend on one platform** (whichever the CI runner is). End-to-end:
   crash → dump → queue → symbolize in tooling.
3. **Symbol archive + `crashkit_symbolize`.** The point at which reports become
   readable rather than collected.
4. **Classification + triage tooling.** Where the idea's real value lands.
5. **Remaining platforms**, then `HttpSink`, consent and redaction.
6. **`Hang` watchdog and `Gpu` hooks** — the two categories nobody builds and
   everybody needs.

Native capture backends are explicitly *last*, and optional.

## How to test a crash reporter

This is mechanizable and belongs in the `stress` lane: a harness that spawns a child
which fails in one specific way, then asserts a report exists with the expected tier
and intact annotations.

Matrix worth covering: null deref, stack overflow (deep recursion), heap corruption,
pure-virtual call, `abort`, `__fastfail`/`__builtin_trap`, uncaught C++ exception,
`std::terminate` from a noexcept violation, OOM, a crash on a non-main thread, a
crash *during shutdown*, a crash *inside the handler*, a second crash while a report
is queued, and a crash before `crashkit::init`.

The last four are where homegrown reporters fail, and none of them are hard to
provoke on purpose — which is the whole argument for testing this like any other
subsystem instead of hoping.

## Honest risks

- **The field is not empty.** Crashpad, Sentry Native, Backtrace, BugSplat all
  exist; Sentry in particular already wraps Crashpad and has a backend. The
  defensible niche is engine-integrated tiering plus permissive licensing plus no
  mandatory SaaS — not "a better minidump writer".
- **Platform drift is permanent maintenance.** OS updates break capture. Any plan
  here is a subscription, not a purchase, and that argues hard for the Crashpad
  backend doing the drifting parts.
- **Attribution can mislead.** A confident wrong tier is worse than no tier, which
  is exactly why classification is server-side and re-runnable.
- **Scope creep into a service.** Ingestion, storage, dashboards and auth are a
  product. This note stops at `IReportSink` on purpose.
