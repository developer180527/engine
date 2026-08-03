---
status: as-built
tier: working
verified: 2026-08-03
covers:
  - src/assets/cookers/material/
  - modules/assetlib/src/material_asset.cpp
tests:
  - tests/material_cooker_test.cpp
parses-external-input: true
---
# Material cooking

## Purpose
Make a material **a shader reference plus values**, instead of a fixed C++
struct. This is the payoff of the declared interface built in
`src/assets/cookers/shader/`, and the second half of Phase 5 /
finding R3 in `docs/plans/renderer-audit-and-plan.md`.

[`material.h`](../../../render/material.h) hardcodes five fields, so the set of
things a material can express is compiled into the engine. A `.material` names
a `.shader` and supplies values for the parameters **that shader declared** — so
a project defines its own look without touching engine source.

## Names are resolved at COOK time
The cooker reads the shader's declared interface, checks every parameter and
sampler name, validates arity, fills defaults, and emits finished float blocks.
The runtime therefore does **no name lookup and no validation** — it uploads
bytes. A misspelled parameter is a failed cook, not a silent no-op at 60 Hz:

```
Cook FAILED: assets/materials/rust.material — material does not match shader "standard":
  parameter "roughtness" is not declared by the shader (declared: baseColorFactor, roughness, metallic)
  texture "emissive" is not a sampler declared by the shader (declared: baseColor, normalMap)
```

## Blocks are complete, never sparse
Uniform blocks are built from the **shader's** parameter list, not from what the
material happened to set, and unset parameters carry the shader's declared
defaults. A sparse block would leave whatever the previous draw wrote in the
gaps — the classic "looks right alone, wrong next to another object" bug, which
is miserable to diagnose because each material is individually fine. Same rule
for samplers: every declared sampler emits a binding, so an unset stage gets its
fallback rather than the last texture bound there.

## Why it reads the .shader SOURCE, not the cooked .cshader
The interface is **authored** in the manifest; compiling only *verifies* it, and
`ShaderCooker` already does that once against the real uniform table. Reading
the manifest means a material has no cook-order dependency on its shader, which
avoids inventing a deferral mechanism the pipeline doesn't have (`ICooker` has
no "my input isn't ready yet" result, and registry dependency edges only exist
after a first successful cook has recorded them).

The cost, stated plainly: a material can cook successfully against a shader
whose own cook failed. That surfaces at load, and the shader's failure is
already loud.

## Files — one concern each

| File | Concern |
|---|---|
| `material_manifest.h/.cpp` | Parse a `.material`. No shader lookup, no validation. |
| `material_resolve.h/.cpp` | Bind authored values to a declared interface. Pure. |
| `material_cooker.cpp` | `ICooker`: find the shader, sequence, write. |
| `assetlib/material_asset.h` | The `.cmat` container format. |

## Authoring

```json
{
  "shader": "shaders/standard.shader",
  "parameters": { "roughness": 0.85, "baseColorFactor": [0.72, 0.25, 0.10, 1.0] },
  "textures":   { "baseColor": "textures/rust_albedo.png" },
  "features":   [],
  "doubleSided": false
}
```

A single number splats across a vector (`"baseColorFactor": 0.5` = mid grey) —
that is real authoring intent. Any other arity mismatch fails, because silently
truncating three values into a float is never what someone meant.

The shader reference is resolved against, in order: the material's own
directory, the project root, then `<project>/assets`. All three are listed in
the error when it isn't found.

## Known limitations
- **Nothing loads `.cmat` yet.** `MaterialRegistry` still holds the fixed
  `Material` struct and `ForwardPipeline` still binds from it. The shader half
  of Phase 5 is live (`src/render/shader/info.md`); this is the remaining half,
  and it is what finally deletes the hardcoded fields.
- **Texture paths are not resolved to UUIDs.** `CookContext` exposes
  `addDependency(UUID)` but no registry lookup, so the cooked material carries
  the authored path and the runtime resolves it through `AssetService`. This
  also means a material does not currently declare a dependency edge on its
  textures. (The `.shader` manifest IS declared — through
  `declaredInputs()`, which takes paths rather than UUIDs. Textures could be
  declared the same way; they are not yet, because a material's cooked bytes do
  not depend on a texture's CONTENT, only on the path it names.)
- **No material instancing.** Every `.material` is standalone. Unreal's
  Material/Material-Instance split exists to avoid recompiling shaders per
  material — here materials never trigger compilation at all, so the split buys
  nothing yet. Revisit if shared-parameter-block editing becomes a real workflow.
- **`.mat` is still mapped to `AssetType::Material`** for legacy scans but has
  no cooker; `.material` is the cookable unit.

## Tier evidence (`working`)
- `tests/material_cooker_test.cpp` — hermetic: manifest validation, resolution
  rules, default-filling, feature masks, and the container including every
  truncation offset.
- End-to-end verified by really cooking a `.material` against
  `shaders/standard.shader`: `u_colorFactor` = the authored colour, `u_params` =
  `[0, 0.85, 0, 0]` (roughness authored, metallic defaulted), `s_normalMap`
  bound to its `flatNormal` fallback.
- **Mutation-proved on a real cook**: `roughtness` + an `emissive` texture fail
  the cook and list the declared names.

`parses-external-input: true` — a `.material` is project-authored JSON. Reaching
`hardened` requires a FUZZ lane over the manifest parser and `loadMaterial`.
