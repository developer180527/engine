// ── eng_kits — a project's C modules, run as kits (WO-094) ───────────────────
// A C module named in project.json's "kits" is a shared library exporting
// engModuleEntry (eng_module.h). KitHost loads it like any kit; this file is
// what the C API gives it to run one: the entry is called with engGetProc,
// the description checked, on_load run, and the module wrapped as a plugin:
//
//   onSimulationStart  the simulation world's C world (shared by every C kit
//                      and the program's handle) + on_world_start
//   onSimulationStop   on_world_stop, then whatever systems and queries the
//                      module made are destroyed, and the world let go
//   onDetach           on_unload
//
// A hot reload is a stop, a detach, a load and a start, so the new code
// starts in a world with none of the old code's systems in it.
#include <eng/engine.h>

#include "capi/eng_internal.h"
#include "core/logger.h"
#include "runtime/kit_host.h"
#include "runtime/plugin.h"
#include "runtime/runtime.h"

#include <memory>
#include <string>

namespace {

class CModulePlugin final : public IEnginePlugin {
public:
    CModulePlugin(EngineRuntime* rt, const EngModuleDesc& d, void* state)
        : m_rt(rt), m_d(d), m_name(d.name), m_version(d.version ? d.version : "?"), m_state(state) {}

    const char* name() const override { return m_name.c_str(); }
    const char* version() const override { return m_version.c_str(); }

    void onAttach(RuntimeContext&) override {}
    void onDetach() override {
        if (!m_loaded) return;
        m_loaded = false;
        if (m_d.on_unload) m_d.on_unload(nullptr, m_state);
    }
    void onSimulationStart(flecs::world& w) override {
        if (m_world) return;
        m_world = engInternalAcquireKitWorld(m_rt, w);
        if (!m_d.on_world_start) return;
        engInternalSetError("");
        const void* was = engInternalSetOwner(m_world, this);
        const EngResult r = m_d.on_world_start(m_world, m_state);
        engInternalSetOwner(m_world, was);
        if (r == ENG_OK) return;
        // Nothing of it stays running: what it made is destroyed, its world
        // let go. The kit stays listed, and does nothing this session.
        LOG_ERROR("Kit", "module '%s': on_world_start failed (%d): %s", m_name.c_str(), (int)r,
                  engGetLastErrorMessage());
        release();
    }
    void onSimulationStop() override {
        if (!m_world) return;
        if (m_d.on_world_stop) {
            const void* was = engInternalSetOwner(m_world, this);
            m_d.on_world_stop(m_world, m_state);
            engInternalSetOwner(m_world, was);
        }
        release();
    }
    ~CModulePlugin() override { onSimulationStop(); onDetach(); }

private:
    void release() {
        engInternalDropOwned(m_world, this);
        engInternalReleaseKitWorld(m_world);
        m_world = nullptr;
    }

    EngineRuntime* m_rt;
    EngModuleDesc m_d;
    std::string m_name, m_version;
    void* m_state;
    EngWorld* m_world = nullptr;
    bool m_loaded = true;
};

} // namespace

void engInternalHostCModules(EngineRuntime* rt) {
    if (!rt) return;
    rt->kits().setCModuleFactory([rt](void* entry, std::string& why) -> std::shared_ptr<IEnginePlugin> {
        const auto fn = reinterpret_cast<EngModuleEntryFn>(entry);
        const EngModuleDesc* desc = fn(&engGetProc);
        if (!desc) {
            why = "engModuleEntry returned NULL (a function it needs is missing from this engine?)";
            return nullptr;
        }
        EngModuleDesc d{};
        if (engInternalReadModule(desc, "its description", d, why) != ENG_OK) return nullptr;
        void* state = nullptr;
        if (d.on_load) {
            engInternalSetError("");
            if (const EngResult r = d.on_load(nullptr, &state); r != ENG_OK) {
                why = "module '" + std::string(d.name) + "': on_load failed (" + std::to_string((int)r) +
                      "): " + engGetLastErrorMessage();
                return nullptr;
            }
        }
        return std::make_shared<CModulePlugin>(rt, d, state);
    });
}
