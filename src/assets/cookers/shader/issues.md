---
status: decided
---
# Issues — shader cooking (reported Sat Aug 1)

Every claim below was **re-checked against source** before acting. Two did not
hold up; the rest were real. Verdicts and resolutions follow the report.

## Resolved

| # | Claim | Verdict |
|---|---|---|
| A1.2 / A2.1 | Interface verified only on the first variant/profile | **TRUE — fixed** |
| — | *(found by that fix)* SPIR-V samplers reflect as `Unknown` | **REAL BUG — fixed** |
| A1.3 | `settingsFingerprint` ignores nested `.sh` includes | **TRUE — fixed** |
| A2.2 | Duplicate feature *defines* are legal | **TRUE — fixed** |
| A1.5 / A2.5 | Cross-stage uniforms warn twice | **TRUE — fixed** |
| A2.6 | Reflection doesn't validate uniform-name uniqueness | **TRUE — fixed** |
| A2.7 | `waitpid` failure leaves the output file behind | **TRUE — fixed** |
| A2.3 | Compile count not surfaced before work begins | **TRUE — fixed** |

### The one that mattered: verification ran once
`verified = true` after the first variant meant **2^n × profiles − 1** of the
compiled outputs were never checked. Now every variant on every profile is
reflected and verified.

This immediately paid for itself. On the very first run it failed the cook with:

```
declared interface does not match the compiled shader [spirv, feature mask 0x0]:
  sampler "baseColor" targets "s_baseColor", which is a unknown, not a sampler
```

**A genuine bug in our own reflector**, invisible while only Metal was checked.
bgfx's uniform type byte carries **four** flag bits — fragment `0x10`, sampler
`0x20`, read-only `0x40`, compare `0x80` (`bgfx_p.h:1598-1607`). We masked off
only `0x10`. SPIR-V writes samplers as `Sampler | kUniformSamplerBit`
(`shaderc_spirv.cpp:790`); Metal omits that bit. So on SPIR-V the masked value
came out `0x20`, larger than `Mat4`, and every sampler reflected as `Unknown`.

Any Vulkan build would have failed its interface check — or, before this
existed, bound samplers against a misread table. Regression-tested against the
exact SPIR-V byte encoding.

On the design question the report raised (forbid interface-changing defines, or
verify all variants): **both**. Verifying every variant *is* the enforcement —
a feature may add engine-driven uniforms, which only warn, but it may not remove
a declared material parameter. Conditional material parameters are now
mechanically impossible rather than discouraged by a comment.

### Nested `.sh` includes
Same hole as the `.sc` gap, one level down: shading code factored into a shared
header could be edited and nothing re-cooked. `hashSourceTree()` now walks
`#include "..."` transitively (visited set, depth cap 16) and hashes the whole
tree into the settings key. It scans rather than preprocesses, so an include
under a false `#if` is still hashed — over-approximating re-cooks when it needn't
have, which is the safe direction.

Verified: editing only `mylib.sh` re-cooks; an unchanged tree stays up to date.

## Rejected — claim does not hold

| # | Claim | Why not |
|---|---|---|
| A1.1 | `readDefaults()` leaves `defaults[3]` as "uninitialized stack garbage" | **FALSE.** `ShaderParam::defaults` has an NSDMI: `float defaults[4] = {0,0,0,0}` (`shader_asset.h:79`). The parser default-constructs `ShaderParam p;`, so unfilled components are a deterministic **0**, never garbage. A short array leaving the rest zero is documented behaviour, not a memory bug. |
| A1.6 | `size < 12` guard "despite reading a 14-byte header" | **Not a bug.** The byte count is right (12 + a `uint16` count = 14), but every read goes through a bounds-checked cursor, so a 12- or 13-byte blob returns `truncated uniform table` rather than reading past the end. `tests/shader_cooker_test.cpp` already asserts detection at every truncation offset from 12 up. Only the error *wording* differs. |
| A1.4 | Disjoint name sets allow parameter/sampler name collisions | **Not a bug.** They are separate namespaces by construction: `.material` has distinct `parameters` and `textures` objects, and `resolveMaterial` looks each up in its own list. A shared name is legal and unambiguous. |

## Accepted, deliberately not done

| # | Item | Why not now |
|---|---|---|
| A2.8 | No timeout around shaderc | **Already bounded in the shipping path.** Cooks run inside `engine_cook_worker`, which `cookInWorkerProcess` reaps against `COOK_TASK_TIMEOUT_SEC` and kills. A wedged shaderc dies with its worker. Only `COOK_INPROC=1` (a debugging mode) can hang, and that mode has no crash isolation either. A dedicated watchdog is worth adding when a farm exists. |
| A2.9a | Parallelize the variant matrix | **Couples to the memory governor.** `estimatePeakBytes()` reports *one* child's peak precisely because variants run sequentially; parallelizing inside a cook would silently blow past the admission budget the thermal work exists to enforce. Doing it properly means teaching the scheduler about sub-task parallelism — worth it when a shader has features (today the matrix is 1×3×2 = 6). |
| A2.9b | Pipes instead of a scratch file for diagnostics | Micro-optimization on a path that runs 6 times per shader. Real cost is the `posix_spawn` itself. |
| A2.4 | Hash map for reflection lookup | O(n) over ~10 uniforms, done once per variant at cook time. Measurably irrelevant; would trade clarity for nothing. |
| A2.10 | JSON source locations in diagnostics | A genuine tooling improvement, but it wants an editor surfacing it inline. Revisit with the editor work. |

## Status
All accepted fixes landed and are covered by `tests/shader_cooker_test.cpp`
(manifest rejections, duplicate uniforms, cross-stage warning dedup, the SPIR-V
flag-byte regression) plus real end-to-end cooks of `shaders/standard.shader`
across metal/spirv/glsl.
