// ── engine_module_probe — run the module load gauntlet and say what happened ─
//
// Loads one or more shared libraries through the REAL `ModuleLibrary::load` and
// reports, per module, whether the host accepted or refused it. Two jobs, and
// the second is the reason this is a program rather than a test:
//
//  1. A diagnostic. "My kit won't load" is a question the engine could only
//     answer by running the editor and reading the log. This answers it in one
//     command, against the same code path the editor uses, with no GPU, no
//     window and no project.
//
//  2. A TESTABLE BOUNDARY. The gauntlet in module_loader.h is C++ internals —
//     there is no C entry point to call and inventing one would mean writing
//     untested C++ for the privilege of testing in Rust. A command line is a
//     boundary that already has to be stable, the same way engine_cook_worker's
//     is, so the Rust suite drives this and no shim exists purely to be tested.
//
// ── This is the first Add-on ────────────────────────────────────────────────
// `extension-model.md` designed a fourth extension tier and did not build it:
// the Add-on — separate process, protocol-versioned, a crash that takes down
// only itself, "for: tools". `tool-ecosystem.md` §8 says to prove that protocol
// by converting a tool that already exists rather than inventing one to
// demonstrate it, because only an existing tool can push back on a bad design.
//
// This is that conversion. The protocol is `engine/addon_protocol.h` and this
// probe is its first speaker.
//
// ── What the conversion actually fixed ──────────────────────────────────────
// The old contract put the machine-readable verdicts on STDOUT. That was wrong,
// and not theoretically:
//
//     This program dlopens code it does not trust. That is the entire reason it
//     exists. And loaded code can PRINT.
//
// A module's static initialiser, or its `create`, can write `module 0 ok /x` to
// the same stdout the probe writes its own verdicts to — and nothing downstream
// can tell the two apart. Not noise: forgery. A parser reading that stream saw
// two verdicts where one module was probed, or a refusal reported as an accept,
// depending on what the module chose to print. `engine_cook_worker` had already
// written the rule down — "cookers print freely, so stdout is not a channel" —
// and the probe, whose untrusted-code exposure is worse, had not applied it.
//
// `abi_gate_noisy_stdout` is a fixture that does exactly this, and the
// conformance suite now requires the verdicts to survive it.
//
// ── Output contract ─────────────────────────────────────────────────────────
// HUMAN, on stdout — unchanged, and never parsed by anything:
//
//     module <index> <ok|refused> <path>
//     probe done
//
// The host's own LOG_ERROR explanation goes to stderr, untouched — that is the
// half a human wants and the half a test must not parse.
//
// MACHINE, in the file named by `--addon-result=<path>`, framed per the Add-on
// protocol with a digest trailer:
//
//     ENGINE_ADDON_RESULT 1
//     VERDICT ok
//     MODULE <index> <ok|refused> <path>
//     END <lines> <hex>
//
// `--addon-manifest` prints the tool's self-description and exits, loading
// nothing.
//
// Exit status is 0 whenever the probe RAN, refusals included: a refusal is a
// successful probe of a bad module, and collapsing it into a non-zero exit would
// make it indistinguishable from a crash. 2 is usage, 3 is a path that is not
// there, 4 is the probe failing to write its own result. That separation is what
// lets the test suite assert that a rejected module is rejected CLEANLY rather
// than by falling over.
//
// ── Why several modules in one run ──────────────────────────────────────────
// The kit-to-kit contract check compares a module's declarations against a
// registry that starts EMPTY, so a single module can never fail it — the first
// one to declare a contract is the one that pins it. Provoking a mismatch needs
// two modules in one process, in order, with the first still loaded. Every
// library stays alive until the probe exits for exactly that reason.
#include "runtime/module_loader.h"

#include <engine/addon_protocol.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace addon = engine::addon;

static int usage() {
    std::fprintf(stderr,
        "usage: engine_module_probe [options] <module> [module...]\n"
        "       engine_module_probe --addon-manifest\n"
        "\n"
        "  Loads each module through the engine's compatibility gauntlet and\n"
        "  prints 'module <i> ok|refused <path>' for each, in order.\n"
        "\n"
        "  --reload=never          dlopen the file in place (what a shipped player does)\n"
        "  --reload=allowed        copy to temp first, the editor's hot-reload path (default)\n"
        "  --addon-result=<path>   write the machine-readable result there. Stdout is a\n"
        "                          human channel: this probe loads untrusted modules and\n"
        "                          they can print, so stdout cannot be parsed.\n"
        "  --addon-manifest        print this tool's Add-on manifest and exit\n");
    return addon::exitCode(addon::Exit::Usage);
}

