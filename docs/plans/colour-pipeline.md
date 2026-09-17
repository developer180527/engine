---
status: target
---
# Colour pipeline

Design and staged plan, written 2026-09-11. **Stages A and B are built
(2026-09-17); C and D are not.** §1 is the measurement stage A was designed
against and is kept as the record of what was wrong; §3a and §4a say what shipped
and where it differs from the design.

Evidence is graded on the ladder in `docs/rhi/workflow.md` §1. Each claim below is
marked **[measured]** (read in this tree, with a path), **[vendor]** (the owner's
own documentation, linked in §9), or **[recalled]** (prior art from memory — must
be checked before it is relied on). Rungs 5–6 cannot close a decision alone.

---

## 0. The one-paragraph version

The engine does not have a missing colour pipeline so much as a **wrong** one:
textures authored in sRGB are sampled as if linear, lighting is computed on those
values, and the result is written unencoded into an 8-bit target and shown as if
it were sRGB. The two errors partly cancel for flat-lit content — which is why the
image looks plausible — but every lighting operation between them runs in an
undefined space, and anything brighter than 1.0 clips. Stage A fixes that without
adding a single feature. Stages B–C add the HDR chain, exposure, tone mapping and
HDR display output that `docs/plans/aaa-gap-analysis.md` lists as absent. Both
target platforms can do HDR output **without patching bgfx** (§5).

---

## 1. What the engine did before stage A [measured 2026-09-11]

| Stage | What happens | Where |
|---|---|---|
| Texture storage | No colour-space field. `TextureHeader` v2 has `_pad[4]` spare. | `modules/assetlib/include/assetlib/texture_asset.h:130` |
| Texture cook | Knows `isNormalMap` when choosing a format; records nothing about colour space. | `src/assets/cookers/texture/texture_target.h:60` |
| Texture upload | Never passes `BGFX_TEXTURE_SRGB`. Base-colour texels reach the shader **still sRGB-encoded**. | `grep BGFX_TEXTURE_SRGB src/ modules/` → no hits |
| Shading | PBR (GGX, Schlick) treats the sampled value as **linear** reflectance. | `shaders/fs_triangle.sc` |
| Light colour | `kelvinToRGB` returns "approx sRGB [0,1]" (its own comment) and is multiplied straight into the **linear** light colour. | `src/components/light.h`, `src/render/renderer/extract.cpp:331` |
| Scene target | `RGBA8` UNORM. No headroom: anything > 1.0 clips. | `src/render/renderer/targets.cpp:43`, `:65` |
| Output | `gl_FragColor = vec4(ambient + direct, a)` — linear, **unencoded**. No post pass. | `shaders/fs_triangle.sc`, `renderToBackbuffer` in `targets.cpp` |
| Swapchain | `BGFX_RESET_VSYNC` only — no sRGB backbuffer. | `src/render/renderer/device.cpp:119`, `:185` |
| Editor display | `ImGui::Image` of the raw scene colour texture. | `src/editor/editor_app.h:458`, `panels/game_view_panel.h:110` |
| Exposure / tone map / grading | None. | `grep -i "exposure\|tonemap" src/render` → no hits |

**Size of the error.** An sRGB-encoded mid-grey of 0.5 is linear ≈ 0.214, so the
shader currently uses **~2.3× too much diffuse reflectance** at mid-grey, with the
ratio growing toward the darks. For an ambient-lit pixel the display's sRGB decode
undoes the texture's encoding and the colour looks right; for anything that is
*lit* — N·L falloff, two lights summed, specular, Fresnel — the maths sits between
two mismatched encodings and is wrong. Light falloff reads too harsh and additive
lighting does not look additive. This is the classic gamma-incorrect renderer.

It is also invisible to every existing test: correctness of colour is presentation,
nothing hashes it, and no GPU image comparison exists.

---

## 2. Principles

1. **Scene-referred linear from sample to tone map; display-referred only at the
   very end.** Every stage in between operates on linear light.
2. **One declared working space.** Linear, Rec.709/sRGB primaries — what all
   current content is authored in, and Unreal's default [vendor]. ACEScg (wide
   gamut) is a later *option*, not a starting point (§7).
