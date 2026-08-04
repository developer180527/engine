---
status: unreviewed
---
# Issues — `src/render/world` audit, triaged (Tue Aug 4)

Two external assessments, 10 claims. Every one was **verified before being acted
on**, because this repo has a history of high-rated findings that measured as nil.
Four were real, four were wrong, two were already-known gaps restated.

The verification is the durable part of this file: the tests written to settle the
claims are permanent (`tests/render_world_test.cpp`), so the same questions do not
have to be re-argued from reading.

| # | Claim | Verdict | Action |
|---|---|---|---|
| A1.1 | `worldSphere` under-estimates the radius | **REAL, narrower than stated** | fixed |
| A1.2 | `packLights` writes an out-of-bounds shadow index | **FALSE** | already tested |
| A1.3 / A2.1 | Euclidean distance is not view depth; ignores `boundsCenter` | **REAL, no impact today** | fixed |
| A1.4 | Transparency not implemented | **TRUE, already documented** | recorded |
| A1.5 | Parallel cull false sharing / `std::function` cost | **self-refuted / nil** | declined |
| A1.6 | Key field widths silently truncate | **REAL** | asserts added; half open |
| A2.2 | `extractFrustumPlanes` row/column-major mismatch | **FALSE** | test added |
| A2.3 / A2.P4 | Dynamic `maxDist` wastes depth precision | **REAL, latent** | open |
| A2.P2 | Packing `VisibleDraw` to 12 bytes is faster | **FALSE — measured slower** | declined |
| A2.P3 | Fixed `kCullGrain` leaves cores idle | **REAL** | fixed |
| A2.P1 | SoA / SIMD frustum culling | **REAL (layout), nil (SIMD)** | streams done; NEON measured and declined |

---

## A1.1 — `worldSphere` under-estimated the radius ✅ FIXED

**Claim:** the radius uses the max basis-row length where it needs the max singular
value, so a rotated non-uniform scale gets too small a sphere and visible geometry is
culled. Offered counterexample: `R(45°) · diag(2,1,1)`, max singular value 2 vs max
row length ~1.58.

**Verified by transforming all 8 local AABB corners and asking whether the sphere
contains them.** The mechanism is right; the stated scope is wrong:

| case | worst corner overrun |
|---|---|
| rotated, non-uniform SRT from `Transform::getMatrix` | **0.000000** |
| 45° child under a parent scaled 4× on X | **0.347** |

The first result is not luck. `getMatrix` builds `diag(scale) · R` with `R`
orthonormal, so its rows have length exactly `s_i` — the row lengths *are* the
singular values, and no single transform in this engine can trigger the bug. The
counterexample given is rotation-then-world-axis-scale, which the component model
never produces. It takes a **hierarchy** — a rotated child under a non-uniformly
scaled parent — for the composed linear part to stop being `diag · orthonormal`.

Real regardless: an under-estimated sphere culls geometry that is genuinely visible,
which shows up as objects popping out of existence near the screen edge and never as
a crash.

**Fixed by bounding the box instead of the matrix.** Under `p' = p·M` the world
half-extent along axis *j* is `Σᵢ hᵢ·|M[i][j]|` — the standard absolute-value AABB
transform — and the containing sphere's radius is that extent's length. Conservative
by construction, still one `sqrt`, and *tighter* than the old form for non-uniform
scale, which multiplied the whole local diagonal by the largest axis.

Because it is tighter, **cull counts legitimately changed** — 20 000 objects
10 945 → 11 025 culled, 50 000 31 012 → 31 128. Counter values in older notes are not
comparable across this fix.

## A1.2 — shadow index out of bounds — **FALSE**

**Claim:** when a shadow caster appears after the buffer is full,
`out.shadowLightIndex = out.count` is assigned with `count == kMaxLights`, so the
shader indexes past the array.

The loop is `for (i = 0; i < lights.size() && out.count < kMaxLights; ++i)`. The
assignment is *inside* that loop, so it cannot execute with `count == kMaxLights`; a
caster past the cap is never visited and the index stays `-1`. The review quotes the
assignment without its loop guard.

