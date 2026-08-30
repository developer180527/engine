// ── ui_backend_test — the editor UI seam, crossed by something that is not ImGui
//
// `EngineApiUiV1` is frozen (offset 536, size 48) and every Kit that draws a
// tuning panel reaches it through `editorUi` in EngineGameModuleV1. Until this
// test, it had exactly ONE implementation — `s_uiBackend` in
// `src/editor/imgui/imgui_bgfx.cpp:264` — and one implementation is how a swap
// point becomes decoration. `provider-abi.md` §3 names the precedent by name:
// IRenderPipeline was a real seam with a single implementation and it decayed
// until a second one could not be written without leaking bgfx.
//
// The immediate reason to prove it now is a planned Rust/egui editor. The
// question that project has to answer first is whether the frozen UI ABI is
// secretly ImGui-shaped — because if it were, replacing the editor would break
// every kit, and the ABI cannot be un-frozen. It is not: the group is five
// functions over `const char*`, `float*` and `bool*`, with no windows, no
// docking, no draw lists and no IDs. This test is what turns that from a
// reading of the header into a fact about the build.
//
// WHAT IS DELIBERATELY NOT TESTED: ImGui. The editor keeps its ImGui backend and
// stays the working demo. This asserts the seam admits a SECOND backend, which
// is the property the rewrite depends on — not that the first one should go.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <engine/engine_api.h>
#include <engine/engine_api_table.h>

// HOST-INTERNAL, not part of the public SDK: the predicate the API table uses to
// decide whether to publish the ui group. Forward-declared here exactly as
// engine_api_table.cpp:8 does it, because this test is host-side code asserting
// a host-side invariant. A kit could not — and should not — reach it; a kit reads
// `ui.version` instead, which §2 below asserts is the same answer.
bool engineUiHasBackend(void);   // C++ linkage, as engine_api_table.cpp declares it

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

// ── A backend that is not ImGui, and could not be ─────────────────────────────
// It records. That is the whole implementation, and the point: a backend needs
// no windowing, no draw list, no immediate-mode context and no frame — only five
// functions over C types. An egui backend is this shape with `ui.label(..)` in
// place of the push_back.
namespace {

struct Recorder {
    std::vector<std::string> calls;
    // Values the widgets should write THROUGH the pointers they are handed.
    // A backend that only reported clicks and never wrote back would satisfy a
    // weaker test while being useless to a kit.
    float  sliderWrites   = 0.0f;
    bool   checkboxWrites = false;
    bool   buttonReturns  = false;
    bool   sliderChanged  = false;
    bool   checkboxChanged= false;
};
Recorder g_rec;

void recText(const char* s)  { g_rec.calls.push_back(std::string("text:") + s); }
bool recButton(const char* label) {
    g_rec.calls.push_back(std::string("button:") + label);
    return g_rec.buttonReturns;
}
bool recSlider(const char* label, float* v, float lo, float hi) {
    g_rec.calls.push_back(std::string("slider:") + label);
    if (v) *v = g_rec.sliderWrites;            // the backend OWNS the edit
    (void)lo; (void)hi;
    return g_rec.sliderChanged;
}
bool recCheckbox(const char* label, bool* v) {
    g_rec.calls.push_back(std::string("checkbox:") + label);
    if (v) *v = g_rec.checkboxWrites;
    return g_rec.checkboxChanged;
}
void recSeparator(void) { g_rec.calls.push_back("separator"); }

const EngineUiBackend kRecordingBackend = {
    recText, recButton, recSlider, recCheckbox, recSeparator
};

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("ui_backend_test: a second EngineUiBackend, with no ImGui in it\n");

    // ── 1. No backend: every call is a safe no-op ───────────────────────────
    // This is the headless host (engine_host, a dedicated server) and it is the
    // state the process starts in. A kit that calls engineUi* there must not
    // crash and must not be able to tell it is being ignored, beyond the
    // capability bit checked below.
    {
        std::printf("\n-- 1. no backend --\n");
        engineUiSetBackend(nullptr);
        CHECK(!engineUiHasBackend(), "no backend registered at start");

        float f = 3.0f; bool b = true;
        engineUiText("anything %d", 1);
        const bool clicked = engineUiButton("go");
        const bool slid    = engineUiSliderFloat("s", &f, 0.0f, 1.0f);
        const bool checked = engineUiCheckbox("c", &b);
        engineUiSeparator();

        CHECK(!clicked && !slid && !checked,
              "widgets report NOT interacted rather than garbage");
        // The false direction matters more than it looks: a no-op that reported
        // `true` would make a kit act on a click nobody made, in a host with no
        // UI at all.
        CHECK(f == 3.0f && b == true,
              "and the caller's values are untouched (f=%.1f b=%d)", f, (int)b);
    }

    // ── 2. The capability bit, which is what makes a swap visible ───────────
    // engine_api_table.cpp publishes `ui.version = 0` when no backend is live,
    // so a kit sees the group as ABSENT and skips building its panel. That is
    // the same mechanism a new editor uses to light the group back up — the
    // reason a Rust editor does not need a kit rebuild.
    {
        std::printf("\n-- 2. capability negotiation --\n");
        engineUiSetBackend(nullptr);
        CHECK(engineApiHostTable()->ui.version == 0,
              "ui.version is 0 with no backend — the group reads as absent");

        engineUiSetBackend(&kRecordingBackend);
        CHECK(engineUiHasBackend(), "backend registered");
        CHECK(engineApiHostTable()->ui.version == ENGINE_API_UI_V,
              "ui.version becomes %d once ANY backend is live — no rebuild, no "
              "ImGui", ENGINE_API_UI_V);
    }

