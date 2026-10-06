---
status: plan
covers:
  - docs/rhi/
---
# Study 001 — Bindless-only, or immutable binding sets?

| | |
|---|---|
| **Status** | `concluded` |
| **Opened** | 2026-09-04 (queued); worked 2026-10-05 |
| **Concluded** | 2026-10-05 (WO-056) |
| **Verdict lands in** | `../design-axioms.md` (axiom 2) |
| **Highest rung reached** | 3 (library documentation and headers, Khronos and LunarG validation documentation, Apple's WWDC25 session) |
| **Superseded by** | — |

## 1. The question

Should tier L keep axiom 2, bindless-only (a resource handle IS its shader
index, and there is no binding API), or offer immutable binding sets as NVRHI
and NRI do?

**Why it is expensive to get wrong.** It is the one axiom that cannot be
relaxed later: every shader and every resource type is shaped by whether the
handle is the index.

## 2. The falsifier

*Recorded honestly: this file was written after its reads, which overlapped
study 007's second pass on the same day. The expectation below is the queue
entry's, written 2026-09-04, before any of them.*

**Bindless-only is wrong if** either holds:
- one of the two tier-L APIs cannot express it as its normal model (it would
  be a bolt-on we would have to emulate);
- or its loss of validation leaves a class of bug no tool can catch, so we
  would ship index errors blind.

**Expected (queue entry, 2026-09-04):** keep axiom 2 and "accept the harder
validation story, knowingly".

## 3. Method

Reading (rung 3): NVRHI's programming guide; NRI's README; Metal 4 (WWDC25
205); Vulkan descriptor indexing and `VK_EXT_descriptor_heap`; the Vulkan
validation layers' GPU-assisted validation documentation.

## 4. What was found

**What the sources say:**
- **Both NVIDIA RHIs offer both models, and neither forces binding sets.**
  NVRHI: binding sets (fixed, pre-filled) and descriptor tables ("an untyped
  array of resource bindings… variable size… modified after creation", which
  "do not keep strong references"). NRI: descriptor pools/sets and "directly
  indexed descriptor heaps" (`NRIDescriptorHeap.h`).
- **Both tier-L APIs now treat bindless as the primary model.** Metal 4:
  `MTL4ArgumentTable`, where "in the bindless case, the argument table just
  needs one buffer binding". Vulkan: descriptor indexing is core since 1.2,
  and `VK_EXT_descriptor_heap` (Vulkan 1.4.340, 2026-01) removes descriptor
  sets and pipeline layouts outright, with descriptors found by heap offset.
- **The validation gap has a tool.** Vulkan's GPU-assisted validation
  instruments shaders and checks, at execution time, out-of-bounds indexing
  into descriptor arrays and use of unwritten descriptors (with descriptor
  indexing enabled), plus buffer-device-address accesses outside any known
  buffer. Its documentation states "significant shader performance
  degradation" and extra memory, and that it is opt-in.

**What we conclude (rung 6):**
- The first falsifier condition did not fire: bindless is each tier-L API's
  own direction, not a bolt-on. The binding-set model NVRHI and NRI keep
  serves D3D11 and older Vulkan, which tier L does not target.
- The second did not fire either, but only because of a split validation
  story: CPU-side checks in the RHI's validation device for what the CPU
  can know (a handle's generation, an index's range at the moment it is
  written, a resource's residency), and GPU-assisted validation for what
  only the GPU sees (the index a shader actually used). That is the "harder
  validation story" the queue entry expected, and it is now written down
  (`design-api.md` "Validation").

## 5. Verdict

**Keep axiom 2: tier L is bindless-only, with no binding API.**

- **Did the falsifier fire?** No, on either condition.
- **What changed:** axiom 2 cites this study and both tier-L paths (descriptor
  indexing as the floor, the descriptor heap where present). Open decision 7
  is answered. The validation split is in `design-api.md`.
- **Still unknown:** GPU-assisted validation's cost on our scenes (rung 1,
  needs the Vulkan CI lane, WO-058); and Metal's equivalent coverage, which
  the sources read here do not describe.

## 6. Sources (opened 2026-10-05)

- NVIDIA-RTX/NVRHI, `doc/ProgrammingGuide.md`, "binding sets", "descriptor tables".
- NVIDIA-RTX/NRI, README, descriptor model and `NRIDescriptorHeap.h`.
- Apple, WWDC25 session 205 "Discover Metal 4", argument tables.
- Khronos, `VK_EXT_descriptor_heap` reference page; Khronos blog, 2026-01-23.
- LunarG, "GPU Assisted Validation" (Vulkan SDK 1.4.304 documentation), descriptor indexing and buffer device address checks; and the Vulkan-ValidationLayers `docs/gpu_validation.md`, mechanism and cost.
