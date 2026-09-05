---
status: as-built
verified: 2026-09-05
covers:
  - src/render/
---
# How much of the engine would have to move

*Original `rhi-design.md` §2 and §2.1. Split out 2026-09-04; wording preserved.*

> Rung 2 on [`workflow.md`](workflow.md) §1 — every number here is a count a
> script could re-derive from the tree. `verified:` is the date they were taken.

## 2. The surface area we actually have to replace

This is the finding that makes the project tractable, and it is why the answer in
[`decision-record.md`](decision-record.md) is yes rather than no. Counted from the
tree:

- **~72 distinct `bgfx::` symbols, of which ~48 are functions.** Not a dozen
  APIs' worth of surface — one engine's worth.
- The whole list is unremarkable: create/destroy for buffers, textures, shaders,
  programs, uniforms, framebuffers; `setState`/`setTexture`/`setUniform`/
  `setVertexBuffer`/`setIndexBuffer`/`setTransform`/`submit`; views; `frame`;
  `getCaps`; `getStats`; transient and instance-data buffers.

## 2.1 The contamination, re-counted — and it is two problems, not one

**Corrected 2026-08-28.** This section previously read "57 files include bgfx, 30
of them outside `src/render/`" and treated that as one number. It is two, they
cost completely different things, and conflating them made the cleanup look four
times larger than it is.

| | files | of which |
|---|---|---|
| `#include <bgfx/…>` outside `src/render/` | **18** | 10 tests, 3 editor |
| **non-test, non-editor graphics coupling** | **5** | `asset_service.cpp`, `async_loader/upload.cpp`, `mesh_loader.cpp`, `gltf_importer.cpp`, `assimp_importer.cpp` |
| `#include <bx/…>` only — a **math** dependency | 18 | `core/math_types.h`, `core/transform.h`, `animation/pose.h`, `components/*` |

**The graphics problem is five files.** Every one of them mixes *loading* with
*GPU upload*, which is the same defect in five places and the one that has to be
fixed for a headless server, a second backend, or an embedding host — all three
want bytes handed over without a device in the room.

> **RESOLVED 2026-09-04 (G1a).** All five now go through `src/render/gpu.h`:
> opaque handles, an opaque `gpu::Blob` staging type, and five operations —
> stage, create vertex/index/texture, destroy. The count of non-test,
> non-editor files including a graphics API outside `src/render/` is **zero**,
> and `scripts/check_gpu_seam.py` fails the `unit` lane if that changes.
>
> Two things the work turned up that this section did not predict. **The
> coupling was not in the five files alone** — `Mesh`, `Texture`, `vertex.h`
> and `skinned_vertex.h` all named bgfx types, so any file that so much as
> declared a `Mesh` inherited the include; the five were the visible half.
> And **nine test files called `bgfx::init` by hand**, which the new device
> flag turned into a silent headless path until they were routed through
> `tests/gpu_test_device.h`.

**The bx problem is a different project and probably not urgent.** `bx` is
bgfx's base library, and 18 files depend on it for MATH — `Vec3`, `mtxMul`. That
survives bgfx's removal untouched if we want it to; it is a maths-library choice,
not a graphics dependency, and it should be decided on its own schedule rather
than being swept into an RHI migration.

`src/runtime/docs/bgfx-includes-in-runtime.md` still names `camera_util.h` as the
poster child. **That one is already fixed**: `homogeneousDepth` is a `bool`
parameter now, with the header comment recording it as audit A.2. `runtime.h`
remains real — `Renderer m_renderer` by value at line 253, pulling bgfx
transitively through `render/primitive_library.h`, and that is what blocks a
headless build.

**The cleanup has to happen whether or not we replace bgfx** — it is the same
refactor the headless dedicated server needs, and the same one an embedding host
needs. So G1 pays for itself even if the RHI is abandoned. That is the single
most important property of this plan: **its early phases are valuable
independently of its late phases.**
