#pragma once
// ── The Add-on protocol, v1 — driving a tool the editor did not link ─────────
//
// `docs/architecture/extension-model.md` defines four extension tiers and
// implements three. The fourth is the Add-on: a separate PROCESS, spoken to over
// IPC, protocol-versioned, whose crash takes down only itself. Its argument is
// one line of that document:
//
//     "The process boundary is affordable exactly where the frame boundary is
//      not."
//
// An importer costing 40 ms of round trip is fine. A physics engine costing that
// is not. So Add-ons are for TOOLS, and this header is the contract they speak.
//
// ── What this protocol is NOT ───────────────────────────────────────────────
// It covers exactly one direction: the host launches a tool, the tool does one
// job, the tool exits. Batch. Request/response with a process lifetime.
//
// It does NOT cover live link — a running tool streaming edits into a running
// editor. That needs a persistent connection, a discovery mechanism, and
// incremental state, and none of those three has anything in common with what is
// below. `tool-ecosystem.md` §10 records this as an open question and warns that
// pretending the two are one protocol "would produce a protocol that serves
// neither". Scoping is how that is answered honestly: v1 is the batch direction,
// and a live-link protocol will be a different document with a different header.
//
// ── The three rules ─────────────────────────────────────────────────────────
//
// 1. STDOUT AND STDERR ARE HUMAN CHANNELS. Always. A tool prints whatever helps
//    a person debugging it, in whatever wording reads best, and changing that
//    wording is never a breaking change.
//
// 2. THE RESULT FILE IS THE MACHINE CHANNEL. A framed, digest-trailered sidecar
//    file named by the host on the command line. This is the only thing a host
//    or a test is allowed to parse.
//
//    Rule 2 exists because of a specific, already-realised failure. Add-ons run
//    OTHER PEOPLE'S CODE — that is the whole reason they are out of process — and
//    other people's code prints. `engine_cook_worker` learned this first and its
//    header says so plainly: "cookers print freely, so stdout is not a channel."
//    `engine_module_probe` learned it the harder way: it dlopens untrusted
//    modules, and a module's static initialiser or `create` can write a line to
//    stdout that is INDISTINGUISHABLE from the probe's own verdicts. Not noise —
//    forgery. A channel shared with the thing under test is not a channel.
//
// 3. EXIT STATUS SAYS WHETHER THE TOOL RAN. Never what it decided.
//
//    This is the probe's insight, generalised. A refused module is a SUCCESSFUL
//    probe of a bad module; a texture that fails validation is a successful
//    validation. Collapsing "the answer is no" into a non-zero exit makes it
//    indistinguishable from a crash, and the host then cannot tell "your asset is
//    broken" from "the tool is broken" — two findings with completely different
//    remedies. The verdict lives in the result file, where it can be specific.
//
// ── Why the frame has a trailer ─────────────────────────────────────────────
// `VERDICT ok` is the FIRST body line, so a tool killed part-way through writing
// — a deadline SIGKILL, an rlimit OOM, a signal out of a corrupt parse — left a
// file that parsed as a clean success with every subsequent record simply absent.
// That shipped once already, through the cook worker: a mesh committed with its
// sibling textures never registered, the silently-untextured build arriving
// through the IPC channel instead of the packager. A result is valid only if the
// trailer agrees with the body, so a partial write is DETECTABLY partial.
//
// That also means the write needs no atomic temp-and-rename. Torn writes are
// detected rather than prevented, which is the weaker guarantee and the right
// one: a tool that is SIGKILLed cannot rename anything, so prevention was never
// available in the first place.
//
// FNV-1a, not BLAKE3. This detects truncation and interrupted writes in our own
// temp directory. It is not a security boundary — anyone who can rewrite the
// result file can rewrite the artifact beside it — and the DDC's content hash is
// what guards artifacts. A 64-bit non-cryptographic digest is the right tool for
// "did this write finish".
//
// ── Deliberate duplication ──────────────────────────────────────────────────
// `assetlib/cook_result_file.h` implements the same framing under a different
// magic, and is NOT expressed in terms of this file. assetlib is a standalone
// CMake project that does not depend on the engine SDK, and inverting that to
// remove thirty lines of duplication would be the worse trade. The two are kept
// honest by a test that frames the same body through both and requires identical
// bytes, so a change to one that the other does not follow fails a build rather
// than diverging quietly on disk.
//
// ── Header-only, and dependency-free on purpose ─────────────────────────────
// An Add-on may be written by anyone, in anything, and must not have to link the
// engine to talk to it. Everything here is standard library only, so a
// third-party tool can vendor this one file — or reimplement it from the format
// documented in the comments, which is what the Rust conformance suite does, and
// which is the real test of whether the format is specified or merely
// implemented.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace engine::addon {

