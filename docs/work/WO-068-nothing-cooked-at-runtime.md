---
status: plan
id: WO-068
title: Nothing is cooked or compiled at runtime — shaders' final driver step is the only exception
program: assets
priority: P2
size: L
state: todo
depends: [WO-069]
touches:
  - src/animation/clip_library.h
  - src/animation/ozz_bridge.h
  - src/animation/cooked_clip.h
  - src/plugins/lua_script_plugin.h
  - src/audio/miniaudio_provider.cpp
  - src/runtime/services/nav_service.cpp
  - src/tools/engine_build.cpp
source: owner 2026-11-06 ("on runtime EVERYTHING except shaders should be pre-cooked"); review of what the runtime still builds
---
## Why
A shipped game should only load and use data. All building, converting and compiling should happen before it ships, so load times are predictable, a shipped game cannot fail a build step, and the runtime carries no builders. WO-018 already stopped the runtime parsing source formats. A few things are still built after loading:
- **Animation:** every clip is compiled into ozz's runtime form when it loads (`clip_library.h` → `anim::bindRawClip` → ozz `AnimationBuilder`), per skeleton.
- **Scripts:** Lua ships as source and is compiled when it loads (`luaL_loadfile`, `lua_script_plugin.h`).
- **Audio:** source `.wav`/`.mp3`/`.ogg`/`.flac` files ship, and the audio provider decodes whatever format each file happens to be in.
- **Navigation:** Recast's navmesh builder (`nav_service.cpp`) is compiled into the runtime. No caller was found, and there is no cooked navmesh to load instead.

## The rule
**At runtime the engine loads cooked data and uses it. It never builds, converts, resamples, optimises or compiles.** Decompressing a format chosen at cook time is allowed (a streamed Opus or Vorbis track, BC7 textures), because that is reading data, not making it.

**The one exception is shaders,** and only their last step. Shaders ship already compiled (bytecode per backend, as `engine_cook` already produces). What remains at runtime is the GPU driver turning bytecode into a pipeline, which no engine can do ahead of time on PC. That step is kept off the frame: pipelines are created at load, and a pipeline cache (librhi has one) is warmed and saved, so a second run compiles nothing. A pipeline created mid-frame is reported as a hitch.

## Done when
- [ ] animation: the cook produces the clip in the form the runtime samples, so loading a clip runs no ozz builder. Cooked clips are bound per skeleton at cook time, or else the cooked form is skeleton-independent and binding is a lookup table with no building; this order records which one, with load-time numbers for both
- [ ] scripts: `engine_build` ships Lua as precompiled bytecode, the shipped runtime loads binary chunks only (`mode "b"`), and refuses source
- [ ] audio: the shipped game loads only the cooked engine audio format from WO-069, and does no format conversion or resampling at runtime
- [ ] navigation: the navmesh is baked at cook time into a cooked asset, the runtime loads it, and Recast's builder is absent from the shipped player
- [ ] shaders: no runtime shader compiler in the shipped player (no `shaderc`, no Slang, no source in `dist/`); pipelines are created at load; a pipeline created during a frame is logged with its name
- [ ] a check fails CI if the shipped player contains any builder symbol (ozz `AnimationBuilder`/`SkeletonBuilder`, Lua's parser, Recast build functions, shader compilers), extending WO-066's symbol check, mutation-tested
- [ ] the asset-system doc states the rule and the one exception

## Contract
Nothing: a development build may still build things on demand (the editor's cook requests, WO-018). Only the shipped runtime loses them, and an asset that was not cooked fails at load, by name, as today.

## Steps
1. Measure: load time for the sample projects with the current clip building, so the change is shown to help, not assumed to.
2. Animation, then audio, then Lua, then the navmesh.
3. Shader pipelines at load, plus the warm cache.
4. The symbol check.

## Not in scope
- Text the game only knows at runtime (player names, chat), which needs glyphs rasterised at runtime. That is the game UI's question (WO-067), and the order there should decide whether it is a second exception.
- Procedural content a game generates on purpose (`engCreateMesh`, `engCreateTexture` in the API draft). That is the game's own data, not cooking.
- Designing the animation and audio formats themselves: that is WO-069, which this order builds on.
