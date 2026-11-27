#pragma once
// ── DemoGrid — an entity of the engine's demo grid, never saved ──────────────
// The default scene (runtime_boot.cpp, buildDefaultScene) spawns a 3x3 grid of
// spinning primitive cubes so an empty project shows something. They are made
// by code each boot, and their mesh is an engine primitive with no asset
// behind it, so they are never saved into a scene, never copied into a Play
// snapshot, and not listed in the hierarchy.
//
// This tag is that rule. It used to be "has a Spinner", which also dropped,
// on save, every entity a user gave a Spinner in the inspector, and made a
// scene's "spinner" key impossible to load (the fuzz scene's movers never
// moved). Spinner is now an ordinary saved component (core.Spinner).
struct DemoGrid {};
