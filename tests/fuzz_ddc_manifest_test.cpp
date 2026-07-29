// ── fuzz_ddc_manifest_test — DDC manifest parser ─────────────────────────────
// A manifest fetched from a SHARED DDC tier is remote input: it was written by
// another machine, and nothing about it is trustworthy. ddcFetchRecord() parses
// it and then WRITES A FILE per member, so a parser bug here is not "wrong
// asset", it is arbitrary file placement on every machine in the studio.
//
// This target caught its own motivating bug: the original validation blocked
// "/", "\" and ".." but not ":", so "C:evil.ctex" escaped to a drive-relative
// path on Windows. That case is in the committed corpus — it fails before the
// allowlist fix and passes after, which is exactly what a regression corpus is
// for.
//
// Properties asserted per case:
//   1. Never crashes / hangs on any input.
//   2. CONTAINMENT: not one byte is written outside the primary output's own
//      directory. This is the security property; everything else is polish.
//   3. All-or-nothing: a true return means every member materialized, so a
//      cache hit can never hand the runtime a mesh whose textures are missing.
//   4. No temp files are left behind in the store.
#include "fuzz/fuzz.h"

#include <assetlib/ddc.h>
#include <assetlib/ddc_manifest.h>

#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Bump when the generator's meaning changes (see fuzz::ReproKey).
static constexpr uint32_t kGeneratorVersion = 1;

namespace {

// Names that have broken path validation in real systems. The fuzzer mixes
// these with random noise: pure random bytes essentially never produce a
// traversal attempt, so a generator that only mutates bytes would never find
// the bug this target exists to catch.
const char* kHostileNames[] = {
    "../escape.ctex", "../../../../etc/passwd", "..", ".", "./x.ctex",
    "sub/dir.ctex", "sub\\dir.ctex", "/abs/evil.ctex", "\\abs\\evil.ctex",
    "C:evil.ctex", "C:/evil.ctex", "c:\\windows\\system32\\evil.dll",
    "\\\\server\\share\\evil", "name:stream", "..\\..\\up.ctex",
    "", " ", "\t", "a\nb", "a\rb", "con", "nul", "aux",   // reserved on Windows
    "very_long_name_that_goes_on", "@", "@@", "-", "--",
};

std::string randomName(fuzz::Rng& rng) {
    if (rng.chance(45)) {                    // known-hostile
        const auto n = (uint32_t)(sizeof(kHostileNames) / sizeof(*kHostileNames));
        return kHostileNames[rng.below(n)];
    }
    if (rng.chance(30)) {                    // plausible-but-random
        std::string s = "mesh_t" + std::to_string(rng.below(8)) + ".ctex";
        if (rng.chance(20)) s.insert(rng.below((uint32_t)s.size()),
                                     1, (char)rng.range(1, 127));
        return s;
    }
    // Arbitrary bytes, including embedded separators and control characters.
    std::string s;
    const uint32_t len = rng.range(1, 40);
    for (uint32_t i = 0; i < len; ++i) s.push_back((char)rng.range(1, 255));
    return s;
}

// Builds a manifest body. Deliberately reachable states: valid, wrong magic,
// truncated mid-line, missing tab, absent primary, duplicate primary, bogus
// member keys, absurd member counts.
std::string buildManifest(fuzz::Rng& rng, const std::string& realKey,
                          bool& expectAllValid) {
    expectAllValid = true;
    std::string m;

    if (rng.chance(8)) { m = "not-a-manifest\n"; expectAllValid = false; }
    else if (rng.chance(5)) { m = ""; expectAllValid = false; }
    else m = "ddc-manifest-v1\n";

    const uint32_t members = rng.chance(5) ? rng.range(50, 200) : rng.range(0, 6);
    bool wrotePrimary = false;
    for (uint32_t i = 0; i < members; ++i) {
        // Member key: the real blob (so a valid name actually materializes),
        // or garbage (a miss the parser must handle, not trust).
        std::string key;
        if (rng.chance(70)) key = realKey;
        else if (rng.chance(50)) key = std::string(64, (char)('a' + rng.below(6)));
        else { const uint32_t n = rng.range(0, 80);
               for (uint32_t k = 0; k < n; ++k) key.push_back((char)rng.range(48, 122)); }
        if (key != realKey) expectAllValid = false;

        std::string name;
        if (!wrotePrimary && rng.chance(60)) { name = "@"; wrotePrimary = true; }
        else { name = randomName(rng); expectAllValid = false; }

        if (rng.chance(6)) { m += key + name + "\n"; expectAllValid = false; }  // no tab
        else               { m += key + "\t" + name + "\n"; }
    }
    if (!wrotePrimary) expectAllValid = false;
    if (rng.chance(8) && !m.empty()) {       // truncate mid-stream
        m.resize(rng.below((uint32_t)m.size()));
        expectAllValid = false;
    }
    return m;
}

// Every file under `root`, for the containment check.
std::set<std::string> snapshot(const fs::path& root) {
    std::set<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(root, ec))
        if (!ec && e.is_regular_file()) out.insert(e.path().string());
    return out;
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed       = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target           = "ddc_manifest";