    // ── 3. The facade routes to whatever is registered ──────────────────────
    {
        std::printf("\n-- 3. routing --\n");
        g_rec = Recorder{};
        engineUiSetBackend(&kRecordingBackend);

        g_rec.buttonReturns   = true;
        g_rec.sliderChanged   = true;
        g_rec.checkboxChanged = true;
        g_rec.sliderWrites    = 0.25f;
        g_rec.checkboxWrites  = true;

        float f = 9.0f; bool b = false;
        engineUiText("hp %d/%d", 7, 10);
        const bool clicked = engineUiButton("respawn");
        const bool slid    = engineUiSliderFloat("speed", &f, 0.0f, 1.0f);
        const bool checked = engineUiCheckbox("godmode", &b);
        engineUiSeparator();

        CHECK(g_rec.calls.size() == 5, "all five widgets reached the backend (%zu)",
              g_rec.calls.size());
        CHECK(clicked && slid && checked,
              "return values come BACK from the backend");
        CHECK(f == 0.25f && b == true,
              "and the backend's edits reach the kit's variables (f=%.2f b=%d)",
              f, (int)b);

        // Order matters: a kit draws a panel top to bottom and a backend that
        // reordered or batched would produce a different UI than the kit wrote.
        const char* expect[] = { "text:hp 7/10", "button:respawn", "slider:speed",
                                 "checkbox:godmode", "separator" };
        bool ordered = g_rec.calls.size() == 5;
        for (size_t i = 0; ordered && i < 5; ++i)
            ordered = g_rec.calls[i] == expect[i];
        CHECK(ordered, "in the order the kit issued them");
    }

    // ── 4. Formatting is the HOST's job, not the backend's ─────────────────
    // engineUiText is variadic; EngineUiBackend::text takes one pre-formatted
    // string. That split is deliberate and load-bearing for a non-C backend: a
    // Rust or C# host cannot consume a C va_list, so the facade must have done
    // the printf before the boundary. The header says "PRE-FORMATTED — the
    // client shim printf's"; this is that comment, enforced.
    {
        std::printf("\n-- 4. pre-formatted text --\n");
        g_rec = Recorder{};
        engineUiSetBackend(&kRecordingBackend);
        engineUiText("%s=%d", "count", 42);
        CHECK(g_rec.calls.size() == 1 && g_rec.calls[0] == "text:count=42",
              "the backend receives finished text, never a format string (%s)",
              g_rec.calls.empty() ? "(none)" : g_rec.calls[0].c_str());
    }

    // ── 5. Swapping backends mid-flight ────────────────────────────────────
    // The rewrite's actual shape: one backend retires, another takes over, and
    // no kit is rebuilt. Also the teardown path — a backend must be able to
    // deregister before its own memory goes away, or the facade holds a
    // dangling pointer for the life of the process.
    {
        std::printf("\n-- 5. swap and retire --\n");
        g_rec = Recorder{};
        engineUiSetBackend(&kRecordingBackend);
        engineUiButton("first");
        CHECK(g_rec.calls.size() == 1, "backend A saw the call");

        engineUiSetBackend(nullptr);
        engineUiButton("second");
        CHECK(g_rec.calls.size() == 1,
              "after deregistering, A sees nothing more (%zu)", g_rec.calls.size());
        CHECK(engineApiHostTable()->ui.version == 0,
              "and the group reads absent again — a host can retire its UI");

        engineUiSetBackend(&kRecordingBackend);
        engineUiButton("third");
        CHECK(g_rec.calls.size() == 2, "and a backend can be registered again");
        engineUiSetBackend(nullptr);
    }

    // ── 6. A PARTIAL backend is legal ──────────────────────────────────────
    // `provider-abi.md` rule 7: "unsupported is a normal answer", and it is what
    // makes an incremental port possible — an egui backend can land `text` and
    // `button` first and fill in the rest later without crashing a kit that
    // calls all five. Every function pointer is null-checked by the facade.
    {
        std::printf("\n-- 6. partial backend --\n");
        const EngineUiBackend partial = { recText, nullptr, nullptr, nullptr, nullptr };
        g_rec = Recorder{};
        engineUiSetBackend(&partial);

        float f = 5.0f; bool b = false;
        engineUiText("still works");
        const bool clicked = engineUiButton("unimplemented");
        const bool slid    = engineUiSliderFloat("also", &f, 0.0f, 1.0f);
        engineUiCheckbox("and", &b);
        engineUiSeparator();

        CHECK(g_rec.calls.size() == 1, "the implemented function ran (%zu)",
              g_rec.calls.size());
        CHECK(!clicked && !slid && f == 5.0f && b == false,
              "the unimplemented ones no-op instead of crashing");
        engineUiSetBackend(nullptr);
    }

    if (g_failures) {
        std::printf("\nui_backend_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nui_backend_test: ALL PASS — the UI seam admits a non-ImGui backend\n");
    return 0;
}
