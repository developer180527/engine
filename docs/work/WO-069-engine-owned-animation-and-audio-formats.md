---
status: plan
id: WO-069
title: Cooked animation and audio in the engine's own formats, so changing a provider never changes them
program: assets
priority: P2
size: L
state: todo
depends: []
contracts: [audio-provider]
touches:
  - modules/assetlib/include/assetlib/clip_asset.h
  - modules/assetlib/include/assetlib/mesh_asset.h
  - src/animation/cooked_clip.h
  - src/animation/cooked_skin.h
  - src/assets/cookers/clip/clip_cook.cpp
  - src/audio/miniaudio_provider.cpp
  - src/tools/engine_build.cpp
source: review 2026-11-06 (should cooked audio and animation be engine formats); engine-architecture.md §4.1
---
## Why
Cooked meshes and scenes are in the engine's own formats. Animation and audio are not. A cooked clip (`CLIP`, `cooked_clip.h`) holds an **ozz archive**, and a skinned mesh embeds ozz's skeleton and clip archives (`cooked_skin.h`). That is ozz's own serialisation, so upgrading ozz or replacing the animation runtime invalidates every cooked clip and skinned mesh. Audio is not cooked at all: source `.wav`/`.mp3`/`.ogg`/`.flac` files ship as they are, so each audio provider must decode every source format itself, and a new provider can quietly fail to play some of them. What a game ships should not depend on which provider is plugged in.

## The rule
**Two tiers.**
1. **The cooked asset is in the engine's format.** It is documented field by field in `docs/reference/`, versioned, and migrated, never broken. It is what ships, and it is covered by the API plan's data promise (`docs/plans/engine-api.md` §2).
2. **A provider's own form is a derived cache,** built from tier 1. Examples are ozz's runtime animation and a middleware's decoded buffers. It can always be thrown away and rebuilt, and it is never the only copy. This is how renderers already treat textures: BC7 or ASTC is a per-renderer cook profile of the engine's data.

**One exception: middleware banks** (Wwise `.bnk`, FMOD `.bank`) are the provider's own data by design. The project declares them as requiring that provider, so a missing provider fails at load by name (scene extensions, `engine-architecture.md` §4.7).

## Done when
- [ ] **animation format** in `docs/reference/animation-format.md`: skeleton (bone names, parents, rest pose) and clips (per-bone keys by bone name: time, translation, rotation, scale, plus the clip's duration and frame rate), with no third-party type anywhere in the bytes
- [ ] the clip cooker and the skinned mesh write it; no ozz archive is left in any cooked file; `modules/assetlib` reads and writes it without ozz
- [ ] ozz's runtime form is built from it by the animation runtime, at cook time into a cache keyed by provider and version (WO-068: nothing built at runtime), so deleting that cache rebuilds it and changes nothing visible
- [ ] **audio format** in `docs/reference/audio-format.md`: sample rate, channels, frame count, loop points, a loudness value, and the samples as PCM or one chosen codec. A short study picks the codec and records why
- [ ] an audio cooker turns source files into it; `engine_build` ships the cooked audio, not the source files
- [ ] the audio provider contract says a provider receives this format (or a bank it declared), and the conformance suite plays the cooked format through every provider
- [ ] migrations: a clip, a skinned mesh and a scene cooked before this change still load, by rebuilding from the old form once, tested with files kept from before the change
- [ ] a test swaps the animation runtime's derived cache (delete it) and the audio provider (miniaudio for the null provider), and the cooked files are byte-identical before and after

## Contract
Nothing: the audio provider contract keeps accepting today's bytes until the cooked format lands, so a provider written against it keeps working. The null provider accepts the new format and plays silence, as today.

## Steps
1. Write both reference docs first, and have them reviewed: they are the promise.
2. Animation: write the new format, convert from it to ozz, then remove ozz archives from cooked files.
3. The audio codec study, then the audio cooker, then the provider contract.
4. Migrations and the swap test.

## Not in scope
- Animation compression beyond what the format needs (curve fitting, quantisation). It can come later as a format version, with a migration.
- Spatial audio data (reverb zones, occlusion geometry). Those are scene data, not sound files.
- Replacing ozz. This makes it replaceable; it does not replace it.