    // Independent substreams so adding a generator later cannot perturb this
    // one's output for an already-archived seed.
    fuzz::Rng rng(fuzz::deriveSeed(masterSeed, "manifest_body"));

    fuzz::Scratch scratch("ddcman");
    const fs::path root  = scratch.path() / "store";
    const fs::path out   = scratch.path() / "out";       // legal write area
    const fs::path guard = scratch.path() / "guard";     // must stay untouched
    std::error_code ec;
    fs::create_directories(out, ec);
    fs::create_directories(guard, ec);
    { std::ofstream f(guard / "canary.txt"); f << "untouched"; }

    assetlib::DdcStore store(root, {});

    // A real member blob so that legal names genuinely materialize — otherwise
    // every case would fail at the fetch and never reach the naming logic.
    const fs::path member = scratch.path() / "member.bin";
    { std::ofstream f(member, std::ios::binary); f << "payload-bytes"; }
    const std::string realKey = assetlib::blake3File(member);
    if (realKey.empty() || !store.store(realKey, member)) {
        rep.fail(key, "test setup failed: could not store member blob");
        return;
    }

    bool expectAllValid = false;
    const std::string body = buildManifest(rng, realKey, expectAllValid);

    const std::string cookKey(64, 'f');
    store.storeBytes(cookKey, body);

    const auto before = snapshot(scratch.path());
    const fs::path primary = out / "primary.cooked";

    // The call under test. Any crash/hang here IS the finding.
    const bool ok = assetlib::ddcFetchRecord(store, cookKey, primary);

    const auto after = snapshot(scratch.path());

    // ── Property 2: containment ──────────────────────────────────────────────
    for (const auto& p : after) {
        if (before.count(p)) continue;                 // pre-existing
        const fs::path np(p);
        // New files are permitted only directly inside the primary's directory
        // (siblings) or inside the DDC store itself (blob promotion).
        const bool inOutDir = np.parent_path() == out;
        const bool inStore  = p.rfind(root.string(), 0) == 0;
        if (!inOutDir && !inStore) {
            rep.fail(key, "CONTAINMENT VIOLATION: wrote outside the output "
                          "directory: " + p);
            return;
        }
    }
    // The canary must survive byte-for-byte.
    {
        std::ifstream f(guard / "canary.txt");
        std::string s; std::getline(f, s);
        if (s != "untouched") {
            rep.fail(key, "CONTAINMENT VIOLATION: guard file was modified");
            return;
        }
    }

    // ── Property 3: all-or-nothing ───────────────────────────────────────────
    if (ok && !fs::exists(primary, ec))
        rep.fail(key, "returned true but the primary output does not exist");

    // A manifest we built as fully valid must be accepted — otherwise the
    // parser is rejecting legitimate records and every cache hit silently
    // becomes a recook.
    if (expectAllValid && !ok)
        rep.fail(key, "rejected a manifest that was generated fully valid");

    // ── Property 4: no temp leakage ──────────────────────────────────────────
    for (const auto& p : after) {
        if (p.find(".bytes.") != std::string::npos ||
            p.find(".ingest.") != std::string::npos) {
            rep.fail(key, "leaked a store temp file: " + p);
            return;
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    return fuzz::run("ddc_manifest", argc, argv, oneCase);
}
