---
status: as-built
verified: 2026-08-27
covers:
  - src/assets/cookers/scene/scene_cooker.cpp
  - src/core/json_read.h
  - modules/assetlib/include/assetlib/scene_asset.h
---
# The `.scene` format

**The hand-editable source format for a scene, field by field, with what happens
when you get it wrong.**

A `.scene` file is JSON. It is the *source* format; `engine_cook` turns it into a
binary `.cooked` scene that the runtime loads. Both the editor and a person with a
text editor write these files, and the cooker cannot tell which — that is the
point.

This document exists because [`tool-ecosystem.md`](../architecture/tool-ecosystem.md)
§4 claims a complete game can be built with the SDK, the CLI tools and a text
editor. That claim rests entirely on this format being writable by hand, and until
now it was undocumented — the only way to learn it was to read the cooker.

> **How this file is kept honest.** Two mechanisms, and it is worth knowing which
> does what, because neither does the other's job:
>
> - **`status: as-built`** puts it under the docs gate's staleness check, so when
>   the code in `covers:` changes after `verified:`, CI flags this file. That
>   prompts a human to re-read it. It does **not** check whether the prose is
>   true.
> - **[`tests/sdk_only_game_test.cpp`](../../tests/sdk_only_game_test.cpp)** pins
>   the surprising claims below — the ones about what a *wrong* value does — by
>   cooking a scene full of deliberately bad values and asserting the outcomes
>   this document promises.
>
> The second exists because writing this file turned up a claim that was wrong
> (see "the one that is not a silent default"). A schema reference nobody checks
> is worse than none, because people trust it.

---

## Where scenes live, and where they go

The cooker scans two directories, both optional, and cooks every file ending
`.scene`:

```
<project>/scenes/
<project>/<assetRoot>/scenes/      # assetRoot comes from project.json, usually "assets"
```

Output goes to `<project>/.cache/scenes/<stem>.cooked`. So
`assets/scenes/main.scene` becomes `.cache/scenes/main.cooked`.

`project.json`'s `lastScene` names the scene to open, as a project-relative path:

```json
{ "lastScene": "assets/scenes/main.scene" }
```

The player uses only its **stem**: it loads
`.cache/scenes/<stem-of-lastScene>.cooked`. So `lastScene` may point at the source
`.scene` — which is what you want, since that is the file you edit — and the
directory part is not used to find the cooked output. Two scenes with the same
filename in different directories therefore collide in `.cache/scenes/`; give
scenes distinct filenames.

Nothing else in the directory matters. Subdirectories are **not** scanned — the
walk is one level deep per directory.

---

## The top level

```json
{
  "entities": [ ... ]
}
```

`entities` is the only key read. Anything else in the object is ignored, so
comments-by-convention (`"_note": "..."`) are harmless.

If `entities` is missing, or present but not an array, the file cooks to an
**empty scene** rather than failing. That is deliberate: an empty, inspectable
result is a more honest answer for a file that declares no usable entities than a
crash, and `scene.value("entities", array())` would have *thrown* on a non-array,
taking down a background cook thread.

A file that is not valid JSON at all **fails to cook, and says so by name**:

```
[SceneCook] Parse error in main.scene: [json.exception.parse_error.101]
            parse error at line 1, column 18: syntax error while parsing value
[CookService] Scene cook failed: main.scene
```

No output is produced for that scene. Other scenes still cook — one broken file
does not abort the pass — so check the log rather than the exit of the whole cook.
Note the asymmetry with the case above it: **malformed JSON is loud; a
well-formed file that declares nothing usable is silent**, because the second one
has an honest empty answer and the first does not.

---

## An entity

```json
{
  "id": 1,
  "parentId": 0,
  "name": "Player",
  "transform": { "position": [0, 1, 0] },
  "camera": { "fov": 71.5 }
}
```

| Key | Type | Default | Meaning |
|---|---|---|---|
| `id` | integer | `0` | This entity's identity *within this file*. Only needed for hierarchy. |
| `parentId` | integer | `0` | The `id` of this entity's parent. `0` means **no parent**. |

Everything else is a **component**, keyed by name. Array elements that are not
objects are skipped silently.

### Hierarchy, and the trap in it

Parent links are resolved by matching `parentId` against another entity's `id`
**in the same file**. An unresolvable `parentId` is ignored — the entity simply
has no parent, with no diagnostic.

