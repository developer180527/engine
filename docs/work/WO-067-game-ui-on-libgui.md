---
status: plan
id: WO-067
title: Game UI (HUD, menus, cockpit panels) on libgui, never on editor code
program: providers
priority: P3
size: L
state: todo
depends: []
new-contracts: [game-ui]
touches:
  - experiments/libgui_sandbox/CMakeLists.txt
  - CMakeLists.txt
source: review 2026-11-06 (should libgui also draw the in-game HUD)
---
## Why
A game needs UI: a HUD, menus, and for something like the Apollo LM, panels on surfaces in the 3D world. libgui (the owner's Rust UI core with a C ABI, `../libgui`) already fits a game's constraints. It owns no window, GPU, clock or threads; it builds for wasm32, iOS and Android; it can be driven by a gamepad (`UiAction`); and it sleeps when nothing moves. One UI library for the editor and the game is simpler than two, provided a game never reaches editor code. Today libgui exists in the engine only as `experiments/libgui_sandbox`, off by default, and the editor is ImGui.

## Done when
- [ ] a `game-ui` contract in `docs/contracts/` names what a game sees: a UI service (`engine.ui.v1` in the new API) with begin/end frame, input routed from the view it belongs to, and output drawn as a view layer or into a texture
- [ ] libgui builds as part of the engine (cargo through CMake) on every CI leg, behind an option, with the toolchain cost recorded
- [ ] a HUD in one sample: health, crosshair and a pause menu, driven by keyboard, mouse and gamepad
- [ ] UI drawn into a texture that a scene uses (one world-space panel), with input reaching it through a ray from the camera
- [ ] an idle HUD costs nothing measurable: `empty_game_budget_test` with a static HUD shows no per-frame UI work
- [ ] an audit rule: no file a game links includes `src/editor/` or editor-only libgui widgets (docking)
- [ ] a game theme (image-skinned widgets, the game's fonts) is either built here or recorded as a libgui work order, with the reason
- [ ] **Rust on every platform a game ships to:** libgui is Rust, so game UI makes every port need a Rust toolchain for that target. Desktop, iOS, Android and wasm are covered; consoles are not public (Rust for them exists only under the platform holders' agreements, and lags). This order records which console targets are wanted and whether Rust is confirmed there; where it is not, game UI on that platform has a named fallback (a C/C++ implementation of `engine.ui.v1`), so the contract never depends on the language

## Contract
Nothing: without the UI service a game draws no UI, and `engFindService("engine.ui.v1")` returns NOT_FOUND, which a game handles.

## Steps
1. The contract, written against `docs/plans/engine-api.md`.
2. libgui in the build, renderer backend from `experiments/libgui_sandbox/libgui_bgfx.cpp`.
3. Screen-space HUD, then UI into a texture, then world-space input.
4. Idle cost and the audit rule.

## Not in scope
- Moving the editor from ImGui to libgui. That is separate and does not block this.
- Accessibility (screen readers). It is on libgui's own roadmap and blocks shipping to consumers, so it is recorded here as a known gap.
- A designer-facing layout file format.
