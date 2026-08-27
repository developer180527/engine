// ── The Add-on protocol, tested directly ────────────────────────────────────
//
// Until now the protocol had only INCIDENTAL coverage: the Rust conformance
// suite reads frames the probe wrote, and cook_hardening_test compares its
// framing against assetlib's. Both are valuable and neither tests the writer's
// own rules — ordering, sanitising, the missing-verdict fallback, the split
// between `record` and `recordExact`. Those are pure functions with no I/O, so
// there was no reason for them to be untested other than nobody having done it.
//
// The two halves here:
//
//   1. THE WRITER'S RULES. What `Result` guarantees structurally rather than by
//      documentation, because "the tool must remember to" is not a guarantee.
//   2. THE SECOND SPEAKER'S CONTRACT. engine_build's exit codes and manifest,
//      checked by running it. Not its packaging — package_closure_test covers
//      that, and a real package needs a cmake build of the whole engine. What is
//      checked is the part a CALLER depends on and nothing else verifies.
#include <engine/addon_protocol.h>

#include "test_watchdog.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if !defined(_WIN32)
// WIFEXITED/WEXITSTATUS. std::system returns a wait status on POSIX, not an exit
// code, and reading it as one makes every non-zero status look like a different
// number than the tool actually returned.
#  include <sys/wait.h>
#endif

