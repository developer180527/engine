// provider_resolve.cpp — see provider_resolve.h (WO-086).
#include "project/provider_resolve.h"

#include <algorithm>

namespace provsel {

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> k = {"physics", "scripting", "audio", "renderer"};
    return k;
}

const std::vector<std::string>& coreServices() {
    // What core hands every provider (provider-abi.md rule 9). A provider's
    // `requires` naming anything else must be met by another chosen provider.
    static const std::vector<std::string> s = {
        "engine.jobs.v1", "engine.memory.v1", "engine.log.v1", "engine.debug_draw.v1"};
    return s;
}

const nlohmann::json& engineDefaults() {
    // The rule "no display, or a server, gets no renderer" is this table, not
    // a special case in code: a runtime with no display resolves as `server`.
    static const nlohmann::json d = {
        {"physics", "jolt"},
        {"scripting", "lua"},
        {"audio", "miniaudio"},
        {"renderer", {{"default", "forward-bgfx"}, {"server", "none"}}},
    };
    return d;
}

const std::vector<std::string>& flavours() {
    static const std::vector<std::string> f = {"client", "server"};
    return f;
}

const std::string* Result::chosen(const std::string& kind) const {
    for (const KindResult& k : kinds)
        if (k.kind == kind && !k.chosen.empty()) return &k.chosen;
    return nullptr;
}

namespace {

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& x : v) { if (!s.empty()) s += ", "; s += x; }
    return s;
}

std::vector<std::string> platformNames(const Inputs& in) {
    std::vector<std::string> n;
    for (const auto& [name, p] : in.platforms) n.push_back(name);
    return n;
}

// Is `key` a valid target key? Fills `why` when not.
bool validTargetKey(const Inputs& in, const std::string& key) {
    if (key == "default") return true;
    if (in.platforms.count(key) || contains(flavours(), key)) return true;
    const auto dot = key.find('.');
    if (dot == std::string::npos) return false;
    return in.platforms.count(key.substr(0, dot)) && contains(flavours(), key.substr(dot + 1));
}

// The name a value chooses for the target, and the key that chose it.
// Returns false when the value has no key for this target (and no default).
bool pick(const nlohmann::json& v, const Target& t, std::string& name, std::string& key) {
    if (v.is_string()) { name = v.get<std::string>(); key = "default"; return true; }
    for (const std::string& k : {t.platform + "." + t.flavour, t.flavour, t.platform, std::string("default")}) {
        auto it = v.find(k);
        if (it != v.end()) { name = it->get<std::string>(); key = k; return true; }
    }
    return false;
}

// Why `d` cannot fill its kind for this target; empty when it can. The
// services check needs every choice, so it is separate.
std::string whyNot(const Decl& d, const Inputs& in) {
    const Target& t = in.target;
    if (!contains(d.platforms, t.platform))
        return "it supports " + join(d.platforms) + ", not " + t.platform;
    if (!contains(d.flavours, t.flavour))
        return "it is not for " + t.flavour + " builds (flavours: " + join(d.flavours) + ")";
    auto p = in.platforms.find(t.platform);
    for (const std::string& n : d.needs) {
        const bool have = p != in.platforms.end() && p->second.facts.count(n) && p->second.facts.at(n);
        if (!have) return "it needs '" + n + "', which this " + t.platform + " target does not have";
    }
    if (!d.built) return "it is not built here: " + d.whyNotBuilt;
    return {};
}

std::vector<std::string> stringList(const nlohmann::json& j, const char* key, std::string& why, bool& ok) {
    std::vector<std::string> out;
    auto it = j.find(key);
    if (it == j.end()) return out;
    if (!it->is_array()) { why = std::string("'") + key + "' must be an array of names"; ok = false; return out; }
    for (const auto& e : *it) {
        if (!e.is_string()) { why = std::string("'") + key + "' must be an array of names"; ok = false; return out; }
        out.push_back(e.get<std::string>());
    }
    return out;
}

} // namespace

