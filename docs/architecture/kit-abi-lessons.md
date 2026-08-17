---
status: plan
covers:
  - include/engine/
  - src/runtime/
---
# What the CAD plugin contract has that this engine does not

> Read against `vCAD/docs/design/PLUGIN_CONTRACT.md` and `vCAD/abi/` (the header,
> `Loader.cpp`, `Manifest.cpp`). Written to separate three things that get
> confused when comparing two designs: what we already have and arrived at
> independently, what is genuinely portable, and what cannot be ported because
> the two products want opposite things.

## 0. We already made vCAD's central decision, and I got this wrong first

vCAD's `Loader.h` says it plainly: **it never unloads.** Plugins are not
hot-reloadable, because reloading a library whose function pointers a live
document references means unmapping code the recompute is about to call.

My first read of this section said the engine cannot copy that, because
`KitHost::stop()` dlcloses at every simulation stop and hot-reload is the point of
a game engine's module story. **That was wrong, and the tree says so:**
`ModuleLibrary::unload()` calls `releaseContracts()` and the module's own destroy
and then **parks the handle in a process-lifetime graveyard — `libClose` is
defined and never called, on either platform.** The comment cites the Unreal
hot-reload model, and `tests/kit_lifecycle_test.cpp` records the crash that
motivated it: kits register flecs component hooks (ctor/dtor template
instantiations compiled INTO the kit), the world keeps those pointers, and
dlclosing made "unload a kit mid-play, resume, shoot" jump into unmapped memory.

So both projects reached the same decision from the same kind of crash, and the
engine gets hot-reload anyway by unmapping nothing. That is a better answer than
either "never unload" or "unload carefully".

**Which means two claims I made this week were wrong about their mechanism**, and
the record should say so rather than quietly improve:

| Change | What I claimed | What is actually true |
|---|---|---|
| `jobs::drainMain()` before `m_kits.stop()` | a queued callback's function pointer would jump into unmapped code | the code is never unmapped. The real defect is that those callbacks **never ran at all** — `pumpMain()` only runs in `tickSystems`, and the simulation has stopped. Dropped work, not a crash. The fix is right; the reason was not. |
| `elog::categoryCopied` | a kit's string literal dangles after `dlclose` | literals stay mapped forever too. The copy is defensible insurance — it stops the logger depending on the graveyard remaining policy — but it did not fix a live bug. |

Both are worth keeping as defence in depth: they make those subsystems correct
independently of one decision in `module_loader.h`. Neither was the
dangling-pointer bug I described.

## 1. The retention invariant, stated correctly

The rule I wrote first ("never retain anything a module owns") is too strong, and
being too strong made it useless — it condemns things the graveyard already makes
safe, so nobody would believe it. The real boundary is sharp and worth memorising:

> **Code and string literals from a module image are safe to retain forever** —
> `unload()` never unmaps them.
> **Anything the module ALLOCATED is not.** `unload()` calls the module's own
> destroy, so the table and everything reachable only through it dies there.

That is why `jobs::onMain(fn, user)` is safe in its `fn` and questionable in its
`user`: the function is code, the context may be a module `new`. And it is why the
`plugin()` shared_ptr rule below is the one that still has teeth.

The audit, against the corrected rule:

| Host retains | Owner | Status |
|---|---|---|
| `EngineContractDecl::name` in `contractRegistry()` | kit | **safe** — copied into `std::map<std::string,…>` and `std::vector<std::string>` |
| `KitStatus::message` / version strings | kit | **safe** — `std::string` |
| `IEnginePlugin*` in the plugin registry | kit | **safe** — detached before `dlclose` |
| `jobs::onMain` queue entries — the `fn` pointer | kit | safe (code, graveyard). See §0: what `drainMain()` actually fixed was work that never ran, not a crash |
| `elog::Category::name` | kit | safe either way (literal, graveyard). `categoryCopied` is insurance against the graveyard being revisited, not a bug fix — §0 |
| flecs component **hooks** (ctor/dtor/move) registered from a kit | kit | **safe** — and this is the bug the graveyard exists for. Found before I got here; `tests/kit_lifecycle_test.cpp` is the regression. The current kits' components are all trivial PODs so no hooks are installed today, but a kit adding a `std::vector` member would install them, and the graveyard already covers it. |
| flecs component **names** from a kit | kit | safe — flecs copies names into its own storage, and the literal is graveyard-mapped regardless |
| the module-owned `EngineGameModuleV1` table, via a `plugin()` shared_ptr | kit | **NOW CHECKED.** `m_destroy(m_table)` runs inside `unload()`, so an outstanding adapter reference becomes a live `shared_ptr` into freed module memory. The rule "release them all first" was a sentence in a comment; `unload()` now logs an error naming the outstanding count. |
| a `void* user` context handed to `jobs::onMain` | kit | safe *in ordering* — `drainMain()` runs before `m_kits.stop()`, so before the module's destroy — but this is the one place the allocated-memory half of the rule is reachable, and it is ordering that saves it, not the graveyard |
| profiler zone names (`TimerSample::name`) | host today | **latent** — `profiler.h` documents `name = address of the string literal (stable id; no hashing/interning)`. Correct today because kits cannot reach the profiler. If profiling is ever added to the primitive tier, a kit's literal becomes a map key held across frames, and the design's stability assumption quietly changes owner. |
| `ReflectedPending` blobs for unloaded kit components | host | safe by design — the data is copied, which is why a scene survives a missing kit |

