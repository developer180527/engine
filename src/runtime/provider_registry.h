#pragma once
// ── provider_registry — which providers this build has, by kind and name ─────
// WO-076. One list per build (plugins/stock_providers.cpp) registers every
// provider explicitly: kind, name, factory. Nothing self-registers through a
// static initialiser, because a static library drops objects nobody references
// and a provider registered that way silently disappears from a shipped game.
//
// Choosing is refusing by name (DR-0035): `resolve` either returns the entry or
// an error that names the kind, the name asked for and every name this build
// has. It never substitutes another provider. Every kind has a `none` provider,
// chosen by name like any other and never used as a fallback.
//
// The kinds are a fixed list until the C API's module part lands
// (docs/plans/engine-api.md). Manifests, platforms and build-time resolution
// feed this registry later (WO-085, WO-086); it assumes nothing beyond kind,
// name and factory, so they can.
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class IEnginePlugin;
struct IRenderer;

namespace providers {

enum class Kind { Physics, Scripting, Audio, Renderer };
inline constexpr Kind kAllKinds[] = { Kind::Physics, Kind::Scripting, Kind::Audio, Kind::Renderer };

const char* kindName(Kind k);
bool        kindFromName(std::string_view name, Kind& out);
std::string allKindNames();                 // "physics, scripting, audio, renderer"

using PluginFactory   = std::function<std::shared_ptr<IEnginePlugin>()>;
using RendererFactory = std::function<std::unique_ptr<IRenderer>()>;

struct Entry {
    Kind            kind;
    std::string     name;
    std::string     description;            // one line, for --list-providers
    PluginFactory   makePlugin;             // physics, scripting, audio
    RendererFactory makeRenderer;           // renderer
};

class Registry {
public:
    // Registers a provider. A second provider with the same kind and name is a
    // defect in the build's list: refused, and reported through `why`.
    bool addPlugin  (Kind k, std::string name, std::string description,
                     PluginFactory f, std::string* why = nullptr);
    bool addRenderer(std::string name, std::string description,
                     RendererFactory f, std::string* why = nullptr);

    const Entry* find(Kind k, std::string_view name) const;

    // The entry for `name`, or nullptr with `why` naming what was asked for
    // and what this build has.
    const Entry* resolve(Kind k, std::string_view name, std::string& why) const;

    std::vector<std::string> names(Kind k) const;   // in registration order
    std::string              namesList(Kind k) const;   // "jolt, none"
    const std::vector<Entry>& entries() const { return m_entries; }
    bool empty() const { return m_entries.empty(); }

    // A provider object handed over by the host, without a name
    // (engine-architecture.md §2b). It fills its kind instead of the project's
    // choice. Plugin kinds only: a renderer is created during init().
    void inject(Kind k, std::shared_ptr<IEnginePlugin> p);
    std::shared_ptr<IEnginePlugin> injected(Kind k) const;

    // True the first time a list with this name is registered into this
    // registry, false after: a list registers once, however many paths reach it.
    bool claimList(const std::string& listName);

    // The whole list as text or JSON, for `--list-providers`.
    std::string describe() const;
    std::string describeJson() const;

private:
    bool add(Entry e, std::string* why);
    std::vector<Entry> m_entries;
    std::vector<std::string> m_lists;
    std::shared_ptr<IEnginePlugin> m_injected[4];
};

} // namespace providers
