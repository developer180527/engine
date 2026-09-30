## BUG-0070 — A rig over the bone limit was silently truncated, or cooked never to animate
- found:     2026-09-30
- status:    fixed
- class:     logic
- where:     src/assets/import/frontend_assimp_skeleton.h, src/assets/cookers/mesh/mesh_backend.cpp
- symptom:   two failures, both silent. An Assimp rig of more than 128 bones imported as a different, smaller rig: its weights on the dropped bones were lost, and it cooked broken. A glTF rig of 129 to 256 bones cooked, and then drew in its raw bind pose and never animated. The only trace of either was one line on stderr.
- cause:     the bone limit was stated six times and the copies disagreed. `extractSkeleton` (then in `src/animation/assimp_skeleton_loader.h`, moved into the Assimp front end by WO-015) truncated to `kMaxBones` (128) and `extractBoneWeights` skipped every influence on a bone past the 256th, while the cook back end allowed 256. The animator, whose GPU palette holds 128, then refused the rig and turned its palette off.
- pinned-by: tests/mesh_backend_test.cpp, tests/frontend_assimp_test.cpp, tests/frontend_cgltf_test.cpp, tests/render_pipeline_test.cpp
- lane:      unit
- proof:     `mesh_backend_test` cooks a rig of exactly 128 bones and refuses 129 naming the count and the limit; the front-end tests read a 300-bone rig whole with its top vertices on 'Bone299', and refuse a 65,546-joint glTF skin as Skin/Wrong; `render_pipeline_test` checks the skinning shaders' palette literal against the one constant. Each was red with its fix reverted: truncation back (128 bones read), the 255 cut-off back ('Bone299' lost), the back end at 256 (129 cooks), no uint16 check (the huge skin accepted). Fixed in `784efd4` (WO-040): one limit, `core/bone_limit.h`; the cook refuses by name (cooker v20); nothing truncates.