`render_world_test` already asserted exactly this ("a shadow caster dropped at the cap
does not leave a stale index"), which is why this one needed a reading rather than an
experiment.

## A1.3 / A2.1 — Euclidean distance is not view depth ✅ FIXED

**Both sub-claims are factually correct.** `sort_key.h` has always documented
`depth01` as "normalized [0,1] view depth", and the code measured straight-line
distance from the eye — spherical shells, so two objects at equal depth get different
keys as one moves off-axis. It also measured to the transform ORIGIN, not the bounds
centre, so a mesh pivoted at its base sorted by its pivot while being culled by its
bounds.

**Impact today: nil**, and worth saying plainly rather than dressing the fix up. The
opaque key layout puts material and mesh *above* depth, so depth only breaks ties
inside one material+mesh group — and a group that batches is instanced as a unit,
where order does not matter. The claim's own note is right: it matters when
transparency lands, because that layout is depth-dominant.

Fixed anyway, because the correct version is **cheaper**: view-space z is the third
column of `view` (row-vector convention), so three multiply-adds replace three
subtractions, three multiplies and a `sqrt`. Taken as a magnitude, so a caller's
handedness convention cannot silently collapse every depth to zero. The bounds centre
is already computed for the cull, so using it is free.

Submit counters are unchanged by this — it moves depth ordering *within* groups, not
cull decisions or batching.

## A1.4 — transparency not implemented — **TRUE, and already documented**

`buildVisibleSet` always passes `BlendClass::Opaque`, and the line that does says so:
*"Blend class is not yet carried by RenderItem — every draw is opaque today (the
pipeline has no transparent path wired). Stated here rather than hidden, so the sort
key is honest about what it knows."*

Not a defect; a wired-up-when-needed gap. `blendOf`/`materialOf`/`meshOf` are not
"unused" — they are how the tests read the ordering policy back, and `depthOf` was
added to join them for exactly that purpose (see A2.3).

Real work when transparency lands: `RenderItem` needs a blend class from its material,
A1.3's view-depth fix is a prerequisite, and the split key (see A2.P1) needs a
transparent-layout variant, since that layout puts depth above material.

## A1.5 — false sharing / `std::function` overhead — **nil**

The claim refutes itself ("no synchronisation is needed. This is correct"). On the
`std::function` point: the dispatcher is invoked **once per cull phase**, not per item
or per range, and it is built once in `onAttach` rather than per frame specifically to
keep an allocation out of the hot path. A raw function pointer would save one indirect
call per frame and cost the ability to inject a hostile dispatcher in tests — which is
what proves the parallel path correct. Declined.

## A1.6 — key field widths truncate silently ⚠️ PARTLY FIXED

**Real, and worse than the claim says.** Material ids get 16 bits, mesh ids 21, and
both are registry slot indices. The claim stops at "duplicate keys and sorting
errors". The actual consequence is `sameBatch`, which compares those very bits: two
*different* materials that alias in 16 bits are judged the same batch and collapse
into **one instanced submit with one material bound** — the wrong one. Confidently
wrong rendering, no diagnostic.

Debug assertions added in `makeSortKey`, with the consequence spelled out. They
immediately earned their place: `render_pipeline_test` was using the Mesh **pointer**
as a mesh key — distinct per mesh, so it looked fine, and it blew the 21-bit field.
Fixed to small sequential ids, like the real ones.

**Open half:** a release build still truncates. Refusing an over-large id belongs at
registry-add time, where it can fail loudly, not in a hot-path header.

## A2.2 — row/column-major mismatch in `extractFrustumPlanes` — **FALSE**

**Claim:** the header documents row-major, the extraction indexes column-major, so
planes come out transposed and culling is broken.

