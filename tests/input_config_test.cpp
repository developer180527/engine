// ── input_config_test — malformed input.json gauntlet (audit H.4) ───────────
// loadConfigText used unguarded std::stof/std::stoi and json::get<string>():
// a binding written as a number, a scale that overflows a float, or a field
// present with the wrong type threw an uncaught exception straight out of
// engine boot / editor config hot-reload. Every case below must be REJECTED
// or SKIPPED with a warning — never a throw, never a crash. Exits non-zero
// on first failure.
#include <cstdio>
#include <string>

#include "runtime/input/input_manager.h"

using input::InputManager;

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// Wrap one action with one binding list into a full config document.
static std::string cfg(const std::string& bindings) {
    return R"({"contexts":[{"name":"c","actions":[)"
           R"({"name":"a","type":"digital","bindings":)" + bindings +
           R"(}]}]})";
}

int main() {
    std::printf("input_config_test: malformed input.json gauntlet\n");

    // ── The crash corpus: every load must return, not throw ──────────────
    struct Case { const char* label; std::string text; };
    const Case corpus[] = {
        {"binding is a number",        cfg("[42]")},
        {"binding is an object",       cfg(R"([{"k":"v"}])")},
        {"binding is null",            cfg("[null]")},
        {"scale is garbage",           cfg(R"(["key:W:abc"])")},
        {"scale overflows float",      cfg(R"(["key:W:1e99"])")},
        {"scale is empty",             cfg(R"(["key:W:"])")},
        {"mouse button is garbage",    cfg(R"(["mouse:xyz"])")},
        {"mouse button overflows u16", cfg(R"(["mouse:99999999999999"])")},
        {"scroll scale garbage",       cfg(R"(["scroll:++"])")},
        {"context name wrong type",    R"({"contexts":[{"name":[1,2],"actions":[]}]})"},
        {"actions wrong type",         R"({"contexts":[{"name":"c","actions":"nope"}]})"},
        {"contexts is a string",       R"({"contexts":"nope"})"},
        {"empty bindings array",       cfg("[]")},
        {"not json at all",            "]]]]{{{{"},
        {"empty document",             ""},
    };
    for (const auto& c : corpus) {
        InputManager m;
        (void)m.loadConfigText(c.text);   // return value free; throwing is the bug
        CHECK(true, "%s: no throw", c.label);
    }

    // Wrong-typed "type" field specifically (present, not a string).
    {
        InputManager m;
        (void)m.loadConfigText(
            R"({"contexts":[{"name":"c","actions":[{"name":"a","type":7,"bindings":[]}]}]})");
        CHECK(true, "action type is a number: no throw");
    }

    // ── Degradation, not rejection: bad bindings skip, good ones load ────
    {
        InputManager m;
        bool ok = m.loadConfigText(cfg(R"(["key:W:notanumber", "key:W"])"));
        CHECK(ok, "config with one bad + one good binding still loads");
    }

    // ── Control: a fully valid config keeps loading ───────────────────────
    {
        InputManager m;
        bool ok = m.loadConfigText(
            R"({"contexts":[{"name":"gameplay","actions":[)"
            R"({"name":"jump","type":"digital","bindings":["key:Space"]},)"
            R"({"name":"look","type":"axis2","bindings":["mouse:motion:0.05"]},)"
            R"({"name":"zoom","type":"axis1","bindings":["scroll:2.5"]}]}]})");
        CHECK(ok, "valid config loads");
    }

    if (g_failures) { std::printf("input_config_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("input_config_test: ALL PASS\n");
    return 0;
}