3. **Every texture declares its encoding at cook time.** Colour textures are sRGB;
   data textures (normal, roughness, metallic, AO, masks) are linear and are never
   converted.
4. **Decode in hardware.** sRGB texture formats decode on sample for free; no
   per-pixel `pow` in shaders.
5. **An HDR intermediate is the precondition for everything else.**
6. **Exposure is explicit and physical** (EV100), before anything automatic.
7. **Tone mapping is one swappable "display transform" stage, parameterised by
   the output device** — the HGIG principle [vendor]: the game adapts its tone
   mapper to what the display reports, rather than handing the display values it
   will clip or re-map.
8. **The engine owns output encoding**, per target: SDR sRGB, Windows scRGB or
   HDR10, Apple EDR.
9. **Colour management is data, not a runtime dependency.** OpenColorIO is an
   authoring/offline tool that *bakes* LUTs; the game runtime executes a small fixed
   shader plus an optional 3D LUT. OCIO itself is linked only by tools (and, later,
   the film product — `future-plans/film-tool.md`).

Colour is presentation end to end: the simulation, `simhash::hashWorld` and every
`SimState` component are untouched by all of it.

---

## 3. Stage A — correctness (no new features)

Fixes §1. Changes every scene's appearance, deliberately.

**A1. Texture colour space in the cooked format.** Use one of `TextureHeader`'s
spare pad bytes as `colourSpace`, bump the version 2 → 3. `0` means *legacy /
linear* — exactly today's behaviour — so a v2 file loads unchanged. Change the
texture cooker's `settingsFingerprint` so the DDC re-cooks every texture with an
explicit value, which is the intended migration path.

**A2. The cooker assigns it from usage.** Base colour and emissive → sRGB;
everything else → linear. The cooker already receives `isNormalMap` at the point
of format choice; generalise that flag into a usage enum from the same source.

**A3. Upload with `BGFX_TEXTURE_SRGB`** when `colourSpace == sRGB`, gated per
format on `BGFX_CAPS_FORMAT_TEXTURE_2D_SRGB` [measured — `defines.h:491`]. The
Metal backend maps sRGB twins for every colour format the cooker emits — BC1, BC3,
BC7, ETC2, ETC2A, ASTC 4×4 / 6×6, RGBA8 [measured — `renderer_mtl.cpp:236–307`].
BC5 has no twin and needs none: it only ever carries normal maps.

**A4. HDR scene target.** `RGBA8` → `RGBA16F` for the scene and game colour
targets — renderable with MSAA on Metal [measured — `renderer_mtl.cpp:474`].
Cost: 8 bytes/px instead of 4 [measured — bgfx format table, `bgfx.h:251`]:
≈ 3.7 MB → 7.4 MB per colour target at 1280×720. `RG11B10F` (4 bytes, no alpha)
is the measured optimisation to try *afterwards* if alpha proves unused; bgfx's
Metal backend reports it renderable (`framebuffer = true`, MSAA too) [measured —
the per-format caps switch, `renderer_mtl.cpp:520–525`]. Check
`BGFX_CAPS_FORMAT_TEXTURE_FRAMEBUFFER` at runtime on every other backend. Note that
render-target memory is already ~5× the naive figure for reasons still unexplained
(`docs/plans/renderer-audit-and-plan.md` R6) — measure before and after.

**A5. One output pass.** A fullscreen pass from the HDR scene target to the
display target, which in Stage A only encodes linear → sRGB. Prefer the hardware
encode (`BGFX_RESET_SRGB_BACKBUFFER`; Metal selects `BGRA8Unorm_sRGB` for it
[measured — `renderer_mtl.cpp:3849`]), with a shader encode as fallback. This pass
is where every later stage attaches, and it is the natural first client of the P4
render graph ("shadow + opaque + one post pass through the graph" —
`docs/plans/renderer-program.md`). Build it hand-wired now, shaped to migrate.

**A6. The editor shows the encoded image.** `ImGui::Image` of an `RGBA16F`
linear target would display raw linear values. The scene and game-view panels
must show the **output of the display transform**, not the HDR scene target.

**A7. Kelvin in linear.** Apply the sRGB decode to `kelvinToRGB`'s result before
it is multiplied into the light colour. (A CIE-based blackbody table in linear
Rec.709 is the better long-term replacement; the decode is the minimum fix.)