Under row-vector convention `clip.x = Σₖ pₖ·vp[4k+0]`, so the left plane
(`clip.x + clip.w > 0`) needs coefficients `vp[0]+vp[3], vp[4]+vp[7], vp[8]+vp[11],
vp[12]+vp[15]` — exactly what the code writes. Indices 0,4,8,12 **are** column 0 in
row-major storage; that is the requirement, not the bug.

Settled empirically, because reading alone could not: every frustum test used
hand-built planes, so nothing covered the extraction end to end. That gap is now
closed — a real `mtxLookAt`/`mtxProj` pair and 8 assertions, including the one a
transposed extraction could not reproduce: an offset that is outside near the camera
and inside further away, because a perspective frustum widens with distance. All pass.

## A2.3 / A2.P4 — dynamic `maxDist` wastes depth precision — **REAL, latent, OPEN**

Normalising against the single farthest survivor means one distant object compresses
everything else. The claim's example is right: a skybox at 10 000 m with gameplay
geometry inside 50 m leaves 99% of draws in the bottom 0.5% of the 24-bit field.

Latent for the same reason as A1.3 — depth is only a tiebreaker under the opaque
layout — and it becomes real with transparency, where quantisation collisions mean
draw-order flicker.

**Open.** The suggested fix (derive the range from the projection's near/far) is not
obviously better: a 0.1–1000 m projection spends most of the 24-bit field on empty
space, which is the problem the dynamic normaliser was written to avoid. The
defensible version is a percentile or a log distribution, and that wants a real scene
with transparency to tune against, not a stress grid of identical cubes.

What DID come out of this: `depthOf()` in `sort_key.h`, and an absolute assertion that
at most one draw may sit at full-scale depth. That is what catches an under-estimated
normaliser — a comparison against a serial baseline cannot, because both paths share
the reduction. A mutation that dropped the reduction previously failed **zero**
assertions; it now fails with 4 554 of 5 999 draws saturated.

## A2.P2 — pack `VisibleDraw` to 12 bytes — **FALSE, measured slower**

**Claim:** `#pragma pack` to 12 bytes gives 5.3 elements per cache line instead of 4
and cuts radix-pass traffic by 25%.

Measured directly — 19 000 draws, realistic key distribution (constant high bytes,
varying 24-bit depth), 200 sorts each:

| layout | per sort |
|---|---|
| 16 bytes (current) | **0.0841 ms** |
| 12 bytes, packed | 0.0885 ms |

**5% slower.** The 8-byte key becomes unaligned and the stride stops being a power of
two, which costs more in address arithmetic and unaligned loads than the saved
bandwidth returns — and the elements-per-line figure does not apply anyway, since
radix scatters writes across 256 buckets rather than streaming them. Declined.

## A2.P3 — fixed `kCullGrain` leaves cores idle ✅ FIXED

**Real.** 2048 items per range meant a 5 000-item scene produced three ranges, so this
12-core machine left nine cores idle in the cull. Grain now targets ~4 ranges per
hardware thread with a 512 floor, so the range count tracks the machine at both ends.

`hardware_concurrency` is a hint, not the pool size: `rworld` deliberately does not
know about the job facade (see `ParallelForFn`), so it cannot ask how many workers
exist. Guessing wrong changes only the range count, never the result — which the
scheduling-independence tests assert.

## A2.P1 — SoA cull streams ✅ DONE (SIMD deliberately not yet)

Sound direction. `RenderItem` is **144 bytes** and the cull touched offsets 0..141 —
all three of its cache lines — to recover a bounding sphere and two ids.

**One correction to the sketch:** it is SSE (`_mm_load_ps`, `_mm_cmplt_ps`). This
engine's primary target is arm64 — every measurement in these notes is from an
M-series Mac — where those intrinsics do not exist. It needs NEON, or a portable
4-wide wrapper with NEON and SSE behind it. Worth deciding deliberately, since the
Windows/x86 port is a tracked task.

**What landed:** `CullStreams` — parallel `x/y/z/r/keyBase` arrays, 24 bytes per item,
filled by extraction while the model matrix is still in registers. The radius carries
two sentinels so the hot loop needs no side table (`r < 0` not renderable, `r ==
infinity` renderable but never culled). The sort key is split: extraction packs
material+mesh, the cull ORs in depth, and the two forms are asserted equal over
randomised inputs — a one-bit disagreement would reorder draws and break batching
while producing perfectly plausible keys.

### The result is smaller than the byte-count arithmetic implies

Three-run medians:

| | before | after |
|---|---|---|
| 50 k `Render.shadow` | 0.25 ms | **0.09** |
| 50 k `Render.cull` | 0.94 | **0.83** |
| 50 k `Render.extract` | 2.69 | 2.89 |
| 50 k `Render` total | 4.14 | **4.06** |
| 100 k `Render.shadow` | 0.48 | **0.15** |
| 100 k `Render.cull` | 1.62 | **1.34** |
| 100 k `Render.extract` | 5.37 | 5.54 |
| 100 k `Render` total | 7.80 | **7.42** |

**Net ~2% at 50 k, ~5% at 100 k.** Extraction absorbed most of what the cull and
shadow passes gave up, because the work MOVED rather than disappeared — extraction now
computes the sphere and writes five streams. What genuinely vanished is the
duplication: **the world bounding sphere does not depend on the camera**, yet it was
being rebuilt once for the camera frustum and again for the light's. That is the whole
of the shadow pass's 0.25 → 0.09.

Both phases were already parallel across 12 cores, so the bandwidth being saved was
already spread. That is the honest explanation for a modest total, and it was
predictable from A1.1's neighbourhood: cutting `worldSphere` from four square roots to
one was worth **4%**, which is what a loop waiting on memory looks like.

What it DID buy, and the reason to keep it: the cull now reads four contiguous float
arrays, which is the precondition for a 4-wide test. Doing SIMD first, against the
144-byte AoS layout, would have meant deinterleaving four items out of three cache
lines each before any arithmetic — the layout, not the instruction set, was the thing
in the way.

### Both follow-ups resolved by measurement — and the answer reversed the design

A standalone benchmark over 100 000 spheres at the stress scene's ~65% cull ratio, with
the four combinations of layout and instruction set (survivor counts identical in all
four, so they are the same computation):

| variant | per 100 k | vs scalar |
|---|---|---|
| scalar, separate arrays, early-exit | 0.1465 ms | — |
| **NEON, separate arrays** | 0.0636 | **2.31×** |
| NEON, interleaved + `vld4q_f32` | 0.0738 | 1.98× |
| scalar, interleaved | 0.1156 | 1.27× |

**1. NEON: DECLINED, and the arithmetic is the reason.** The scalar plane test costs
1.47 ns per item. The cull's real in-engine cost is ~33 ns per item (0.07 ms per
2 083-item range). So the plane arithmetic is **~4% of the phase**, and a 2.31× win on
4% is ~2% of the cull — 0.4% of the render path, roughly 0.03 ms at 100 000 objects.
That does not justify intrinsics, a portable NEON/SSE wrapper, or the determinism
problem FMA introduces (fused multiply-add rounds differently from mul-then-add, so a
SIMD path could flip a borderline cull decision and break the byte-identical-counter
evidence every other test here leans on).

