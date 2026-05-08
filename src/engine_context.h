#pragma once

#include <flecs.h>

#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "io/project_context.h"
#include "io/importer_registry.h"
#include "editor/editor_state.h"
#include "editor/gizmo_state.h"

// EngineContext: everything a panel or subsystem needs, in one place.
//
// Passed by reference to all drawXxxPanel() functions and gizmo code.
// Replaces the growing per-call parameter lists:
//   before: drawHierarchyPanel(ecs, editor)
//           drawInspectorPanel(ecs, editor)
//           drawGizmo(editor, view, proj, gizmoState)
//   after:  drawHierarchyPanel(ctx)
//           drawInspectorPanel(ctx)
//           drawGizmo(ctx, view, proj)
//
// OWNERSHIP NOTE: EngineContext holds *references* to the heavy objects
// (ecs, assets, project, importers) which are owned by main.cpp. It
// directly owns the editor-only state (editor, gizmoState) because
// nothing outside the editor touches those.
//
// MILESTONE 6 NOTE: When the Engine class arrives, it will own ecs,
// assets, project, and importers. EngineContext will then hold references
// into the Engine class instead of into main.cpp stack variables. The
// migration is mechanical and localized to main.cpp.
struct EngineContext {
    flecs::world&      ecs;
    AssetRegistry&     assets;
    TextureRegistry&   textures;
    MaterialRegistry&  materials;
    ProjectContext&    project;
    ImporterRegistry&  importers;

    // Editor-only state. Panels read/write these freely.
    EditorState  editor;
    GizmoState   gizmoState;
};