**Verification.** The GPU path cannot be image-tested today — CI renders headless
through bgfx Noop. So:

- **CPU reference implementations** of every transform in one header
  (`srgbToLinear`, `linearToSrgb`, later the tone mappers and PQ), unit-tested:
  round-trip within a stated tolerance; `linearToSrgb(0.214) ≈ 0.5`; the
  piecewise segment at 0.0031308. **Mutation: swapping encode and decode must
  redden the test.**
- **A mirror check** that the shader and CPU versions agree, by compiling the same
  expressions from a shared source or by pinning constants in both.
- **A colour-checker reference scene** (24-patch chart under one white directional
  light) for manual before/after review, until a GPU CI leg makes golden images
  possible.

---

## 3a. Stage A as built (2026-09-17)

Everything in §3 shipped. Where the implementation departed from the design, this
is why:

| §3 said | Built | Why |
|---|---|---|
| A2: generalise `isNormalMap` into a usage enum | `encodeTexture` sets `colourSpace` from the existing flag | The encoder already knew: it chose the format and the mip filter from that flag. Recording it there keeps the three from ever disagreeing, and covers every cooker at once |
| A3: upload with `BGFX_TEXTURE_SRGB` | `gpu::ColourSpace` made a **required** argument | The bug was a silent default. Requiring it surfaced two upload paths the design had not counted — the async streaming stage and the editor async loader — at compile time |
| A5: prefer the hardware encode (sRGB backbuffer) | Shader encode into an `RGBA8` display target | The editor shows the scene through `ImGui::Image`; sampling an sRGB texture decodes it back to linear, so a hardware-encoded target would display dark |
| A5: one output pass | Renderer-owned `OutputPass`, views 190–192 | Encoding for the display is the renderer's contract with the screen, not a pipeline's look; a swapped pipeline gets it free |
| (not in §3) | Clear colours decoded, through bgfx's float palette | Packed 8-bit clears quantise dark greys: the editor's 0.102 displayed as 0.110 |
| (not in §3) | Debug-line colours decoded in `fs_line.sc` | Authored sRGB; left encoded they washed out |
| (not in §3) | The cooker's private sRGB curve replaced by `core/colour.h` | A third copy of the curve, feeding mips the GPU decodes with the reference one |
| (not in §3) | `colour.sh` made a build dependency of every fragment shader | bgfx tracks only the `.sc` and `varying.def.sc`; an edited include left shaders stale |

**Verification.** `colour_test` (the curve, its continuity, every 8-bit round trip,
and the shader's constants read from `colour.sh`); `cooker_test` §2b (colour space
recorded, legacy by version below v3, unknown refused, both fingerprints name the
format version); the Rust `cooked_format` reader asserts byte 28 of a real cook.
Each was mutation-checked. On Metal, `engine_host fps_shooter --frames 240` ran with
no asserts; its stale cache loaded as legacy with the warning, re-cooked on its own
through the fingerprint, and then uploaded base colour `sRGB[x]` and the normal map
`sRGB[ ]`.

**Not verified:** the rendered image. There is still no readback or golden-image
test, so "the picture is now correct" rests on the maths being tested and the GPU
flags being observed — not on comparing pixels. Look at a scene in the editor.

**Limits carried forward:**
- A standalone texture's role comes from the filename heuristic. A missed normal
  map cooks as sRGB BC7 — its format is already wrong in that case, so no new
  failure mode.
- Material colour *factors* (`baseColorFactor`, `Light::color`) are passed through
  as authored. glTF defines its factors as linear; whether the editor's colour
  pickers are sRGB-perceived is undecided.
- Highlights above 1.0 clip after lighting. That is stage B's job.
- `allocView` must stay below 190. Asserted in `RenderContext::allocView` since
  review, with the number shared as `RenderContext::kFirstOutputView` so the
  renderer's output views and the assert cannot drift apart.
- Clear colours use bgfx's float palette only for views 0–15. Review found the
  first version keyed the slot `viewId % 16`: the palette is one per frame, so
  view 17 would have taken view 1's colour. Higher views now clear with a rounded
  packed colour (`render/clear_colour.h`, `colour_test` §5).