// Bumped when the FRAME or the reserved records change — never for a tool's own
// vocabulary. A host that reads a version it does not know must refuse the tool
// rather than guess, because the failure mode of guessing is reading a partial
// body as a complete one.
inline constexpr int kProtocolVersion = 1;

inline constexpr std::string_view kResultMagic   = "ENGINE_ADDON_RESULT";
inline constexpr std::string_view kManifestMagic = "ENGINE_ADDON_MANIFEST";

// ── Exit status ─────────────────────────────────────────────────────────────
// Only "the tool ran" is zero. Everything else means the tool did NOT reach a
// verdict, and the host must not look for a result file.
//
// 1 IS DELIBERATELY UNASSIGNED. It is the exit code of every accidental failure
// in existence: a bare `return 1`, an uncaught exception through a runtime that
// chose 1, a shell wrapper that lost the real status. Giving it meaning would
// make the most common accident indistinguishable from a documented outcome, so
// it is left in the "unknown — treat as a crash" bucket along with 1, 5, 139 and
// everything else this enum does not name.
enum class Exit : int {
    Ran            = 0, // ran to completion; the verdict is `ok` or `skip`
    Usage          = 2, // the command line was wrong; nothing ran
    MissingInput   = 3, // a named input was not there; nothing ran
    Failed         = 4, // started and could not finish — the TOOL's failure,
                        // not a verdict about the input
    RanWithFailure = 5, // ran to completion; the verdict is `fail`
};

// Whether this status means a result file exists and can be trusted. `Ran` and
// `RanWithFailure` are the only two, and that is the whole point of separating
// them from `Failed`.
inline bool reachedVerdict(int status) {
    return status == static_cast<int>(Exit::Ran)
        || status == static_cast<int>(Exit::RanWithFailure);
}

// ── Why `RanWithFailure` exists, when rule 3 says the status is not the verdict
// It is still not the verdict. It says WHICH of two things the caller should read
// — and both of them mean "the result file is complete and trustworthy", which is
// exactly the distinction rule 3 protects. A host reads the file either way;
// `reachedVerdict` is the test it actually cares about, and 0 and 5 both pass it.
//
// It exists because the second tool to speak this protocol has TWO kinds of
// caller. `engine_module_probe` has one: a host that reads the result file, for
// which exit 0 and a `refused` record is perfect. `engine_build` is also run
// directly by people and by CI, where the exit status is the ONLY channel — and a
// packaging step that exits 0 on a defective package is precisely the bug being
// fixed by giving it a protocol at all.
//
// The rejected alternative was to make the exit status depend on whether
// `--addon-result` was passed. That is worse: it makes a tool's contract change
// shape based on how it was invoked, and a caller reading the docs cannot tell
// which contract it is going to get.
//
// A tool whose "no" is a NORMAL outcome should still report it as a record at
// exit 0, the way the probe reports a refusal — a refused module is a successful
// probe. Use `RanWithFailure` only when the tool's own verdict for the whole run
// is `fail`.

inline int exitCode(Exit e) { return static_cast<int>(e); }

// ── The reserved records ────────────────────────────────────────────────────
// The body is line-oriented `KEY value`, deliberately human-readable — you can
// `cat` one while debugging. Two keys are reserved and mean the same thing for
// every Add-on that will ever exist:
//
//     VERDICT ok|fail|skip     exactly one, always first
//     ERROR   <one line>       at most one, present iff the verdict is fail
//
// Everything else is the tool's own vocabulary, declared in its manifest. The
// protocol does not fix a record vocabulary across tools, and will not until
// three real Add-ons exist to generalise from — inventing an asset-type ontology
// before then would produce names that fit nothing.
enum class Verdict { Ok, Fail, Skip };

inline std::string_view verdictName(Verdict v) {
    switch (v) {
        case Verdict::Ok:   return "ok";
        case Verdict::Fail: return "fail";
        case Verdict::Skip: return "skip";
    }
    return "fail";   // unreachable; `fail` is the safe direction if it happens
}

inline bool parseVerdict(std::string_view s, Verdict& out) {
    if (s == "ok")   { out = Verdict::Ok;   return true; }
    if (s == "fail") { out = Verdict::Fail; return true; }
    if (s == "skip") { out = Verdict::Skip; return true; }
    return false;
}

