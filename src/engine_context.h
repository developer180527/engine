#pragma once
class PrimitiveLibrary;
class AssetService;
class SceneService;
class SkeletonRegistry;
class AnimClipRegistry;
#include "engine/runtime_context.h"
#include <assetlib/asset_registry.h>
#include "editor/editor_state.h"
#include "editor/gizmo_state.h"

// EngineContext bundles the runtime view with editor-owned state.
// Constructed on the stack each frame inside EditorApp::renderUI().
// Never stored persistently — panels receive it by reference for one frame.
// EngineRuntime has zero knowledge of this struct.
struct EngineContext {
    flecs::world&     ecs;
    AssetRegistry&    assets;
    TextureRegistry&  textures;
    MaterialRegistry& materials;
    ProjectContext&   project;
    ImporterRegistry& importers;
    EditorState&      editor;
    GizmoState&       gizmoState;
    assetlib::AssetRegistry* assetLib     = nullptr;
    PrimitiveLibrary*        primitives   = nullptr;
    AssetService*            assetService = nullptr;
    SceneService*            sceneService = nullptr;
    SkeletonRegistry*        skeletons    = nullptr;
    AnimClipRegistry*        clips        = nullptr;
};
