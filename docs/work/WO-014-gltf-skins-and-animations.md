---
status: plan
id: WO-014
title: glTF skins and animations, supported for real
program: assets
priority: P2
size: M
state: done
done: 2026-09-30
evidence: frontend_cgltf_test (contract suite: all 8 cases, none skipped; spline; WO-002 inverted); cooker_test s2c inverted; 6 mutations red. The real-file check moved to WO-036
depends: [WO-012]
contracts: [import-frontend]
touches:
  - src/assets/importers/gltf_importer.cpp
source: review 2026-09-29 C2
---
## Why
This closes C2 properly. glTF is the format the engine says it owns, and today it can't carry a character.

## Done when
- [x] the cgltf front end fills skeleton, skin weights and clips; the contract suite's skinned cases pass (the skip is removed; all 8 cases pass, none skipped)
- [x] ~~a real skinned `.glb` cooks as v3 and animates in the editor~~ **moved to WO-036.** No skinned glTF exists in the tree (every `.glb`/`.gltf` checked has zero skins), so this needs a file from outside. Generated skinned glTFs cook as **v6** (the skinned format's real version) in `cooker_test` §2c.
- [x] the WO-002 refusal is removed, and its test inverts: the same fixtures, given real `JOINTS_0`/`WEIGHTS_0`, now cook as v6 with a bone, and with a clip when animated

## Contract
Nothing: an animation channel type the engine cannot play (for example morph weights) goes in the dropped list. The rest of the clip still cooks.

## Log
- 2026-09-30: The cgltf front end reads skins and clips. Following the glTF spec:
  - **bones** are every skin's joints plus their ancestors (a conversion above
    the armature is kept), with the rest pose from node transforms and
    `inverseBind` from the skin's own matrices
  - **a skinned mesh's node transform is ignored**; it hangs from a node whose
    transform is the skin's bind space
  - **weights**: `JOINTS_0` is remapped from the skin's joint list to bones;
    weights are normalised; a vertex with no influence follows the root bone;
    `JOINTS_1` is reported
  - **clips**: translation, rotation and scale channels on bones. Cubic-spline
    keeps its values and reports the dropped tangents; step is reported;
    morph weights and channels on non-bones are reported
  - an animation-only file becomes a clip-only scene, which the back end skips
    for WO-016

  The Mixamo clip-name rule is now shared by both front ends
  (`imp::clipDisplayName`).
- **The contract suite gained the check it had lost.** When WO-013 relaxed
  the `inverseBind` rule, nothing replaced it, and a front end that ignored
  inverse binds would have passed. Each skinned corner is now skinned at rest
  (weight × boneRestWorld × inverseBind × vertex) and must land where the scene
  places it. Both real front ends pass it; an ignored inverse bind fails it.
- Two bugs of mine, caught by the suite before anything else ran:
  - node 0, the synthetic root, had no entry in `gltfOf`, shifting every lookup
    by one and reading past the end
  - `meshes()` looped over nodes it was appending
- Mutations, each red:
  - joints not remapped
  - inverse binds not read (caught by the centimetre case)
  - a skinned mesh placed by its own node
  - clips not read
  - cubic-spline reading the in-tangent
  - ancestors dropped from the skeleton
- `gltf_losses.h` now describes only the runtime importer's uncooked preview,
  which WO-018 deletes.
- No skinned glTF exists in the tree, so the real-file check is WO-036.