Because `id` defaults to `0`, **every entity you intend to parent or be a parent
of needs an explicit, non-zero `id`.** Omit them and every entity shares id `0`,
the parent lookup collapses to a single entry, and the hierarchy comes out wrong
in a way nothing reports. If you are not using hierarchy at all, omit both keys.

Cycles and excessive depth are guarded at load, so a self-parenting or looping
file will not hang the runtime.

---

## The rule that surprises everyone

> **A component is attached because its KEY IS PRESENT — not because its value
> makes sense.**

```json
{ "name": "Oops", "camera": 5 }
```

That entity **has a camera**, with every field at its default: fov 60, near 0.1,
far 1000, primary. The cooker sets the component bit on
`je.contains("camera")`, then reads fields out of the value; a non-object value
yields no fields, so all of them keep their defaults.

The same applies to `"camera": {}`, `"camera": null` and `"camera": []`. If you
do not want a component, **remove the key** — do not set it to null.

This cuts both ways and is occasionally what you want: `"transform": {}` is a
valid way to say "identity transform, explicitly."

---

## How wrong values behave

The cooker reads every field through `src/core/json_read.h`, and its contract is
uniform and worth internalising, because it means **a malformed scene degrades
instead of exploding**:

> A missing, wrong-typed, short, or non-finite value **leaves the destination
> alone**. You get the documented default, never a zero you did not ask for.

Concretely:

| You wrote | You get | Why |
|---|---|---|
| `"fov": "71.5"` | `60.0` (the default) | a string is not a number; not an error |
| `"position": [1, 2]` | `(1, 2, 0)` | short arrays fill what they can; index 2 keeps its default |
| `"position": []` | `(0, 0, 0)` | nothing to read, defaults kept |
| `"position": 5` | `(0, 0, 0)` | not an array |
| `"castShadows": "true"` | `false` | **booleans must be real booleans** |
| `"castShadows": 1` | `false` | same — `1` is not `true` here |
| `"id": "3"` | `0` | must be a JSON integer |
| `"fov": 1e999` | **the whole scene fails to cook** | see below — this one is not a default |

### The one that is not a silent default

`"fov": 1e999` does **not** give you 60.0. nlohmann's *parser* rejects a number it
cannot represent, so the file never becomes JSON at all:

```
[SceneCook] Parse error in main.scene: [json.exception.out_of_range.406]
            number overflow parsing '1e999'
[CookService] Scene cook failed: main.scene
```

No `.cooked` file is produced for that scene, and `engine_cook` says so by name.
Other scenes in the project still cook.

This is worth stating because `json_read.h` documents a non-finite guard —
`finiteNumber` rejects anything `!std::isfinite` — and it would be reasonable to
assume a huge literal therefore lands on the default. It does not, for a `.scene`
file: JSON has no `NaN` or `Infinity` literal, and an out-of-range exponent throws
during parsing, before any field is read. That guard is real defence-in-depth for
JSON built *in process* (the undo stack builds JSON in memory, where an infinity
can arrive without a parser in the way) — but a file cannot reach it.

### Two that bite hardest

**Booleans are strict.** `is_boolean()` only. `"true"` and `1` are rejected
rather than reinterpreted, so a quoted boolean silently means "default". If a
flag isn't taking effect, check its quotes first.

**Short float arrays keep defaults, they do not zero.** A two-element `scale` of
`[2, 2]` gives `(2, 2, 1)` — not `(2, 2, 0)`, which would flatten the object to a
plane and be far harder to recognise as a typo.

There is one exception to "wrong type means default": `name`. A non-string `name`
becomes the literal string `"Entity"`.

---

## Enums are integers, not strings

There are no string enums anywhere in this format. Writing `"type": "point"`
gives you the default, silently.

| Field | 0 | 1 | 2 |
|---|---|---|---|
| `camera.projection` | perspective | orthographic | — |
| `rigidBody.bodyType` | static | **kinematic** *(default)* | dynamic |
| `rigidBody.shape` | box | sphere | capsule |
| `light.type` | directional | point | spot |

Note `rigidBody.bodyType` defaults to **1, kinematic** — not dynamic. A body you
expected to fall will sit still until you say `"bodyType": 2`.

---

## Components

Defaults below are what you get when the key is absent, and are transcribed from
`scene_cooker.cpp`.

### `transform`