namespace fs = std::filesystem;
namespace addon = engine::addon;

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// Body lines of a framed result, or empty if the frame does not validate.
static std::vector<std::string> bodyLines(const std::string& framed) {
    std::string body, err;
    if (!addon::unframe(addon::kResultMagic, framed, body, err)) return {};
    std::vector<std::string> out;
    size_t start = 0;
    while (start < body.size()) {
        const size_t nl = body.find('\n', start);
        if (nl == std::string::npos) break;
        out.push_back(body.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

// ── 1. The writer's rules ───────────────────────────────────────────────────
static void testWriterRules() {
    std::printf("\n-- writer rules --\n");

    // ORDER IS STRUCTURAL. Set deliberately backwards: records first, then the
    // error, then the verdict last. The emitted body must still lead with
    // VERDICT — the whole value of the trailer depends on the verdict being the
    // FIRST line, because a truncated file is then missing the one line a host
    // needs rather than keeping it and losing the records.
    {
        addon::Result r;
        r.record("STEP", "one");
        r.error("something went wrong");
        r.verdict(addon::Verdict::Fail);
        const auto lines = bodyLines(r.framed());
        CHECK(lines.size() == 3, "a verdict, an error and a record make 3 lines");
        CHECK(!lines.empty() && lines[0] == "VERDICT fail",
              "VERDICT is first even when it was set LAST");
        CHECK(lines.size() > 1 && lines[1] == "ERROR something went wrong",
              "...and ERROR is second");
    }

    // A MISSING VERDICT IS `fail`, NOT ABSENT. A body with no VERDICT would be
    // rejected wholesale by every reader — correctly reporting "the tool is
    // broken", but burying the records the tool did produce. `fail` keeps them
    // readable and still cannot be mistaken for success.
    {
        addon::Result r;
        r.record("STEP", "one");
        const auto lines = bodyLines(r.framed());
        CHECK(!lines.empty() && lines[0] == "VERDICT fail",
              "a Result with no verdict set emits `fail`, not nothing");
    }

    // SANITISING, for messages. A newline in an error message would otherwise
    // forge a second record in a body whose line count and digest BOTH agree,
    // because the writer computes them after the injection.
    {
        addon::Result r;
        r.verdict(addon::Verdict::Fail);
        r.error("first line\nVERDICT ok");
        const auto lines = bodyLines(r.framed());
        CHECK(lines.size() == 2,
              "an error containing a newline still produces exactly 2 lines");
        CHECK(lines.size() > 1 &&
              lines[1].find("VERDICT ok") != std::string::npos &&
              lines[1].rfind("ERROR ", 0) == 0,
              "...and the forged text stays INSIDE the ERROR record");
    }

    // The frame must survive an empty body — a verdict-only result is the normal
    // shape for most tools most of the time.
    {
        addon::Result r;
        r.verdict(addon::Verdict::Ok);
        std::string body, err;
        CHECK(addon::unframe(addon::kResultMagic, r.framed(), body, err),
              "a verdict-only result validates");
        CHECK(body == "VERDICT ok\n", "...and its body is exactly the verdict");
    }

    // ── recordExact: refuse, do not mangle ──────────────────────────────────
    // The distinction the SECOND tool to speak this protocol needed and the
    // first one did not. A mangled message is a slightly odd sentence; a mangled
    // PATH is a file the caller cannot open, which turns a precise failure into
    // a silent wrong answer.
    {
        addon::Result r;
        r.verdict(addon::Verdict::Ok);
        CHECK(r.recordExact("DIST", "/tmp/dist"),
              "recordExact accepts an ordinary path");
        CHECK(!r.recordExact("DIST", "/tmp/ev\nVERDICT fail"),
              "recordExact REFUSES a path containing a newline");
        const auto lines = bodyLines(r.framed());
        CHECK(lines.size() == 2,
              "...and emits nothing for it: 1 verdict + 1 accepted record");

        CHECK(addon::Result::carryable("plain/path"), "carryable() accepts text");
        CHECK(!addon::Result::carryable("has\ttab"),
              "carryable() rejects a tab, not only a newline");
        CHECK(!addon::Result::carryable(std::string("has\0nul", 7)),
              "carryable() rejects an embedded NUL");
        CHECK(addon::Result::carryable("/tmp/my dir/x"),
              "carryable() ALLOWS a space — a value runs to end of line, and "
              "refusing spaces would refuse every path that has one");
    }

    // ── The key is the other half, and it was unguarded ─────────────────────
    // A record is `KEY value` split on the FIRST space, so a key containing one
    // is re-cut rather than carried: `MY KEY v` reads back as key `MY`, value
    // `KEY v`. That is the same silent wrong answer recordExact was split out of
    // record to prevent — the value was guarded and the key was not.
    //
    // Not reachable from this tree today; every call site passes a literal. It
    // is pinned because "no caller does that yet" is not a property of the
    // function, and this one's entire job is to refuse rather than mangle.
    {
        addon::Result r;
        r.verdict(addon::Verdict::Ok);
        CHECK(!r.recordExact("MY KEY", "/tmp/x"),
              "recordExact REFUSES a key containing a space");
        CHECK(!r.recordExact("", "/tmp/x"),
              "recordExact REFUSES an empty key");
        CHECK(!r.recordExact("BAD\nKEY", "/tmp/x"),
              "recordExact REFUSES a key containing a newline");
        CHECK(bodyLines(r.framed()).size() == 1,
              "...and emits nothing for any of them: the verdict alone");

        CHECK(addon::Result::usableKey("DIST"), "usableKey() accepts a plain key");
        CHECK(!addon::Result::usableKey("HAS SPACE"),
              "usableKey() rejects the delimiter itself, which carryable() must not");
    }

    // `record` cannot refuse — it returns void and its contract is that it always
    // emits, so dropping a warning would be worse than an ugly key. It mangles
    // the space instead, which keeps the record parseable and the mistake visible.
    {
        addon::Result r;
        r.verdict(addon::Verdict::Ok);
        r.record("MY KEY", "a message with spaces");
        const auto lines = bodyLines(r.framed());
        CHECK(lines.size() == 2 && lines[1] == "MY_KEY a message with spaces",
              "record() mangles a space in the KEY to '_' and leaves the VALUE's "
              "spaces alone — a key's space is a delimiter, a value's is text");
    }

    // Exactness is the point: recordExact must not alter what it accepts.
    {
        addon::Result r;
        r.verdict(addon::Verdict::Ok);
        const std::string weird = "/tmp/a b/ünïcode-ok/x.dist";
        CHECK(r.recordExact("DIST", weird),
              "recordExact accepts spaces and multi-byte UTF-8");
        const auto lines = bodyLines(r.framed());
        CHECK(lines.size() == 2 && lines[1] == "DIST " + weird,
              "...and carries them byte for byte");
    }
}

// ── 2. Exit codes ───────────────────────────────────────────────────────────
static void testExitContract() {
    std::printf("\n-- exit contract --\n");

    CHECK(addon::exitCode(addon::Exit::Ran) == 0, "Ran is 0");
    CHECK(addon::reachedVerdict(0), "0 means a result file can be trusted");
    CHECK(addon::reachedVerdict(addon::exitCode(addon::Exit::RanWithFailure)),
          "...and so does RanWithFailure — both mean the tool RAN");
    CHECK(!addon::reachedVerdict(addon::exitCode(addon::Exit::Usage)),
          "Usage does not");
    CHECK(!addon::reachedVerdict(addon::exitCode(addon::Exit::MissingInput)),
          "MissingInput does not");
    CHECK(!addon::reachedVerdict(addon::exitCode(addon::Exit::Failed)),
          "Failed does not — the tool broke, there is nothing to read");

    // 1 IS UNASSIGNED ON PURPOSE. It is the exit code of every accident — a bare
    // `return 1`, an uncaught exception, a wrapper that lost the real status — so
    // giving it meaning would make the commonest accident indistinguishable from
    // a documented outcome. This pins that decision: if someone later adds an
    // Exit == 1, this fails and they have to read the comment explaining why.
    CHECK(!addon::reachedVerdict(1),
          "1 is not a verdict-reaching status (it is deliberately unassigned)");
    for (auto e : { addon::Exit::Ran, addon::Exit::Usage, addon::Exit::MissingInput,
                    addon::Exit::Failed, addon::Exit::RanWithFailure })
        CHECK(addon::exitCode(e) != 1, "no Exit value is 1");
}

// ── 3. engine_build's side of the contract ──────────────────────────────────
// Run only when CMake pointed at the binary. Skipping is correct for a bare
// `./addon_protocol_test` from a shell; under ctest the path is always set, and
// then a missing binary is a finding rather than a shrug — same rule as the
// cooked-format and module-ABI lanes.
static void testEngineBuildContract() {
    std::printf("\n-- engine_build add-on contract --\n");

    const char* exe = std::getenv("ENGINE_BUILD_TOOL");
    if (!exe || !*exe || !fs::exists(exe)) {
        if (std::getenv("ENGINE_REQUIRE_ADDON_TOOL_TESTS")) {
            std::printf("  FAIL  ENGINE_BUILD_TOOL=%s is not a file, and this run "
                        "was expected to have it\n", exe ? exe : "(unset)");
            ++g_failures;
        } else {
            std::printf("  skip  ENGINE_BUILD_TOOL unset — run through ctest\n");
        }
        return;
    }
    const std::string tool = std::string("\"") + exe + "\"";
    const fs::path tmp = fs::temp_directory_path() / "engine_addon_proto_test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    auto runTool = [&](const std::string& args) {
        // Output silenced: this test is about statuses and files, and the tool's
        // human channel is deliberately chatty.
        const std::string cmd = tool + " " + args + " > " +
#if defined(_WIN32)
            "NUL 2>&1";
#else
            "/dev/null 2>&1";
#endif
        const int rc = std::system(cmd.c_str());
#if defined(_WIN32)
        return rc;
#else
        return (rc >= 0 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
#endif
    };

    // No arguments: the command line is wrong, nothing ran.
    CHECK(runTool("") == addon::exitCode(addon::Exit::Usage),
          "no arguments exits Usage(2)");
    CHECK(runTool("someproject --bogus-flag") == addon::exitCode(addon::Exit::Usage),
          "an unknown option exits Usage(2)");

    // A directory with no project.json: the command line was fine and the named
    // input was not there. A caller can act on that difference — one is its own
    // bug, the other is the project's.
    const fs::path empty = tmp / "no_project";
    fs::create_directories(empty, ec);
    const fs::path res = tmp / "r.txt";
    { std::ofstream stale(res); stale << "STALE\n"; }   // must be removed, not read
    CHECK(runTool("\"" + empty.string() + "\" \"--addon-result=" + res.string() + "\"")
              == addon::exitCode(addon::Exit::MissingInput),
          "a directory with no project.json exits MissingInput(3), not Usage");
    CHECK(!fs::exists(res),
          "...and the STALE result file was deleted rather than left for a caller "
          "that forgets to check the status — an intact stale success is the one "
          "failure the digest trailer cannot catch");

    // ── The manifest, cross-checked ─────────────────────────────────────────
    // Read back through the real parser, so a manifest that does not validate is
    // a failure rather than a string this test happens to find substrings in.
    const fs::path manOut = tmp / "manifest.txt";
    const std::string manCmd = tool + " --addon-manifest > \"" + manOut.string() + "\" 2>&1";
    const int manRc = std::system(manCmd.c_str());
    CHECK(manRc == 0, "--addon-manifest exits 0");

    std::ifstream mf(manOut, std::ios::binary);
    const std::string manRaw((std::istreambuf_iterator<char>(mf)),
                              std::istreambuf_iterator<char>());
    std::string manBody, manErr;
    CHECK(addon::unframe(addon::kManifestMagic, manRaw, manBody, manErr),
          "...and its output is a valid manifest frame: %s",
          manErr.empty() ? "ok" : manErr.c_str());

    // The keys a caller depends on. WARNING is the load-bearing one: it is the
    // record that makes twelve defects-in-the-shipped-package visible to
    // something other than a human reading scrollback.
    for (const char* key : { "RECORD WARNING", "RECORD WARNINGS", "RECORD DIST",
                             "ID engine_build" })
        CHECK(manBody.find(std::string(key) + "\n") != std::string::npos,
              "the manifest declares \"%s\"", key);

    fs::remove_all(tmp, ec);
}

int main() {
    testwd::begin("addon_protocol_test", 60);
    testWriterRules();
    testExitContract();
    testEngineBuildContract();
    testwd::end();
    if (g_failures) {
        std::printf("\naddon_protocol_test: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("\naddon_protocol_test: all checks passed\n");
    return 0;
}