// ── Framing ─────────────────────────────────────────────────────────────────

inline uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 1469598103934665603ull;            // offset basis
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    return h;
}

// Wrap a completed body (every line already '\n'-terminated) into file contents:
//
//     <magic> <version>
//     <body>
//     END <line-count> <hex digest of body>
//
inline std::string frame(std::string_view magic, std::string_view body,
                         std::size_t lines) {
    char trailer[64];
    std::snprintf(trailer, sizeof(trailer), "END %zu %llx\n", lines,
                  (unsigned long long)fnv1a64(body));
    std::string out;
    out.reserve(magic.size() + 8 + body.size() + 40);
    out.append(magic).append(" ").append(std::to_string(kProtocolVersion)).append("\n");
    out.append(body);
    out.append(trailer);
    return out;
}

// Strip and validate the frame. On success `body` holds the payload lines; on
// failure `err` says what was wrong in terms a human debugging a tool can act
// on. Never throws — every malformed shape is a `false` return, because this
// parses output from a process that may have died in the middle of producing it.
inline bool unframe(std::string_view magic, std::string_view file,
                    std::string& body, std::string& err) {
    body.clear();

    const std::size_t h = file.find('\n');
    if (h == std::string_view::npos) { err = "no header line"; return false; }
    const std::string_view header = file.substr(0, h);
    if (header.rfind(magic, 0) != 0) {
        err = std::string("not an add-on ") + std::string(magic) + " file (bad magic)";
        return false;
    }
    int ver = 0;
    if (std::sscanf(std::string(header.substr(magic.size())).c_str(), "%d", &ver) != 1) {
        err = "header has no version"; return false;
    }
    if (ver != kProtocolVersion) {
        err = "add-on protocol version " + std::to_string(ver) + ", expected "
            + std::to_string(kProtocolVersion) + " (stale tool binary?)";
        return false;
    }

    // The trailer is the last '\n'-terminated line. A file not ending in a
    // newline was cut mid-line, which is the common truncation shape.
    if (file.empty() || file.back() != '\n') {
        err = "truncated: does not end at a line boundary"; return false;
    }
    const std::size_t lastEnd = file.size() - 1;              // index of final \n
    const std::size_t lastBeg = file.rfind('\n', lastEnd ? lastEnd - 1 : 0);
    if (lastBeg == std::string_view::npos || lastBeg < h) {
        err = "truncated: no END trailer"; return false;
    }
    const std::string_view trailer =
        file.substr(lastBeg + 1, lastEnd - lastBeg - 1);
    if (trailer.rfind("END ", 0) != 0) {
        err = "truncated: last line is not END (tool died mid-write)";
        return false;
    }

    unsigned long long claimedLines = 0, claimedHash = 0;
    if (std::sscanf(std::string(trailer).c_str(), "END %llu %llx",
                    &claimedLines, &claimedHash) != 2) {
        err = "malformed END trailer"; return false;
    }

    const std::string_view payload = file.substr(h + 1, lastBeg - h);
    std::size_t lines = 0;
    for (char c : payload) if (c == '\n') ++lines;
    if (lines != claimedLines) {
        err = "incomplete: END claims " + std::to_string(claimedLines)
            + " line(s), found " + std::to_string(lines);
        return false;
    }
    if (fnv1a64(payload) != claimedHash) {
        err = "corrupt: body digest does not match END";
        return false;
    }

    body.assign(payload);
    return true;
}

// ── Writing a result ────────────────────────────────────────────────────────
// Accumulate, then emit. Two things are structural rather than documented:
//
//   * ORDER. `VERDICT` is emitted first and `ERROR` second no matter when they
//     were set, so a tool cannot accidentally write its verdict last — which is
//     precisely the ordering that would make the trailer pointless, because a
//     truncated file would then be missing the one line a host needs.
//   * THE LINE COUNT. It is derived from the body at emit time and cannot be
//     passed in, so the count and the body cannot disagree.
class Result {
public:
    void verdict(Verdict v) { m_verdict = v; m_haveVerdict = true; }

    // Single-line, human-readable, and only meaningful with `Verdict::Fail`.
    void error(std::string_view msg) { m_error = sanitise(msg); m_haveError = true; }