| Field | Type | Default |
|---|---|---|
| `position` | 3 floats | `[0, 0, 0]` |
| `rotation` | 4 floats, quaternion `(x, y, z, w)` | `[0, 0, 0, 1]` |
| `scale` | 3 floats | `[1, 1, 1]` |

Rotation is a **quaternion, not Euler angles**, and the identity is
`[0, 0, 0, 1]` — a hand-written `[0, 0, 0, 0]` is not a rotation and will not
behave.

### `name`

Not an object — the value *is* the string.

```json
{ "name": "Player" }
```

Names are interned and deduplicated in the cooked string table, so repeating one
across ten thousand entities costs one copy.

### `meshRenderer`

| Field | Type | Default | Meaning |
|---|---|---|---|
| `path` | string | `""` | project-relative source mesh path |
| `sourcePath` | string | — | legacy absolute path; read only if `path` is empty |
| `asset` | string (UUID) | `""` | asset identity; **survives renames**, so prefer it |
| `sourceType` | string | `""` | `"primitive"` selects a built-in shape; anything else means a file |
| `material` | string | `""` | material by **authored name**, not by path |
| `cookedPath` | string | `""` | **do not write this by hand** — see below |

**Resolution order:** the asset database wins. If `asset` or `path` resolves to a
`Ready` record, that record's cooked path is used and any `cookedPath` in the
JSON is overwritten (the cook logs `Healed stale cooked ref`). Only if resolution
fails is the JSON's `cookedPath` used as a last resort.

That ordering exists because `cookedPath` is a snapshot of a *previous registry
generation*. Clone the repo, regenerate `registry.db`, or cook on another machine,
and UUIDs are minted afresh — the remembered `meshs/<old-uuid>.cooked` then points
at a file that does not exist. Trusting it produced scenes that cooked
"successfully" and loaded **nothing**: every mesh reported `Cannot stat` and the
level came up empty.

So when hand-authoring: give `path` (and `asset` if you know the UUID), and leave
`cookedPath` out entirely.

**Built-in primitives** need no asset at all. Set `sourceType` to `"primitive"`
and give the shape's name as the `path`, in the canonical form the engine writes:

```json
"meshRenderer": { "sourceType": "primitive", "path": "engine://primitive/cube" }
```

Three names exist: **`cube`**, **`plane`**, **`sphere`**. The runtime takes
whatever follows the last `/`, so a bare `"path": "cube"` also works — but the
`engine://` form is the one the registry resolver explicitly skips, which is what
keeps a primitive from being looked up as a missing file. Prefer it.

Note the primitive is a **fallback**: it is used only when no cooked mesh loaded.
A `path` that resolves to a real mesh wins over a `sourceType` of `"primitive"`.

**Materials are referenced by name**, which means they cannot be resolved by path
the way meshes are. A scene-scoped cook uses the name to pull the right `.cmat`
into the closure; get the name wrong and the material silently does not exist at
runtime, and you see the mesh's baked material with no indication anything is
missing.

### `camera`

| Field | Type | Default |
|---|---|---|
| `isPrimary` | bool | `true` |
| `projection` | integer | `0` (perspective) |
| `fov` | float, degrees | `60.0` |
| `orthoSize` | float | `10.0` |
| `nearPlane` | float | `0.1` |
| `farPlane` | float | `1000.0` |
| `clearColor` | 4 floats RGBA | `[0.1, 0.1, 0.12, 1.0]` |

### `rigidBody`

| Field | Type | Default |
|---|---|---|
| `bodyType` | integer | `1` (kinematic) |
| `shape` | integer | `0` (box) |
| `mass` | float | `1.0` |
| `restitution` | float | `0.3` |
| `friction` | float | `0.6` |
| `useGravity` | bool | `true` |
| `halfExtent` | 3 floats (box) | `[0.5, 0.5, 0.5]` |
| `radius` | float (sphere/capsule) | `0.5` |
| `halfHeight` | float (capsule) | `0.5` |

### `script`

| Field | Type | Default |
|---|---|---|
| `path` | string | `""` |

### `animator`

| Field | Type | Default |
|---|---|---|
| `path` | string | `""` |
| `clipIndex` | integer | `0` |
| `speed` | float | `1.0` |
| `fade` | float, seconds | `0.2` |
| `playing` | bool | `false` |
| `looping` | bool | `true` |

