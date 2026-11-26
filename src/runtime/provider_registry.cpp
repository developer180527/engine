// provider_registry.cpp — see provider_registry.h (WO-076).
#include "runtime/provider_registry.h"

#include "runtime/plugin.h"
#include "render/renderer_interface.h"

#include <nlohmann/json.hpp>

namespace providers {

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Physics:   return "physics";
        case Kind::Scripting: return "scripting";
        case Kind::Audio:     return "audio";
        case Kind::Renderer:  return "renderer";
    }
    return "?";
}

bool kindFromName(std::string_view name, Kind& out) {
    for (Kind k : kAllKinds)
        if (name == kindName(k)) { out = k; return true; }
    return false;
}

std::string allKindNames() {
    std::string s;
    for (Kind k : kAllKinds) { if (!s.empty()) s += ", "; s += kindName(k); }
    return s;
}

bool Registry::add(Entry e, std::string* why) {
    if (e.name.empty()) {
        if (why) *why = std::string("a ") + kindName(e.kind) + " provider with no name";
        return false;
    }
    if (find(e.kind, e.name)) {
        if (why) *why = std::string("two ") + kindName(e.kind) + " providers named '" + e.name + "'";
        return false;
    }
    m_entries.push_back(std::move(e));
    return true;
}

bool Registry::addPlugin(Kind k, std::string name, std::string description,
                         PluginFactory f, std::string* why) {
    if (k == Kind::Renderer) {
        if (why) *why = "a renderer is registered with addRenderer";
        return false;
    }
    Entry e{k, std::move(name), std::move(description), std::move(f), {}};
    return add(std::move(e), why);
}

bool Registry::addRenderer(std::string name, std::string description,
                           RendererFactory f, std::string* why) {
    Entry e{Kind::Renderer, std::move(name), std::move(description), {}, std::move(f)};
    return add(std::move(e), why);
}

void Registry::addUnavailable(Kind k, std::string name, std::string whyNot) {
    if (find(k, name) || whyUnavailable(k, name)) return;
    m_unavailable.push_back({k, std::move(name), std::move(whyNot)});
}

const std::string* Registry::whyUnavailable(Kind k, std::string_view name) const {
    for (const Unavailable& u : m_unavailable)
        if (u.kind == k && u.name == name) return &u.whyNot;
    return nullptr;
}

const Entry* Registry::find(Kind k, std::string_view name) const {
    for (const Entry& e : m_entries)
        if (e.kind == k && e.name == name) return &e;
    return nullptr;
}

const Entry* Registry::resolve(Kind k, std::string_view name, std::string& why) const {
    if (const Entry* e = find(k, name)) return e;
    if (const std::string* whyNot = whyUnavailable(k, name)) {
        why = std::string("the ") + kindName(k) + " provider '" + std::string(name) +
              "' is not built here: " + *whyNot +
              (names(k).empty() ? std::string(" (none built)") : " (built: " + namesList(k) + ")");
        return nullptr;
    }
    const std::string known = namesList(k);
    why = std::string("no ") + kindName(k) + " provider named '" + std::string(name) +
          "' in this build (" + (known.empty() ? std::string("it has none") : "known: " + known) + ")";
    return nullptr;
}

std::vector<std::string> Registry::names(Kind k) const {
    std::vector<std::string> out;
    for (const Entry& e : m_entries) if (e.kind == k) out.push_back(e.name);
    return out;
}

std::string Registry::namesList(Kind k) const {
    std::string s;
    for (const std::string& n : names(k)) { if (!s.empty()) s += ", "; s += n; }
    return s;
}

bool Registry::claimList(const std::string& listName) {
    for (const std::string& l : m_lists) if (l == listName) return false;
    m_lists.push_back(listName);
    return true;
}

void Registry::inject(Kind k, std::shared_ptr<IEnginePlugin> p) {
    if (k != Kind::Renderer) m_injected[(int)k] = std::move(p);
}

std::shared_ptr<IEnginePlugin> Registry::injected(Kind k) const {
    return m_injected[(int)k];
}

std::string Registry::describe() const {
    std::string s;
    for (Kind k : kAllKinds) {
        s += kindName(k);
        s += ":\n";
        for (const Entry& e : m_entries) {
            if (e.kind != k) continue;
            s += "  " + e.name;
            if (!e.description.empty()) {
                s.append(e.name.size() < 14 ? 14 - e.name.size() : 1, ' ');
                s += e.description;
            }
            s += "\n";
        }
        for (const Unavailable& u : m_unavailable)
            if (u.kind == k) s += "  " + u.name + "  (not built: " + u.whyNot + ")\n";
    }
    return s;
}

std::string Registry::describeJson() const {
    nlohmann::json j = nlohmann::json::object();
    for (Kind k : kAllKinds) {
        nlohmann::json arr = nlohmann::json::array();
        for (const Entry& e : m_entries)
            if (e.kind == k) arr.push_back({{"name", e.name}, {"description", e.description},
                                            {"built", true}});
        for (const Unavailable& u : m_unavailable)
            if (u.kind == k) arr.push_back({{"name", u.name}, {"built", false}, {"why_not", u.whyNot}});
        j[kindName(k)] = std::move(arr);
    }
    return j.dump(2);
}

} // namespace providers