    // One `KEY value` record, for values that are MESSAGES — text a person
    // reads. Both halves are sanitised, so a control character is replaced and
    // the record is still emitted. See `sanitise`.
    void record(std::string_view key, std::string_view value) {
        std::string line = sanitiseKey(key);
        line += ' ';
        line += sanitise(value);
        line += '\n';
        m_records.push_back(std::move(line));
    }

    // One `KEY value` record for values the CALLER WILL RESOLVE — a path it
    // opens, an id it looks up. Returns false, and emits nothing, when the value
    // cannot be carried exactly.
    //
    // ── Why this is not the same function as `record` ───────────────────────
    // Sanitising is right for a message and dangerous for an identifier, and the
    // difference is what the caller does next.
    //
    // Mangle a warning and a human reads a slightly odd sentence. Mangle a PATH
    // and the caller opens the wrong file — or, more likely, a file that does not
    // exist, and now the tool has reported an output nobody can find. That turns
    // a loud, precise failure ("this filename cannot be represented") into a
    // quiet wrong answer, which is the trade this whole protocol exists to refuse.
    //
    // This distinction was missed in v1 and found by the SECOND tool to speak the
    // protocol. The probe only ever put paths in records as diagnostic text — the
    // verdict was the payload — so mangling was harmless and the gap invisible.
    // `engine_build` reports paths its caller is expected to open, and there the
    // same function would have been a defect. One speaker cannot tell you whether
    // a protocol generalises.
    //
    // The caller MUST handle `false`. For a cook or a package that means failing
    // the run with an error naming the offending value: an input the format
    // cannot represent is a real failure, and saying so beats guessing.
    [[nodiscard]] bool recordExact(std::string_view key, std::string_view value) {
        if (!usableKey(key) || !carryable(value)) return false;
        std::string line(key);
        line += ' ';
        line.append(value);
        line += '\n';
        m_records.push_back(std::move(line));
        return true;
    }

    // Whether `recordExact` could carry this VALUE. Exposed so a tool can check
    // before it has done the work, and report the problem where the offending
    // value is still in scope.
    //
    // Spaces are allowed, and must be: a value runs to the end of the line, and
    // refusing spaces would refuse every path with one in it — the case
    // `recordExact` exists to carry. The KEY is the half where a space is
    // structural; see `usableKey`.
    static bool carryable(std::string_view s) {
        for (unsigned char u : s)
            if (u < 0x20 || u == 0x7f) return false;
        return true;
    }

    // Whether this KEY can be written unambiguously.
    //
    // A record is `KEY value`, split on the FIRST space, so a key containing one
    // is not carried — it is silently re-cut. `recordExact("MY KEY", "v")` emits
    // `MY KEY v`, and every reader in this tree hands back key `MY` with value
    // `KEY v`. That is precisely the quiet wrong answer `recordExact` was split
    // out of `record` to refuse, hiding in `recordExact` itself: the value was
    // guarded and the key was not.
    //
    // An empty key is refused for the same reason — ` value` parses as a key of
    // nothing followed by a value that has lost its leading space.
    //
    // Unlike a bad value, a bad key is a mistake by the TOOL'S AUTHOR rather
    // than something that arrived from the world, so it fails the same way and
    // is caught the first time the tool is run.
    static bool usableKey(std::string_view s) {
        if (s.empty()) return false;
        for (unsigned char u : s)
            if (u <= 0x20 || u == 0x7f) return false;   // <= : space included
        return true;
    }

    // The framed file contents. Exposed separately from `writeTo` so a test can
    // check the bytes without a filesystem.
    std::string framed() const { return framedAs(kResultMagic); }

    // Same body, a different magic. The manifest is the only other user: it is
    // the same ordered, sanitised, digest-trailered body under its own header,
    // and giving the magic a parameter is what stops that being a second
    // implementation of everything above.
    std::string framedAs(std::string_view magic) const {
        std::string body;
        // A missing verdict is written as `fail`, not omitted. A body with no
        // VERDICT would be rejected by every reader as malformed, which reports
        // "the tool is broken" — true, but it buries the records the tool DID
        // produce. `fail` keeps them readable and still cannot be mistaken for
        // success.
        body.append("VERDICT ")
            .append(verdictName(m_haveVerdict ? m_verdict : Verdict::Fail))
            .append("\n");
        if (m_haveError) body.append("ERROR ").append(m_error).append("\n");
        for (const std::string& r : m_records) body.append(r);

        std::size_t lines = 0;
        for (char c : body) if (c == '\n') ++lines;
        return frame(magic, body, lines);
    }