Recorded so it is not re-attempted on intuition. If it is revisited, `vld4q_f32`
deinterleaves the current layout in one instruction, so nothing is blocked.

*A caution about that table*: the working set is 1.6 MB and therefore cache-resident,
so it measures ALU throughput, not the streaming behaviour of a real frame. It is
trustworthy for "the arithmetic is not the cost" and untrustworthy for anything else.
The first version of this benchmark also reported the scalar variant at 0.0012 ms —
12 picoseconds per item — because the whole call was hoisted out of the repetition
loop. Perturbing the planes per repetition and consuming every result fixed it.

**2. Interleaving: ADOPTED, reversing the original choice.** The reason for four
parallel arrays was that a 4-wide test wants four centres loaded contiguously. With
SIMD declined, that premise is gone, and what is left is stream count: four arrays are
four prefetch streams and four TLB entries, and the scalar benchmark showed one
16-byte read beating them by 1.27× on identical data. In-engine, three-run medians:

| | separate arrays | interleaved |
|---|---|---|
| 50 k `Render.cull` | 0.83 ms | **0.78** |
| 50 k `Render` total | 4.06 | **3.91** |
| 100 k `Render.cull` | 1.34 | 1.37 (flat) |
| 100 k `Render` total | 7.42 | **7.29** |

