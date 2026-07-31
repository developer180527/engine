// ── rust_ffi_test — the C++ ↔ Rust boundary itself ──────────────────────────
//
// This does not test networking (there is none yet). It tests the SEAM: that
// a Rust staticlib links into a C++ binary, that both call directions work,
// that buffers cross without either side allocating for the other, and that
// the ABI check actually rejects a mismatch.
//
// It exists because an FFI boundary is the one place in this codebase where a
// mistake is undefined behaviour rather than a compile error. Everything the
// real transport will rely on — borrowed buffers in, callbacks out, status
// codes, versioned init — is exercised here while it is still trivial to
// debug. Under ASan/UBSan this is also where a bad pointer contract surfaces.
#include <cstdio>
#include <cstring>
#include <string>

#include <engine_net.h>

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

// Rust calls into this. Captures what it receives so the test can assert on
// the data that crossed, not merely that a call happened.
struct LogCapture {
    int         calls = 0;
    std::string last;
};
static void logSink(void* user, const char* msg) {
    auto* cap = static_cast<LogCapture*>(user);
    ++cap->calls;
    cap->last = msg ? msg : "<null>";
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("rust_ffi_test: C++ <-> Rust boundary\n");

    // ── Version agreement between header and linked library ─────────────────
    CHECK(engine_net_abi_version() == ENGINE_NET_ABI_VERSION,
          "library ABI %u matches header %u",
          engine_net_abi_version(), ENGINE_NET_ABI_VERSION);

    // A stale static library is the realistic failure mode (C++ rebuilt, Rust
    // not), so the mismatch path must actually reject rather than warn.
    CHECK(engine_net_init(ENGINE_NET_ABI_VERSION + 1) == ENGINE_NET_ERR_ABI,
          "init rejects a mismatched ABI version");

    CHECK(engine_net_init(ENGINE_NET_ABI_VERSION) == ENGINE_NET_OK,
          "init succeeds with the correct ABI version");
    CHECK(engine_net_init(ENGINE_NET_ABI_VERSION) == ENGINE_NET_ERR_ALREADY,
          "double init is refused, not silently ignored");

    // ── Status strings: usable from a failing path (static, never null) ─────
    CHECK(engine_net_status_str(ENGINE_NET_OK) != nullptr
              && std::strcmp(engine_net_status_str(ENGINE_NET_OK), "ok") == 0,
          "status_str(OK) = \"%s\"", engine_net_status_str(ENGINE_NET_OK));
    CHECK(engine_net_status_str(-9999) != nullptr,
          "status_str of an unknown code still returns a string, never null");

    // ── C -> Rust: borrowed buffer in, value out, nothing allocated ─────────
    {
        const char* data = "abc";
        uint64_t sum = 0;
        const int32_t rc = engine_net_checksum(
            reinterpret_cast<const uint8_t*>(data), 3, &sum);
        // Same fixed FNV-1a vector the Rust unit test pins, so a divergence
        // between the two build paths (cargo test vs the linked staticlib)
        // shows up here rather than as a protocol mismatch later.
        CHECK(rc == ENGINE_NET_OK && sum == 0xe71fa2190541574bULL,
              "checksum(\"abc\") = 0x%llx via FFI (matches the Rust unit test)",
              (unsigned long long)sum);
    }
    {
        uint64_t sum = 0;
        CHECK(engine_net_checksum(nullptr, 0, &sum) == ENGINE_NET_OK
                  && sum == 0xcbf29ce484222325ULL,
              "empty buffer is legal and yields the FNV offset basis");
        CHECK(engine_net_checksum(nullptr, 8, &sum) == ENGINE_NET_ERR_NULL_ARG,
              "null pointer with non-zero length is REJECTED (would be UB)");
        CHECK(engine_net_checksum(reinterpret_cast<const uint8_t*>("x"), 1, nullptr)
                  == ENGINE_NET_ERR_NULL_ARG,
              "null out-pointer is rejected");
    }
    {
        // A buffer with embedded NULs and high bytes — packet payloads are not
        // C strings, and treating them as such is a classic truncation bug.
        const unsigned char raw[] = {0x00, 0xFF, 0x41, 0x00, 0x7F};
        uint64_t a = 0, b = 0;
        engine_net_checksum(raw, sizeof raw, &a);
        engine_net_checksum(raw, 3, &b);
        CHECK(a != b,
              "binary payload hashed by LENGTH, not stopped at the first NUL");
    }

    // ── Rust -> C: callback with a borrowed string ──────────────────────────
    {
        LogCapture cap;
        engine_net_set_log(&logSink, &cap);
        CHECK(engine_net_log_test("hello") == ENGINE_NET_OK,
              "log round-trip C -> Rust -> C returns OK");
        CHECK(cap.calls == 1, "sink invoked exactly once (%d)", cap.calls);
        CHECK(cap.last == "[engine_net] hello",
              "sink received Rust-formatted text: \"%s\"", cap.last.c_str());

        // Detaching must be honoured — a stale callback firing after teardown
        // is the shape of a use-after-free once real sessions exist.
        engine_net_set_log(nullptr, nullptr);
        CHECK(engine_net_log_test("ignored") == ENGINE_NET_OK && cap.calls == 1,
              "detached sink is not called again (%d)", cap.calls);

        engine_net_set_log(&logSink, &cap);
        CHECK(engine_net_log_test(nullptr) == ENGINE_NET_ERR_NULL_ARG
                  && cap.calls == 1,
              "null message is rejected before reaching the sink");
    }

    // ── Lifecycle: calls after shutdown are refused, not undefined ──────────
    engine_net_shutdown();
    CHECK(engine_net_log_test("after shutdown") == ENGINE_NET_ERR_NOT_INIT,
          "post-shutdown call returns NOT_INIT instead of misbehaving");
    engine_net_shutdown();   // idempotent — must not crash
    CHECK(true, "double shutdown is safe");

    if (g_failures) {
        std::printf("rust_ffi_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("rust_ffi_test: PASS — Rust links and both call directions work\n");
    return 0;
}