Clips are resolved at runtime from `path`, or from `clipIndex` for a clip embedded
in the mesh. Note `playing` defaults to **false** — a skinned mesh with an
animator that nobody starts will T-pose, and that looks like a broken rig rather
than a missing flag.

### `characterController`

| Field | Type | Default |
|---|---|---|
| `radius` | float | `0.3` |
| `height` | float | `1.8` |
| `maxSlopeDeg` | float | `45.0` |
| `stepHeight` | float | `0.3` |
| `mass` | float | `70.0` |
| `gravityScale` | float | `1.0` |

### `light`

| Field | Type | Default |
|---|---|---|
| `type` | integer | `0` (directional) |
| `color` | 3 floats RGB | `[1, 1, 1]` |
| `intensity` | float | `3.0` |
| `range` | float | `15.0` |
| `spotInner` | float, degrees | `25.0` |
| `spotOuter` | float, degrees | `35.0` |
| `castShadows` | bool | `false` |
| `useTemperature` | bool | `false` |
| `temperatureK` | float, kelvin | `6500.0` |

---

## A complete worked example

Hand-written, no editor involved. `assets/scenes/main.scene`:

```json
{
  "entities": [
    {
      "id": 1,
      "name": "Camera",
      "transform": { "position": [0, 2, 8] },
      "camera": { "isPrimary": true, "fov": 65, "nearPlane": 0.1, "farPlane": 500 }
    },
    {
      "id": 2,
      "name": "Sun",
      "transform": { "rotation": [-0.35, 0.35, 0.15, 0.85] },
      "light": { "type": 0, "intensity": 4.5, "castShadows": true }
    },
    {
      "id": 3,
      "name": "Ground",
      "transform": { "position": [0, 0, 0], "scale": [20, 1, 20] },
      "meshRenderer": {
        "sourceType": "primitive",
        "path": "engine://primitive/cube",
        "material": "ground"
      },
      "rigidBody": { "bodyType": 0, "shape": 0, "halfExtent": [10, 0.5, 10] }
    },
    {
      "id": 4,
      "parentId": 3,
      "name": "Crate",
      "transform": { "position": [0, 2, 0] },
      "meshRenderer": { "path": "assets/meshes/crate.glb", "material": "wood" },
      "rigidBody": { "bodyType": 2, "mass": 12.0, "useGravity": true }
    }
  ]
}
```

Then, with no editor anywhere in the loop:

```bash
engine_cook . && engine_player .
```

A smaller version of this scene is exercised by
[`tests/sdk_only_game_test.cpp`](../../tests/sdk_only_game_test.cpp), which writes
a `.scene` by hand, cooks it, and asserts the values survive into the binary the
runtime loads. That test is what stops this format drifting away from hand
authoring — every other `.scene` in the tree came out of the editor, so nothing
else would notice.

---

## What the cooker will not tell you

A successful cook is not a valid scene. These all cook cleanly and produce
nothing visible:

| Symptom | Likely cause |
|---|---|
| entity exists, component does nothing | value wrong-typed, so every field took its default |
| a `true` flag has no effect | it was quoted: `"true"` is not `true` |
| an enum behaves as the first option | it was a string: `"point"` is not `1` |
| a rigid body will not fall | `bodyType` defaults to kinematic; dynamic is `2` |
| hierarchy is flat or wrong | `id` omitted, so every entity is id `0` |
| mesh missing, `Cannot stat` at runtime | a hand-written or stale `cookedPath` |
| material silently wrong | `material` name does not match any authored `.cmat` |
| skinned mesh T-poses | `animator.playing` defaults to `false` |
| nothing in the scene at all | `entities` is present but not an array |

The cook log is the first place to look — it reports the entity count and string
table size per scene (`Cooked N entities → main.cooked`). An entity count lower
than you wrote means array elements were skipped for not being objects.

---

## The cooked side

For completeness, since a tool may want to read the output rather than the input:
`modules/assetlib/include/assetlib/scene_asset.h` defines the binary format —
currently **version 3**, a 32-byte header, fixed 256-byte entity records, and a
packed string table. `assetlib::loadScene` reads it; component presence lives in a
`componentMask` bitfield rather than in the record's contents, which is why the
"key present" rule above shows up on both sides of the cook.

Both the entity record size and the header size are pinned by `static_assert`, so
that format cannot change shape without a build failure.