    // One write, one close. No temp-and-rename: see the header comment — a
    // SIGKILLed tool cannot rename, so detection is the only guarantee that was
    // ever on offer, and the trailer provides it.
    bool writeTo(const std::string& path, std::string& err) const {
        const std::string bytes = framed();
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { err = "cannot open result file: " + path; return false; }
        const std::size_t n =
            std::fwrite(bytes.data(), 1, bytes.size(), f);
        const bool flushed = std::fflush(f) == 0;
        std::fclose(f);
        if (n != bytes.size() || !flushed) {
            err = "short write to result file: " + path;
            return false;
        }
        return true;
    }

private:
    // Newlines and control characters become spaces.
    //
    // Not tidiness — FORGERY PREVENTION, and the same class of bug as rule 2 one
    // level down. The probe puts filesystem PATHS in records, and a POSIX path
    // may legally contain a newline. A module at a path spelling
    // "x\nVERDICT ok" would inject a second reserved record into a body whose
    // digest and line count both agree, because the writer computed them after
    // the injection. Sanitising at the point where untrusted text enters the
    // body is the only place this can be fixed; the frame cannot see it.
    //
    // Correct for MESSAGES, and only messages. For a value the caller resolves,
    // use `recordExact` and handle its `false` — see the note there.
    static std::string sanitise(std::string_view s) {
        std::string out(s);
        for (char& c : out) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 0x20 || u == 0x7f) c = ' ';
        }
        return out;
    }

    // The same, plus the space itself, because in a KEY a space is not text — it
    // is the delimiter, and leaving one in re-cuts the record at the wrong place.
    //
    // `record` has no way to say no: it returns void and its contract is that it
    // always emits, so refusing here would mean silently dropping a warning,
    // which is worse than an ugly key. `_` keeps the record parseable and makes
    // the mistake visible in the file. `recordExact` has a channel and uses it —
    // it refuses via `usableKey` rather than mangling, because there the caller
    // is going to resolve what it reads.
    //
    // An EMPTY key would still produce a leading space, so it becomes `_` too:
    // one visibly wrong key beats a record whose value silently loses a
    // character to the split.
    static std::string sanitiseKey(std::string_view s) {
        if (s.empty()) return "_";
        std::string out = sanitise(s);
        for (char& c : out)
            if (c == ' ') c = '_';
        return out;
    }

    Verdict                  m_verdict{Verdict::Fail};
    bool                     m_haveVerdict{false};
    std::string              m_error;
    bool                     m_haveError{false};
    std::vector<std::string> m_records;
};

// ── The manifest ────────────────────────────────────────────────────────────
// `--addon-manifest` makes a tool self-describing, so a host can decide whether
// it can talk to this binary without a table of hardcoded knowledge per tool.
// Same framing, different magic:
//
//     ENGINE_ADDON_MANIFEST 1
//     VERDICT ok
//     ID engine_module_probe
//     TOOL <tool's own version>
//     RECORD MODULE
//     CONSUMES module-library
//     PRODUCES module-verdict
//     END 6 <hex>
//
// EXACTLY ONE SPACE between key and value, here and in every record. This
// example used to be column-aligned for readability, which was a real defect in
// a header whose stated contract is that a third-party tool can "reimplement it
// from the format documented in the comments": a writer built from the aligned
// version emits padded keys, and every reader splits on the FIRST space and
// hands back a value with leading blanks. A spec cannot use whitespace
// decoratively. The value may then contain spaces freely — it runs to the end of
// the line — which is what lets a path with a space in it survive `recordExact`.
//
// `RECORD` lines declare the tool's own vocabulary. That is what makes a rename
// a LOUD change: a host or test that parses `MODULE` can assert the tool still
// claims to emit it, instead of silently reading zero records off a body that
// renamed the key.
//
// CONSUMES and PRODUCES are free-form tokens on purpose. A real asset-type
// vocabulary is a thing to extract from three working Add-ons, not to invent for
// the first one, and a wrong ontology here would be copied by everything after.
//
// ON STDOUT, not a file — and that is not a violation of rule 2. Rule 2 exists
// because a tool's stdout is shared with code the tool loads. `--addon-manifest`
// loads nothing, opens nothing and runs no third-party anything: it prints a
// constant and exits. The channel is uncontended precisely when the tool has
// done no work, which is the only case this uses it for.
inline void writeManifest(const Result& manifest) {
    const std::string out = manifest.framedAs(kManifestMagic);
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

} // namespace engine::addon
