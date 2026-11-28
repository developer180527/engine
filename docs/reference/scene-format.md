---
status: as-built
verified: 2026-11-28
covers:
  - src/assets/cookers/scene/scene_cooker.cpp
  - src/core/json_read.h
  - src/scene/entity_serializer.h
  - src/scene/component_schema.h
  - src/assets/cooked_scene.h
  - modules/assetlib/include/assetlib/scene_asset.h
---
# The `.scene` format

**The hand-editable source format for a scene, field by field, with what happens
when you get it wrong.**

A `.scene` file is JSON. It is the *source* format; `engine_cook` turns it into a
binary `.cooked` scene that the runtime loads. Both the editor and a person with a
text editor write these files, and the cooker cannot tell which — that is the
point.

**One reader.** Since WO-091 the editor and the shipped player read a scene the
same way: through the schema registry (`src/scene/entity_serializer.h`,
`registerBuiltinSchemas`). The cooked file carries the scene itself, so every rule
below holds in both. Before, the cooker had rules of its own, and two of them
differed from the editor's (a non-object component value, and a missing `id`).

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
>   cooking a scene full of deliberately bad values, loading it through the
>   player's own loader, and asserting the outcomes this document promises.
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

`entities` is the only key read (`version` is written, and is 1). Anything else in
the object is ignored, so comments-by-convention (`"_note": "..."`) are harmless.

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
| `id` | integer | a fresh one | This entity's stable identity. Needed for hierarchy and for references. |
| `parentId` | integer | `0` | The `id` of this entity's parent. `0` means **no parent**. |

Everything else is a **component**, keyed by name. Array elements that are not
objects are skipped silently.

### Hierarchy, and the trap in it

Parent links are resolved by matching `parentId` against another entity's `id`
**in the same file**. An unresolvable `parentId` is ignored — the entity simply
has no parent, with no diagnostic.

An entity that omits `id` (or gives one that is not a non-negative integer) is
given a **fresh** id at load, distinct from every other. So it cannot be anyone's
parent: a `parentId` can only name an id that was written. **Every entity you
intend to be a parent needs an explicit, non-zero `id`.** Two entities writing the
same `id` are both loaded; the second is given a fresh one, with a warning, and a
`parentId` naming that id links to the first. If you are not using hierarchy at
all, omit both keys.

Cycles and excessive depth are guarded at load, so a self-parenting or looping
file will not hang the runtime.

---

## What attaches a component

> **A component is attached when its key is present and its value is an object.**

```json
{ "name": "Defaults", "camera": {} }
{ "name": "Oops",     "camera": 5 }
```

The first has a camera with every field at its default: fov 60, near 0.1, far
1000, primary. The second has **no camera**: a value that is not an object is
skipped, and the load logs `malformed "camera" — skipped (not an object)`. The rest
of that entity still loads. The same applies to `"camera": null` and `"camera": []`.

*(Changed in WO-091. The cooker used to attach a default component for any value,
while the editor skipped it; the shipped game and the editor disagreed about the
same file. Now both skip it. `name` is the one component whose value is not an
object: it is the string.)*

`"transform": {}` is a valid way to say "identity transform, explicitly."

**A key nothing registered is kept.** A component from a provider or a newer
engine that this build does not know (`"sprite": {...}`) loads as opaque data and
is saved back unchanged, with a warning naming it. So is a component saved at a
newer revision than this engine reads (`"$rev"` in its object), rather than being
read as if it were the old one (DR-0034).

**A kit's component registered through the C API** (`engRegisterComponent`,
`eng_world.h`) is saved under its full name (`"apollo.Thruster": {...}`), one
key per field. An entity field is the target's stable id, like any other. A
**blob field** (`ENG_FIELD_BLOB`: variable-length bytes, such as a path's
points) is its bytes as a base64 string (`"points": "YWJjAP8="`), and `""` when
it holds none. Text that is not padded base64 keeps the field's default, which
is no bytes, and the rest of the component loads.

```json
{ "name": "Lander", "apollo.Route": { "points": "YWJjAP8=", "width": 1.5 } }
```

---

## How wrong values behave

Every field is read through its schema's field type (`src/scene/component_schema.h`,
built on `src/core/json_read.h`), and the contract is uniform and worth
internalising, because it means **a malformed scene degrades instead of
exploding**:

> A missing, wrong-typed, out-of-range, short, or non-finite value **leaves the
> destination alone**. You get the documented default, never a zero you did not
> ask for, and the rest of the component still loads.

Concretely:

| You wrote | You get | Why |
|---|---|---|
| `"fov": "71.5"` | `60.0` (the default) | a string is not a number; not an error |
| `"position": [1, 2]` | `(1, 2, 0)` | short arrays fill what they can; index 2 keeps its default |
| `"position": []` | `(0, 0, 0)` | nothing to read, defaults kept |
| `"position": 5` | `(0, 0, 0)` | not an array |
| `"castShadows": "true"` | `false` | **booleans must be real booleans** |
| `"castShadows": 1` | `false` | same — `1` is not `true` here |
| `"id": "3"` | a fresh id | must be a JSON integer |
| `"exposureMode": 300` | the default | an integer field takes only values that fit its type |
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