The last row is worth noting for a different reason: `reflected_serde.h` +
`ReflectedPending` is **the same design as the CAD contract's §4A** (a document
must outlive the plugin that made it, unknown types preserved rather than
dropped), arrived at independently. What we do not have is CAD's specific test —
author with the kit, reopen *without* it, **save from the session that could not
understand it**, reopen with the kit, assert the bytes are unchanged. That save
step is the one that destroys a colleague's work, and it is the one an
implementation passes by accident or fails silently.

## 2. What we already have, and should stop re-deriving

Genuinely converged, so there is nothing to port:

- **`RTLD_NOW | RTLD_LOCAL`** — `module_loader.h:65`, matching the contract's
  §4.7 exactly. The dependency-conflict failure that damaged Revit and every
  large add-in ecosystem is already structurally impossible here.
- **Copy-and-dlopen**, because `dlopen` caches by inode.
- **Size/offset-frozen, additively-versioned tables** with a `>=` accept —
  `EngineApiTableV1`, per-group versions, `ENGINE_API_FROZEN` / `GROUP_AT` /
  `FIELD_AT`. This meets the contract's hardest bar: *"if a plugin author ever
  has to write `if (host_version >= X)`, we have failed."*
- **A cross-module contract gauntlet** pinning `{version, layout}` per component
  contract across kits. vCAD has no equivalent; two kits disagreeing about a
  shared component's layout is a failure mode we already refuse and they have not
  had to face yet.

## 3. Genuinely portable, in value order

### 3.1 A manifest gate that runs BEFORE `dlopen`

The contract's justification is exact and we do not honour it:

> `dlopen` runs static initialisers, and static initialisers are code execution —
> so without a manifest the host must EXECUTE a plugin in order to learn whether
> it should execute it.

