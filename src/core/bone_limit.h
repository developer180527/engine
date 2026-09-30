#pragma once
// ── bone_limit — how many bones one skinned mesh may have, stated ONCE ────────
//
// The limit is the GPU's: the skinning shaders hold the palette in a uniform
// array of kMaxBones mat4s (shaders/vs_skinned.sc, vs_shadow_skinned.sc:
// u_boneMatrices[kMaxBones * 4] vec4s), which render_pipeline_test checks
// against this constant. Everything else takes it from here: the skeleton, the
// palette pool, SkinnedMesh, the animator, and the cook back end, which REFUSES
// a rig over it rather than cooking one the runtime cannot skin.
//
// It used to be stated six times, and the copies disagreed: the back end
// allowed 256, so a 200-bone glTF rig cooked, and the animator then refused it
// and it drew in its raw bind pose, never animating, with no message (WO-040).
//
// No dependencies: SkinnedMesh is a simulation-facing component.
inline constexpr int kMaxBones = 128;
static_assert(kMaxBones <= 256, "a cooked vertex stores joint indices as uint8");