~5% on the cull at 50 k, nothing measurable at 100 k, ~2–4% on the render path. Small,
but it also **halves the stream count** (two, not five) and drops extraction's
compaction from five `memmove`s to two — so it is simpler as well as no slower, which
is the tiebreaker.

The layout now follows the memory, not the instruction set. That is the sentence worth
keeping from this whole exercise.

## A3 — `RenderItem` was 144 bytes, two of them write-only ✅ FIXED

Not from the external audit; found while looking for the next lever after the cull
stopped reading `RenderItem` at all.

**`const Material* mat` and `const Texture* tex` were WRITE-ONLY.** Extraction
resolved both handles and stored the pointers; nothing ever read them, because
`ForwardPipeline::bindMaterial` re-resolves through `ctx.materials` /`ctx.textures`.
So they cost 16 bytes per item plus two registry lookups per item per frame, for
nothing — and they were a second source of truth for an item's material, which is a
stale-cache bug waiting for the first code that mutates a material mid-frame.

That pair of lookups is exactly what an earlier differential measured at ~0.17 ms per
20 000 items and filed under "measured NOT to matter, so nobody spends a day on
them". The conclusion was right and the reason was wrong: they were not a small cost
worth keeping, they were a cost buying nothing.

Removed, and the remainder repacked — matrix and pointers first, the extraction-only
block (bounds, keys, handle) after, small scalars grouped instead of each padding out
to 8. **144 → 128 bytes: exactly two cache lines**, for a struct the submit path reads
by RANDOM index, since draws are visited in sorted order.

| `Render.extract`, 3-run medians | 144 B | 128 B |
|---|---|---|
| 50 000 objects | 2.866 ms | 2.859 (flat) |
| 100 000 objects | 5.518 | **5.342** (−3.2%) |

### Why 3% and not 15% — the thing to know before the next micro-optimisation

The old differential said those two lookups were 0.17 ms per 20 000 items, which
would predict ~0.85 ms saved at 100 000. The actual saving is 0.18 ms. The difference
is not measurement error: **that differential ran against SERIAL extraction.**
Extraction is now `jobs::parallelFor` across 12 cores, so a per-item cost divides by
the core count before it reaches wall time. 0.17 ms of serial work is ~0.014 ms of
frame time.

Which reframes every remaining idea in this file: once a phase is parallel, per-item
savings are worth roughly `1/cores` of what they look like, and the cheap wins are
gone. What is left in extraction is the ECS iteration and the writes themselves, both
already parallel — so the next real lever is not a smaller item or fewer
instructions, it is **not extracting at all** for things that did not change
(persistent/incremental extraction keyed off flecs change detection). That is a
design change, not an optimisation, and it should not be started without first
measuring how much of a typical frame's item set is actually static.

Bounds and keys stay on `RenderItem` deliberately: `rworld::writeCullEntry` takes a
`RenderItem`, so the item is the input the streams are derived from. Splitting the
submit-only fields from the stream-source fields would shrink it further, at the cost
of two structures to keep parallel — not obviously worth it while the cold block is
never touched by submission anyway.

---

## Where the cull stands after this pass

Three-run medians, shadows on, same machine:

| objects | `Render.cull` |
|---|---|
| 5 000 | 0.34 ms |
| 20 000 | 0.52 |
| 50 000 | 0.78 |
| 100 000 | 1.37 |

A1.1 and A1.3 were **not** made for speed and were not isolated for timing; no speed
claim is made for either.
