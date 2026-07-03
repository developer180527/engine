// ── hid — null backend ──────────────────────────────────────────────────────
// Compiled on platforms whose real backend doesn't exist yet (Windows Raw
// Input and Linux evdev land with their ports). init() fails with a clear
// message; every other call is a safe no-op, so consumers link and run.
#include "hid/hid.h"

#include <chrono>

namespace hid {

uint64_t nowNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct Context::Impl {};   // never instantiated

Context::Context()  = default;
Context::~Context() = default;

bool Context::init(const Config&) { return false; }
void Context::shutdown() {}
bool Context::running() const { return false; }
size_t Context::devices(DeviceInfo*, size_t) const { return 0; }
size_t Context::drain(Event*, size_t) { return 0; }
uint64_t Context::dropped() const { return 0; }
const char* Context::lastError() const {
    return "hid: no backend for this platform yet (Win32 Raw Input / evdev "
           "arrive with their ports)";
}

} // namespace hid
