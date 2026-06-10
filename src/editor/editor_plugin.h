#pragma once

// ── IEditorPlugin ──────────────────────────────────────────────────────────
// Optional editor extension for engine plugins. A plugin that wants to draw
// ImGui widgets (settings panels, debug stats) inherits BOTH interfaces:
//
//   class JoltPlugin : public IEnginePlugin, public IEditorPlugin { ... };
//
// The editor walks the runtime's PluginRegistry and dynamic_casts each plugin
// to IEditorPlugin to fan out UI calls. The runtime itself never sees this
// interface — a standalone game ships zero editor code.
class IEditorPlugin {
public:
    virtual ~IEditorPlugin() = default;

    // Called inside an existing ImGui scope (Plugins menu etc.)
    virtual void onEditorUI() = 0;
};