Defaults below are what you get when the key is absent. They are the schemas'
(`registerBuiltinSchemas` in `src/scene/entity_serializer.h`); each schema also
states them (`ComponentSchema::defaults`), for a tool that wants to read them
rather than this table.

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
| `asset` | string (UUID) | `""` |
| `clipIndex` | integer | `0` |
| `speed` | float | `1.0` |
| `fade` | float, seconds | `0.2` |
| `time` | float, seconds | `0.0` |
| `playing` | bool | `false` |
| `looping` | bool | `true` |

Clips are resolved at load from `path` (a standalone clip asset, bound to the
mesh's skeleton by bone name), or from `clipIndex` for a clip embedded in the
mesh. Note `playing` defaults to **false** — a skinned mesh with an animator that
nobody starts will T-pose, and that looks like a broken rig rather than a missing
flag. A skinned mesh with **no** `animator` key plays its first embedded clip.

### `skinnedMesh`

`{}`: a marker. The skeleton always comes from the entity's mesh; nothing about it
is saved.

### `lodMesh`

| Field | Type | Meaning |
|---|---|---|
| `levels` | array of mesh references (`cookedPath`, `asset`, `path`) | the coarser levels, finest first |
| `coarsenBelow` | array of floats | the screen size below which each level is used |

Written by the editor from a cooked LOD chain; rarely by hand.

### `colourGrading`

| Field | Type | Default |
|---|---|---|
| `exposureMode` | integer (0 manual, 1 physical) | `0` |
| `toneMapper` | integer (0 none, 1 PBR Neutral) | `1` |
| `exposureEV` | float, stops | `0.0` |
| `aperture` | float, f-number | `16.0` |
| `shutterSeconds` | float | `0.01` |
| `iso` | float | `100.0` |
| `lutPath` | string | `""` (none) |

An unknown `toneMapper` or `exposureMode` value is kept as written and refused
where it is used, so a scene from a newer engine round-trips.

### `reflected`

A kit's components, by the kit's own name: `{"combat::Health": {"current": 40}}`.
Read once the kit has registered the component (kits load at Play); until then
kept and saved back unchanged.

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

### `spinner`

A constant turn, applied every simulation step: yaw about the entity's local Y,
then pitch about its local X. Loaded and saved since 2026-11-28; before that a
scene's `spinner` was kept as unknown data and the entity did not turn.

| Field | Type | Default |
|---|---|---|
| `speedYaw` | float, radians per second | `0.0` |
| `speedPitch` | float, radians per second | `0.0` |

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
| hierarchy is flat or wrong | the parent's `id` omitted, so no `parentId` can name it |
| a component is missing, with a warning | its value was not an object |
| mesh missing, `Cannot stat` at runtime | a hand-written or stale `cookedPath` |
| material silently wrong | `material` name does not match any authored `.cmat` |
| skinned mesh T-poses | `animator.playing` defaults to `false` |
| nothing in the scene at all | `entities` is present but not an array |

The cook log is the first place to look — it reports the entity count and size
per scene (`Cooked N entities → main.cooked`); the load log names every component
it skipped or kept unread. An entity count lower than you wrote means array
elements were skipped for not being objects.

---

## The cooked side

For completeness, since a tool may want to read the output rather than the input:
`modules/assetlib/include/assetlib/scene_asset.h` defines the container —
currently **version 4**, a 32-byte header (magic, version, payload encoding,
payload size) and a payload. The payload is the scene itself, as above, encoded as
CBOR, with each mesh reference's `cookedPath` healed against the asset database.
`assetlib::loadScene` reads the container; the player reads the payload in place
(`src/assets/cbor_reader.h`), and `src/assets/cooked_scene.h` decodes it into JSON
(refusing CBOR nested deeper than 64 levels) for the packager, which ships the meshes
a scene names, and for a payload the in-place reader does not take.
The tools write the payload **packed** (encoding 2, `src/assets/packed_scene.h`):
registered components as their schemas' fields, everything else as CBOR. A cook
with no packer writes CBOR (encoding 1). Both load, by the same per-field rules:
a packed float that is not finite keeps its default, as a JSON one does.

**The cooked bytes are the same on every platform.** `scene_portability_test`
compares a cook of `every_component.disk.json` with two committed golden files,
byte for byte, on every CI leg, and loads them through every reader. A change
that needs the golden files regenerated is a change to the format: bump the
packed layout or the scene version, and re-cook.

Version 3 and earlier were a fixed 256-byte record per entity with slots for nine
components; colour grading, kits' components and unknown ones did not survive the
cook. They are refused at load with "re-cook the scene": a cooked scene is derived
data, and the `.scene` is the truth.
