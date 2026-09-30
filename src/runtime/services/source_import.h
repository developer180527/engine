#pragma once
// ── source_import — opt a host in to reading SOURCE assets (WO-017) ───────────
//
// A runtime loads cooked content only. Reading a .glb or .fbx directly is a
// development feature (drag-drop into the editor, a dev runner, the scene
// migration tool) and it needs the cook stack: the import front ends, Assimp,
// the glTF parser. That stack lives in the `engine_cooking` library, and this
// function lives in `engine_source_import`, a dev-only library that links it.
//
// It used to be done inside EngineRuntime::init, behind
// `#if ENGINE_WITH_SOURCE_IMPORTERS`, so every runtime of a dev build carried
// the whole cook stack, engine_player included. Now a host that wants it says
// so, after init; a host that doesn't (the player, a server, most tests) links
// none of it.
//
//   EngineRuntime rt; rt.init(cfg);
//   sourceimport::install(rt);     // editor, engine_host, scene_resave
//
// Installs:
//   * the glTF and Assimp mesh importers into the runtime's ImporterRegistry,
//     unless the runtime is headless (as before: meshes need the GPU);
//   * ClipLibrary's source reader, so an uncooked standalone clip loads
//     (headless too: a clip is CPU data).
class EngineRuntime;

namespace sourceimport {
void install(EngineRuntime& rt);
}