// What the probe claims about itself, for a host that has not been taught about
// this specific binary. `RECORD MODULE` is the load-bearing line: it means a
// suite that parses `MODULE` records can assert the tool still says it emits
// them, rather than silently reading zero records off a renamed key.
static void emitManifest() {
    addon::Result m;
    m.verdict(addon::Verdict::Ok);
    m.record("ID",       "engine_module_probe");
    m.record("TOOL",     "1");
    m.record("RECORD",   "MODULE");
    m.record("CONSUMES", "module-library");
    m.record("PRODUCES", "module-verdict");
    addon::writeManifest(m);
}

using modload::ModuleLibrary;

int main(int argc, char** argv) {
    auto reload = ModuleLibrary::Reload::Allowed;
    std::vector<fs::path> paths;
    std::string resultPath;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--reload=never") == 0) {
            reload = ModuleLibrary::Reload::Never;
        } else if (std::strcmp(a, "--reload=allowed") == 0) {
            reload = ModuleLibrary::Reload::Allowed;
        } else if (std::strcmp(a, "--addon-manifest") == 0) {
            emitManifest();
            return addon::exitCode(addon::Exit::Ran);
        } else if (std::strncmp(a, "--addon-result=", 15) == 0) {
            resultPath = a + 15;
            if (resultPath.empty()) {
                std::fprintf(stderr, "engine_module_probe: --addon-result= needs a path\n");
                return usage();
            }
        } else if (a[0] == '-') {
            std::fprintf(stderr, "engine_module_probe: unknown option '%s'\n", a);
            return usage();
        } else {
            paths.emplace_back(a);
        }
    }
    if (paths.empty()) return usage();

    // Removed BEFORE any work, so a result file can never outlive the run that
    // wrote it. Exit 3 and exit 4 both mean "do not read a result", but a host
    // that forgets to check the status would otherwise read a PREVIOUS run's
    // answer — a stale success is the one failure mode the digest trailer cannot
    // catch, because the file it describes is perfectly intact.
    if (!resultPath.empty()) {
        std::error_code ec;
        fs::remove(resultPath, ec);
    }

    // Checked up front, before anything is loaded. A missing file would
    // otherwise surface as an ordinary refusal, and "the gate rejected this
    // module" is a very different fact from "there was no module".
    for (const fs::path& p : paths) {
        if (!fs::exists(p)) {
            std::fprintf(stderr, "engine_module_probe: no such file: %s\n",
                         p.string().c_str());
            return addon::exitCode(addon::Exit::MissingInput);
        }
    }

    // The probe ran, so the verdict is `ok` regardless of what the gauntlet
    // decides about any individual module. Per-module outcomes are records.
    addon::Result result;
    result.verdict(addon::Verdict::Ok);

    // Held for the whole run: a module unloaded early would release its contract
    // pins, and the next module would then find an empty registry and load
    // clean — the exact skew the contract check exists to catch.
    std::vector<std::unique_ptr<ModuleLibrary>> held;
    held.reserve(paths.size());

    for (std::size_t i = 0; i < paths.size(); ++i) {
        auto lib = std::make_unique<ModuleLibrary>();
        const bool ok = lib->load(paths[i], reload);
        const char* verdict = ok ? "ok" : "refused";

        std::printf("module %zu %s %s\n", i, verdict, paths[i].string().c_str());
        std::fflush(stdout);

        result.record("MODULE", std::to_string(i) + " " + verdict + " "
                                + paths[i].string());
        held.push_back(std::move(lib));
    }

    // A terminator on the HUMAN channel, so a person reading a truncated run can
    // see it was truncated. The machine channel does not need it — the framed
    // result's line count and digest already make a partial write detectable,
    // which is strictly stronger than a sentinel that a dying process might
    // still manage to print.
    std::printf("probe done\n");
    std::fflush(stdout);

    if (!resultPath.empty()) {
        std::string err;
        if (!result.writeTo(resultPath, err)) {
            // The probe did its job and cannot report it. That is the tool
            // failing, not a verdict about any module, so it is Failed rather
            // than a result file saying `fail` — there is, by definition, no
            // result file to say it in.
            std::fprintf(stderr, "engine_module_probe: %s\n", err.c_str());
            return addon::exitCode(addon::Exit::Failed);
        }
    }
    return addon::exitCode(addon::Exit::Ran);
}