We have a project manifest listing kits with a `requires` order, but the **API
version check happens after the library is mapped** (`module_loader.h:189`, "API
version %u != host"). An incompatible kit's static constructors have already run
by the time we refuse it.

The fix is small and the code is portable nearly as-is: `abi/src/Manifest.{h,cpp}`
is a 176-line `key = value` parser, deliberately not JSON because it is the first
thing touching untrusted bytes, with the library named as a **bare filename**
(no separators, no `..`, not absolute) resolved against the manifest's own
directory — otherwise installing a plugin becomes "load any library on this
machine". Add `abi`, `min_engine` and `caps` to our kit manifest entries and
refuse before mapping.

### 3.2 Forward refusal with a legible message

`CadPluginDesc.min_host_minor` — *the oldest host this plugin will run on* — so
the **host** refuses cleanly instead of the author writing runtime version
branches. Our client refuses an older host (`structSize <`), but only after being
loaded and bound, and we have no minor-version concept for the table at all. The
contract's reasoning for why this needs its own test is the part to take: loading
anyway means calling a function pointer the old host never populated, and *a null
crash inside third-party code is the hardest possible failure to attribute*.

### 3.3 A determinism check behind an environment variable

`CAD_PLUGIN_DETERMINISM_CHECK=1` runs every plugin compute **twice** and compares
by the same content hash the cache keys on — so agreement means the cache
genuinely cannot tell the runs apart. Off by default, an env var rather than a
build flag so an author can turn it on against a shipped build.

We have the identical exposure and no such check. Our cookers are pure functions
whose output is cached under `{cooker id, version, settings fingerprint}`, and
**the decimation determinism bug was found by reading code, not by a check**:
`(int32_t)floor(NaN)` yielded `INT_MIN` on x86-64 and `0` on arm64, so two
machines would have cooked different bytes and the DDC would have stopped being a
cache. An `ENGINE_COOK_DETERMINISM_CHECK=1` that cooks twice and compares digests
would have caught it without anyone looking, and will catch the next one.
Env-var-gated cook behaviour is already house style (`COOK_TEX_HQ`,
`ENGINE_SHADERC`).

### 3.4 A golden declaration snapshot, not just frozen offsets

`abi_golden.txt` pins 163 lines of declarations, verified red and green. Our
`api_abi_compat_test` pins group offsets and sizes — which I extended two turns
ago to cover field offsets — but **a signature change slips through**: swapping
two same-typed parameters, or retyping `uint32_t` to `int32_t`, moves nothing and
changes everything for a compiled module. A golden snapshot of the declarations
closes it and is the last of rule 1's four prohibitions we cannot currently catch.

### 3.5 Read-only by construction, for the provider ABIs

The CAD compute context has **no** mutation call — no document handle, no
transaction, no `set_param`. Determinism is enforced by the shape of the API
rather than by asking politely.

Our `IEnginePlugin::onPhysicsStep(flecs::world&, float)` hands a plugin **the
entire mutable world**. That is the opposite, and it is the direct argument for
the physics provider ABI in `provider-abi.md` being bulk arrays in and bulk
arrays out: what a provider cannot reach, it cannot corrupt, and no rule has to
be trusted.

### 3.6 Capabilities, declared and honestly labelled

`CAD_CAP_FILESYSTEM | NETWORK | SUBPROCESS | UI`, with the contract stating
plainly that **until sandboxing exists they are advisory** and must be described
that way — *"presenting an unenforced declaration as a permission grant would be
worse than showing nothing, because it invites a trust decision the software
cannot honour."* We have no capability concept. The declaration is cheap; the
honesty about enforcement is the part that matters.

### 3.7 External inputs as content digests, audited

The contract's `Import` bug: the cache key covered the file **path** rather than
its contents, so editing the referenced STEP file served the old shape. Our DDC
content-addresses cooked dependencies, but `sceneDependsOnNewerAssets` compares
**mtimes**, and mtime is not content. Worth an audit pass for any cooker keying on
a path where it should key on a digest — and the contract's second half is open on
their side too and on ours: nothing *notices* an external file changed.

## 4. What does not transfer

- **Never unloading** (§0).
- **Transactional document mutation.** Their isolation guarantee that survives an
  in-process crash is `txn_begin`/`txn_commit`. Our analogue is the scene plus the
  editor's undo stack, and a kit does not mutate scenes through anything
  comparable. Not a gap — a different shape.
- **`param_schema_version` vs `compute_version` as two fields.** Our cooker
  `kVersion` does both jobs, which is coarser (a format-only change re-cooks
  output unnecessarily) and cannot be wrong in the dangerous direction. Not worth
  splitting until a cook is expensive enough to care.
- **The extension catalogue** (features, PMI, GD&T, mates). Domain-specific.

## 5. Order

The audit is **done** and it found one thing, not three: a retention contract
enforced only by a comment, now checked. The flecs-hook hazard I expected was
already fixed by the graveyard before I looked, and the two bugs I thought were
instances of it were not.

What remains, in value order:

1. ~~**The CAD "save from a session without the plugin" test.**~~ **DONE** —
   `tests/reflected_pending_test.cpp`. The design held; the save had never been
   exercised. Mutation-proved by deleting the re-emission. It also settled a
   question the design did not answer: flecs emits floats with 10 significant
   digits, so the round-trip is exact and a document does not drift each time it
   passes through a machine missing a kit.

   The same pass corrected a **factually wrong comment** that would have misled
   the next reader: `unload()` claimed shipped games statically link kits and
   never reach the graveyard. `engine_build` ships real kit binaries, so
   `engine_player` reaches it exactly as the editor does — it was paying for the
   hot-reload copy-to-temp at every launch and leaving a temp file per kit on the
   player's disk. `Reload::Never` removes both; the graveyard itself is bounded by
   kit count in a shipped session and only grows if the game calls
   `kitLoad`/`kitUnload` itself.
2. **`ENGINE_COOK_DETERMINISM_CHECK=1`.** Cheap, and it retroactively covers a
   class of bug we have already shipped once (the decimator's
   architecture-dependent `(int32_t)floor(NaN)`, found by reading).
3. **Manifest gate before `dlopen`**, with `abi` / `min_engine` / `caps`. Port
   `Manifest.cpp`'s shape, including the bare-filename rule. Our API version check
   runs after the image is mapped, so an incompatible kit's static constructors
   have already executed.
4. **Golden declaration snapshot** beside the existing offset asserts — the one
   prohibition (a signature change at constant size) we still cannot catch.
5. Capabilities, labelled advisory.
6. External-input digest audit (`sceneDependsOnNewerAssets` compares mtimes).

And a note for whoever adds a profiling primitive to the kit ABI: read the
profiler row in §1 first.