- A dead `createCookedTexture` in `render/cooked_texture.h` had been given its
  own sRGB rule, diverging from `cookedColourSpace` (no legacy warning, no
  no-sRGB-variant fallback). It had no callers; review deleted it.
- Cost: +8 B/px per view — a 1280x720 scene target set is 14.1 MB.

## 4. Stage B — exposure, tone mapping, grading (SDR output)

**B1. Exposure.** Physical first: EV100 from aperture, shutter and ISO — the same
parameters the film tool's camera needs. Scale scene radiance by `1 / (1.2 ·
2^EV100)` [recalled — the Frostbite/Lagarde convention; verify before use].
Auto-exposure from a luminance histogram comes after, and only once a compute path
is confirmed on every backend.

**B2. The tone mapper is a swappable stage.** Three candidates, with evidence:

| Option | Character | Evidence |
|---|---|---|
| **Khronos PBR Neutral** | Preserves authored base colour, hue and saturation under grey light; analytically invertible; ~13 lines of GLSL. Built for "eCommerce, architecture and CAD". Adopted by Blender, Filament, three.js, Babylon.js, `<model-viewer>`, Autodesk, Dassault. | [vendor] Khronos, May 2024 |
| **AgX** | Filmic; bright saturated colours roll off toward white as a real camera does, avoiding Filmic's "Notorious Six" hue collapse. Blender 4.0's default. | [vendor] Blender 4.0 release notes |
| **ACES-style filmic** | Industry familiarity; Unreal's filmic tonemapper is ACES-matched. Known for hue skews and desaturation under saturated light, which is what motivated AgX. Common game form is Narkowicz's analytic fit. | [vendor] Epic docs; [recalled] the fit |

**Recommendation: Khronos PBR Neutral as the engine default, AgX as the shipped
"filmic" alternative.** A neutral default is the one artists can grade *from*; a
filmic look is a creative choice, which is how Unreal treats it too (a
post-process setting). PBR Neutral is also the obviously right default for a CAD
viewer — its stated audience — so it serves vCAD's needs directly. Its analytic
inverse is useful for UI and for any pass that must undo the tone map. Each tone
mapper gets a CPU reference and tests: monotonic, bounded, mid-grey placement,
and for PBR Neutral, `inverse(forward(x)) ≈ x`.

**B3. Grading as a 3D LUT.** A small volume texture (32³ is typical [recalled]),
sampled after tone mapping, authored offline — DaVinci Resolve or OCIO export a
`.cube`, the cooker converts it. OCIO's GPU API has a "legacy" mode that bakes a
processor to at most one 3D LUT [vendor], which is exactly the artefact the runtime
wants — so OCIO stays a *tool*, and the runtime stays a texture fetch.

---

## 4a. Stage B as built (2026-09-17)

Exposure, a swappable tone mapper and 3D-LUT grading, applied by the renderer's
output pass in the order §2 requires: **exposure → tone map → sRGB encode →
grade**. A camera carries them in a new `ColourGrading` component; the primary
camera's grading drives the game view and the standalone player.

| §4 said | Built | Why |
|---|---|---|
| B1: exposure by EV100, `1/(1.2·2^EV100)` [recalled] | Both modes: **Manual** (a gain of `2^EV`, the default) and **Physical** (aperture/shutter/ISO, the formula above), with `exposureEV` as compensation on top | The formula is now **[vendor]**: `filament/src/Exposure.cpp`, whose comments derive 1.2 = 78/(q·S), q = 0.65, S = 100. But it assumes photometric lights, and this engine's are unitless — a sun of intensity 3 at sunny-16 settings renders black. Manual at 0 EV is exactly gain 1, so no existing scene changes brightness |
| B2: PBR Neutral default, **AgX** as the filmic alternative | **PBR Neutral** (default) and **None** (stage A's clip). AgX not built | PBR Neutral is **[vendor]**, copied constant for constant from `KhronosGroup/ToneMapping`'s `pbrNeutral.glsl`. AgX's references **disagree**: Filament takes Rec.2020 input with an 8-term contrast fit, three.js and the "minimal AgX" it credits take Rec.709 through a conversion with a 7-term fit. A subtly wrong tone curve is invisible to every test in the tree, so the enum has room for AgX and nothing more until its constants come from one named source file |
| B2: `inverse(forward(x)) ≈ x` for PBR Neutral | `pbrNeutralInverse`, **derived** here (Khronos's inverse was not copied) | Held to the forward function by `colour_test` across [0, 8]: worst relative error 1.0e-5 |
| B3: authored `.cube`, **the cooker converts it** | Parsed at load by `core/cube_lut.h`, cached per path by `runtime/services/lut_library.h`, uploaded as an RGBA16F 3D texture | A 33³ LUT is ~36 000 short lines, parsed once per path per process; a cooker, cooked format and registry entry would be most of the stage for no measurable gain. The parser is **fuzzed** (`fuzz_cube_lut`) exactly as a cooked-format reader would be, so moving the parse into a cooker later changes where it runs, not what it accepts |
| B3: grade after tone mapping | After tone map **and** the sRGB encode, with texel-centre sampling | A Resolve or OCIO "Rec.709 / sRGB" export expects display-referred, encoded [0,1]. Sampling [0,1] without the half-texel inset would stretch the outermost cells at black and white |
| (review) | **The LUT's domain is enforced, not only documented:** display-referred, sRGB-encoded [0,1]. `LutLibrary` refuses a `.cube` declaring a domain outside [0,1] (built for scene-linear or log input); `core/output_transform.h` is the pass as one CPU function; `colour_test` §8 proves the grade sees encoded values (a squaring LUT gives 0.25, not 0.235) and reads `fs_output.sc` to require exposure → tone map → encode → grade in that order | Reviewer: a LUT fed the wrong domain "looks cursed" with no error, and the only guard was prose |
| (review) | Negative linear input is clamped **before** the tone map, in the shader and in the CPU reference; `pbrNeutral` itself stays the verbatim Khronos function | The reference folds negatives bright — `pbrNeutral(-1)` ≈ 0.99, pinned by `colour_test` §8 — so the pipeline guards it rather than editing a vendor function |
| (not in §4) | Settings live in `ColourGrading`, beside `Camera`, not in it | `Camera` is in `componentLayoutHash`; a field there makes every kit refuse to load. Not re-exported to kits, not hashed, `SimExempt` |
| (not in §4) | A LUT path is untrusted | It comes from a scene file: absolute paths and paths climbing out with `..` are refused, files over 64 MB are refused unread, failures are cached and logged once |

**Verification.**
- `colour_test` §6–§7 covers the maths and the reader:
  - EV100 of sunny-16 settings, the 1/1.2 scale, and manual gain.
  - PBR Neutral: its sub-knee offset, the toe, bounded and monotonic output over 0–1000, continuity at the knee, and the inverse round trip.
  - The shader's `pbrNeutral` constants, read from `colour.sh`.
  - `.cube` axis order, checked with channel-distinguishable values, plus domain, clamping and twelve malformed files refused.
- `colour_grading_test` covers the path from a camera entity to the output pass:
  - Resolving the settings, including NaN and overflow.
  - Serializer round trip, hostile fields, and future tone-mapper ids kept.
  - Not double-saved by reflected serde.
  - **Reflection offsets read back through flecs.**
  - Classification and hash invariance.
  - The camera finder handing the grading over.
  - `LutLibrary`: caching, path escape and absolute paths refused, failures cached once, project switch.
- `fuzz_cube_lut` and `fuzz_entity_serde` now include the component.
- Every property above was mutation-checked.
- On Metal, `engine_host fps_shooter --frames 240` built the output program with the 3D sampler bound at index 1 and exited cleanly. Its shutdown leak list was identical in size with and without the identity LUT (20 objects): those leaks predate stage B.

**Not verified:** the rendered image, still — no readback or golden-image test.
Tone mapping changes how every scene looks above the knee (≈0.76 after exposure)
and darkens the darks by up to 0.04; look at a scene before relying on it.

**Limits carried forward:**
- AgX (above).
- Editing a `.cube` does not reload it; restart the session.
- Symlinks inside the project that point out of it are not detected.
- The editor's scene viewport uses the default transform, not the scene camera's.
- Auto-exposure (histogram) is not built; it needs a compute path confirmed on every backend.

## 5. Stage C — HDR display output

The finding that shapes this stage: **both target platforms can reach HDR output
through bgfx as it is vendored**, but by different doors.

**C1. Windows — format-driven, already wired.** bgfx's `Dxgi::updateHdr10`
selects the swapchain colour space **from the backbuffer format** [measured —
`dxgi.cpp`, `updateHdr10`]:

| Backbuffer format | Colour space set |
|---|---|
| `R10G10B10A2_UNORM` | `RGB_FULL_G2084_NONE_P2020` — HDR10 / PQ |
| `R16G16B16A16_FLOAT` | `RGB_FULL_G10_NONE_P709` — scRGB |
| anything else | `RGB_FULL_G22_NONE_P709` — SDR |

So HDR on D3D11/D3D12 is a matter of choosing `init.resolution.format`. The
`BGFX_RESET_HDR10` flag is vestigial — no real backend reads it; only
`renderer_noop.cpp` references it [measured].

**Recommend scRGB (FP16) as the Windows HDR path.** It is linear with Rec.709
primaries, so grading and UI compositing are the same as SDR and there is no PQ
quantisation banding in an un-tooled pass [vendor — Microsoft]. The cost is
64 vs 32 bits per pixel against `RGB10A2` — 2×, from bgfx's own format table
[measured — `bgfx.h:251`, `:262`]. (One secondary source claims 4×; the format
sizes say otherwise.) In scRGB, 1.0 is 80 nits [vendor], which matters for C4.

bgfx also already queries `DXGI_OUTPUT_DESC1` for the output in `updateHdr10`
[measured]. Exposing that luminance information to the engine is how the tone
mapper learns the display's range (C3).

**C2. Apple (macOS, iOS, iPadOS) — EDR, reachable without patching bgfx.**

- bgfx's Metal backend **never enables EDR**: it sets no
  `wantsExtendedDynamicRangeContent` and no colour space [measured —
  `renderer_mtl.cpp`].
- But it accepts an `MTKView`, a **`CAMetalLayer`**, an `NSView` or an `NSWindow`
  as the native handle, and **uses a `CAMetalLayer` as-is** [measured —
  `renderer_mtl.cpp:3745–3800`, `SwapChainMtl::init`], and it lists `RGBA16Float`
  as a legal drawable format [measured — `:622`].
- So the engine can **own the `CAMetalLayer`**: set
  `wantsExtendedDynamicRangeContent = YES`, set an extended-linear colour space
  (extended linear Display P3), use `RGBA16Float` [vendor — Apple], then hand the
  layer to bgfx.
- **Today GLFW passes `glfwGetCocoaWindow` — an `NSWindow`** [measured —
  `glfw_platform.cpp:52`] — so bgfx builds its own layer and the engine has no
  hook. That is the one platform-layer change this stage needs. A SwiftUI host
  (vCAD's shell) naturally owns an `MTKView`, which is exactly the handle that
  works.
- **To verify with a spike before relying on it:** bgfx calls `setPixelFormat`
  on the layer but never `setColorspace`, so an engine-set colour space should
  survive — but whether any bgfx reset path touches the layer's EDR state is not
  yet confirmed.

**EDR headroom is dynamic.** `maximumExtendedDynamicRangeColorComponentValue`
reports it in SDR-relative units (100 nits ≈ 1.0, 1000 nits ≈ 10.0), and it
changes with the user's screen brightness [vendor — Apple]. The tone mapper must
read it **per frame**, not store it as a setting.

**C3. The tone mapper takes the display's range as input** — HGIG's core
recommendation [vendor]. HGIG describes the display with MaxTML (brightest level
still showing detail), MaxFFTML (full-frame) and MinTML. The engine feeds the
tone mapper from `DXGI_OUTPUT_DESC1` on Windows, EDR headroom on Apple, and a user
calibration screen (peak and paper-white) where the platform reports nothing
trustworthy.

**C4. UI in HDR.** UI must composite at *paper white*, not at 1.0. In scRGB 1.0 is
80 nits, so unscaled UI renders dim and grey against HDR content — scale UI by
`paperWhite / 80` in scRGB, and by the equivalent on EDR. ImGui (the editor)
needs the same treatment.

**Linux** HDR (Wayland) is deferred.

---

## 6. Stage D — authoring-grade colour (film product, later)

For `future-plans/film-tool.md`, not for games:

- **ACEScg working space** as an option — Unreal 5.1+ offers it as a project
  setting [vendor].
- **OpenColorIO 2.5**, which ships built-in ACES 2.0 CG and Studio configs;
  ACES 2 support left "preview" in OCIO 2.4.2 [vendor]. Linked by the *tool*.
- **OpenEXR** half-float scene-linear output.
- ACES 2.0 output transforms include gamut mapping built on colour-appearance
  modelling and are markedly heavier than ACES 1 [vendor]; whether they fit a
  real-time budget is unmeasured, so they are an offline-render feature until
  someone measures otherwise.

---

## 7. Decisions this leaves open

| Decision | Recommendation | Why it is open |
|---|---|---|
| Default tone mapper | Khronos PBR Neutral; AgX as alternative | A creative default — the user's call |
| HDR scene format | `RGBA16F`, then measure `RG11B10F` | Needs a VRAM measurement (R6) |
| Working space | Linear Rec.709 now; ACEScg as a later option | Wide gamut changes every authored colour |
| HDR output in v1 | Stage C after A+B are proven | Needs the Apple spike and hardware to test on |
| Windows HDR encoding | scRGB | HDR10 is cheaper in bandwidth; scRGB is simpler and linear |

---

## 8. Effort

| Stage | Estimate | Main risk |
|---|---|---|
| A — correctness | **3–4 d** | Every scene changes appearance; lighting may need re-tuning |
| B — exposure, tone map, LUT | **4–6 d** | Choosing defaults; auto-exposure needs compute on every backend |
| C — HDR output | **5–8 d** | Platform-specific; Apple needs the layer-ownership spike; needs HDR hardware |
| D — authoring grade | later | Belongs to the film product |

**Stage A first, and on its own.** It is a correctness fix, independent of the
render graph, and every later stage is built on its assumptions.

---

## 9. Sources

In-tree references are cited inline with paths. External:

- [Khronos PBR Neutral Tone Mapper — press release](https://www.khronos.org/news/press/khronos-pbr-neutral-tone-mapper-released-for-true-to-life-color-rendering-of-3d-products) · [reference implementation](https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md)
- [Blender 4.0 colour management — AgX](https://developer.blender.org/docs/release_notes/4.0/color_management/)
- [Unreal: Working Color Space](https://dev.epicgames.com/documentation/unreal-engine/working-color-space-in-unreal-engine) · [Filmic tonemapper](https://dev.epicgames.com/documentation/unreal-engine/color-grading-and-the-filmic-tonemapper-in-unreal-engine) · [HDR display output](https://dev.epicgames.com/documentation/en-us/unreal-engine/high-dynamic-range-display-output-in-unreal-engine)
- [Microsoft: DirectX with Advanced Color on HDR/SDR displays](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range) · [DirectXTK12: Using HDR rendering](https://github.com/microsoft/DirectXTK12/wiki/Using-HDR-rendering)
- [Apple: `wantsExtendedDynamicRangeContent`](https://developer.apple.com/documentation/quartzcore/cametallayer/wantsextendeddynamicrangecontent) · [WWDC21: Explore HDR rendering with EDR](https://developer.apple.com/videos/play/wwdc2021/10161/) · [WWDC22: Explore EDR on iOS](https://developer.apple.com/videos/play/wwdc2022/10113/)
- [HGIG](https://www.hgig.org/) · [For a Better HDR Gaming Experience (PDF)](https://www.hgig.org/doc/ForBetterHDRGaming.pdf)
- [OpenColorIO 2.4 release](https://opencolorio.readthedocs.io/en/latest/releases/ocio_2_4.html) · [OCIO 2.5 release](https://opencolorio.readthedocs.io/en/latest/releases/ocio_2_5.html) · [OCIO GPU shader API](https://opencolorio.readthedocs.io/en/latest/api/shaders.html)
- [ACES 2.0 release (Variety)](https://variety.com/2025/film/news/academy-arts-sciences-aces-color-encoding-2-0-release-1236360406)