Result resolve(const Inputs& in) {
    Result r;
    r.target = in.target;
    const std::string keysHelp = "default, a platform (" + join(platformNames(in)) +
                                 "), a flavour (" + join(flavours()) + "), or <platform>.<flavour>";

    // 1. The shape of the project's block.
    nlohmann::json block = in.providers.is_null() ? nlohmann::json::object() : in.providers;
    if (!block.is_object()) {
        r.refusals.push_back("project.json \"providers\" must be an object of kind: name");
        return r;
    }
    for (auto it = block.begin(); it != block.end(); ++it) {
        if (!contains(kinds(), it.key())) {
            r.refusals.push_back("project.json \"providers\": '" + it.key() +
                                 "' is not a provider kind (kinds: " + join(kinds()) + ")");
            continue;
        }
        const nlohmann::json& v = it.value();
        if (v.is_string()) continue;
        if (!v.is_object()) {
            r.refusals.push_back("project.json \"providers\"." + it.key() +
                                 ": the value must be a provider name, or an object of target: name");
            continue;
        }
        for (auto kt = v.begin(); kt != v.end(); ++kt) {
            if (!validTargetKey(in, kt.key()))
                r.refusals.push_back("project.json \"providers\"." + it.key() + ": '" + kt.key() +
                                     "' is not a target key (keys: " + keysHelp + ")");
            else if (!kt.value().is_string())
                r.refusals.push_back("project.json \"providers\"." + it.key() + "." + kt.key() +
                                     ": the value must be a provider name");
        }
    }
    for (const auto& [kind, name] : in.overrides)
        if (!contains(kinds(), kind))
            r.refusals.push_back("override: '" + kind + "' is not a provider kind (kinds: " + join(kinds()) + ")");
    if (!r.ok()) return r;

    // 2–6. Per kind: choose, then check the choice.
    for (const std::string& kind : kinds()) {
        KindResult kr;
        kr.kind = kind;
        std::string name, key;
        bool have = false;
        for (const auto& [k, n] : in.overrides)
            if (k == kind) { name = n; key = "override"; have = true; }
        if (!have && block.contains(kind)) have = pick(block[kind], in.target, name, key);
        if (!have) {
            have = pick(engineDefaults()[kind], in.target, name, key);
            key = "engine default";
        }

        std::vector<const Decl*> ofKind;
        for (const Decl& d : in.decls) if (d.kind == kind) ofKind.push_back(&d);
        std::vector<std::string> declared;
        for (const Decl* d : ofKind) declared.push_back(d->name);

        kr.asked = name;
        kr.key = key;
        const Decl* chosen = nullptr;
        for (const Decl* d : ofKind) if (d->name == name) chosen = d;
        if (!chosen) {
            r.refusals.push_back("no " + kind + " provider named '" + name + "' (chosen by " + key +
                                 "; declared: " + (declared.empty() ? "none" : join(declared)) + ")");
        } else if (std::string why = whyNot(*chosen, in); !why.empty()) {
            r.refusals.push_back("the " + kind + " provider '" + name + "' (chosen by " + key +
                                 ") cannot be used for " + in.target.platform + " " + in.target.flavour +
                                 ": " + why);
        } else {
            kr.chosen = name;
        }
        for (const Decl* d : ofKind)
            if (d->name != name) kr.others.push_back({d->name, whyNot(*d, in)});
        r.kinds.push_back(std::move(kr));
    }
    if (!r.ok()) return r;

    // 7. Services: core's, plus whatever the chosen providers provide.
    std::vector<std::string> available = coreServices();
    std::vector<const Decl*> chosenDecls;
    for (const KindResult& kr : r.kinds)
        for (const Decl& d : in.decls)
            if (d.kind == kr.kind && d.name == kr.chosen) chosenDecls.push_back(&d);
    for (const Decl* d : chosenDecls)
        available.insert(available.end(), d->provides.begin(), d->provides.end());
    for (const Decl* d : chosenDecls)
        for (const std::string& s : d->requires_)
            if (!contains(available, s))
                r.refusals.push_back("the " + d->kind + " provider '" + d->name + "' requires the service '" +
                                     s + "', which neither core nor another chosen provider provides");
    return r;
}

bool declFromManifest(const nlohmann::json& m, Decl& d, std::string& why) {
    if (!m.is_object()) { why = "a manifest must be a JSON object"; return false; }
    for (const char* k : {"kind", "name", "contract"}) {
        auto it = m.find(k);
        if (it == m.end() || !it->is_string() || it->get<std::string>().empty()) {
            why = std::string("'") + k + "' is required";
            return false;
        }
    }
    d.kind = m["kind"].get<std::string>();
    d.name = m["name"].get<std::string>();
    d.contract = m["contract"].get<std::string>();
    bool ok = true;
    d.platforms = stringList(m, "platforms", why, ok);
    d.flavours  = stringList(m, "flavours", why, ok);
    d.needs     = stringList(m, "needs", why, ok);
    d.sdk       = stringList(m, "sdk", why, ok);
    d.requires_ = stringList(m, "requires", why, ok);
    d.provides  = stringList(m, "provides", why, ok);
    if (!ok) return false;
    if (!m.contains("flavours")) d.flavours = flavours();
    if (d.platforms.empty()) { why = "'platforms' is required"; return false; }
    return true;
}

bool platformFromTable(const nlohmann::json& t, Platform& p, std::string& why) {
    auto name = t.find("platform");
    auto facts = t.find("facts");
    if (name == t.end() || !name->is_string() || facts == t.end() || !facts->is_object()) {
        why = "a platform table needs 'platform' and 'facts'";
        return false;
    }
    p.name = name->get<std::string>();
    for (auto it = facts->begin(); it != facts->end(); ++it) {
        if (!it.value().is_boolean()) { why = "fact '" + it.key() + "' must be true or false"; return false; }
        p.facts[it.key()] = it.value().get<bool>();
    }
    return true;
}

std::string explainText(const Result& r) {
    std::string s = "providers for " + r.target.platform + " " + r.target.flavour + ":\n";
    for (const KindResult& k : r.kinds) {
        s += "  " + k.kind + std::string(k.kind.size() < 10 ? 10 - k.kind.size() : 1, ' ');
        s += k.chosen.empty() ? "(refused: " + k.asked + ", " + k.key + ")" : k.chosen + "  (" + k.key + ")";
        s += "\n";
        for (const Alternative& a : k.others)
            s += "      not " + a.name + ": " + (a.whyNot.empty() ? std::string("usable, not chosen") : a.whyNot) + "\n";
    }
    for (const std::string& why : r.refusals) s += "  REFUSED: " + why + "\n";
    return s;
}

nlohmann::json explainJson(const Result& r) {
    nlohmann::json j;
    j["platform"] = r.target.platform;
    j["flavour"] = r.target.flavour;
    j["ok"] = r.ok();
    j["refusals"] = r.refusals;
    nlohmann::json ks = nlohmann::json::array();
    for (const KindResult& k : r.kinds) {
        nlohmann::json others = nlohmann::json::array();
        for (const Alternative& a : k.others)
            others.push_back({{"name", a.name}, {"usable", a.whyNot.empty()}, {"why_not", a.whyNot}});
        ks.push_back({{"kind", k.kind}, {"chosen", k.chosen}, {"asked", k.asked}, {"key", k.key}, {"others", others}});
    }
    j["kinds"] = ks;
    return j;
}

} // namespace provsel
