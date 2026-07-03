// ── hid — macOS backend: IOHIDManager (true raw input) ─────────────────────
// Reads HID value reports below the WindowServer pointer pipeline: no
// acceleration, no pixel quantization, full device report rate, hardware
// timestamps (IOHIDValueGetTimeStamp, mach_absolute_time units → ns).
//
// Threading: init() spawns one thread that schedules the IOHIDManager on its
// own CFRunLoop and parks in CFRunLoopRun() — zero CPU while idle, woken by
// the kernel per report. Every callback (device add/remove, value) fires on
// that thread, so the ring has exactly one producer, as required.
//
// PERMISSION: modern macOS gates HID event data behind "Input Monitoring"
// (TCC). Device ENUMERATION works without it; IOHIDManagerOpen returns
// kIOReturnNotPermitted (and macOS shows a one-time prompt) until the host
// process is approved in System Settings → Privacy & Security → Input
// Monitoring. init() surfaces that as lastError().

#include "hid/hid.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <mach/mach_time.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace hid {

// ── Clock: mach ticks → nanoseconds (one timebase read, 128-bit multiply) ───
namespace {
mach_timebase_info_data_t timebase() {
    static mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t;
        mach_timebase_info(&t);
        return t;
    }();
    return tb;
}
uint64_t ticksToNs(uint64_t ticks) {
    const auto tb = timebase();
    return (uint64_t)(((__uint128_t)ticks * tb.numer) / tb.denom);
}
} // namespace

uint64_t nowNs() { return ticksToNs(mach_absolute_time()); }

// ── Impl ────────────────────────────────────────────────────────────────────
struct Context::Impl {
    static constexpr size_t kRingSize = 4096;   // ~0.5s of an 8kHz mouse

    Config           cfg;
    IOHIDManagerRef  manager = nullptr;
    CFRunLoopRef     loop    = nullptr;
    std::thread      thread;
    bool             running = false;
    char             error[256] = {};

    // Backend-thread readiness handshake (init returns after open verdict).
    std::mutex              readyMu;
    std::condition_variable readyCv;
    bool                    ready   = false;
    bool                    openOk  = false;

    EventRing<kRingSize> ring;

    // Device table — mutated only on the backend thread; read via devices()
    // under the mutex (cold path).
    struct Dev { IOHIDDeviceRef ref; DeviceInfo info; };
    mutable std::mutex devMu;
    std::vector<Dev>   devs;
    DeviceId           nextId = 1;

    void setError(const char* msg) {
        std::snprintf(error, sizeof(error), "%s", msg);
    }

    // ── Device table (backend thread) ───────────────────────────────────────
    static DeviceClass classify(IOHIDDeviceRef dev) {
        auto usageOf = [&](CFStringRef key) -> int {
            int v = 0;
            if (CFTypeRef n = IOHIDDeviceGetProperty(dev, key))
                if (CFGetTypeID(n) == CFNumberGetTypeID())
                    CFNumberGetValue((CFNumberRef)n, kCFNumberIntType, &v);
            return v;
        };
        const int page  = usageOf(CFSTR(kIOHIDPrimaryUsagePageKey));
        const int usage = usageOf(CFSTR(kIOHIDPrimaryUsageKey));
        if (page != kHIDPage_GenericDesktop) return DeviceClass::Unknown;
        switch (usage) {
            case kHIDUsage_GD_Mouse:    return DeviceClass::Mouse;
            case kHIDUsage_GD_Pointer:  return DeviceClass::Mouse;
            case kHIDUsage_GD_Keyboard: return DeviceClass::Keyboard;
            case kHIDUsage_GD_Keypad:   return DeviceClass::Keyboard;
            case kHIDUsage_GD_GamePad:  return DeviceClass::Gamepad;
            case kHIDUsage_GD_Joystick: return DeviceClass::Gamepad;
            default:                    return DeviceClass::Unknown;
        }
    }

    void onDeviceAdded(IOHIDDeviceRef dev) {
        DeviceInfo info{};
        info.cls = classify(dev);

        auto num = [&](CFStringRef key) -> uint16_t {
            int v = 0;
            if (CFTypeRef n = IOHIDDeviceGetProperty(dev, key))
                if (CFGetTypeID(n) == CFNumberGetTypeID())
                    CFNumberGetValue((CFNumberRef)n, kCFNumberIntType, &v);
            return (uint16_t)v;
        };
        info.vendorId  = num(CFSTR(kIOHIDVendorIDKey));
        info.productId = num(CFSTR(kIOHIDProductIDKey));
        if (CFTypeRef s = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDProductKey)))
            if (CFGetTypeID(s) == CFStringGetTypeID())
                CFStringGetCString((CFStringRef)s, info.name,
                                   sizeof(info.name), kCFStringEncodingUTF8);
        if (!info.name[0]) std::snprintf(info.name, sizeof(info.name), "(unnamed)");

        // Physical grouping key: LocationID is shared by every collection/
        // interface of one physical device on one transport. Bluetooth
        // devices may lack it — fall back to an FNV hash of identity
        // (vendor:product:serial:name), which still groups the split
        // collections of one physical unit.
        {
            int loc = 0;
            if (CFTypeRef n = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDLocationIDKey)))
                if (CFGetTypeID(n) == CFNumberGetTypeID())
                    CFNumberGetValue((CFNumberRef)n, kCFNumberIntType, &loc);
            if (loc) {
                info.physId = (uint32_t)loc;
            } else {
                char serial[64] = {};
                if (CFTypeRef s = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDSerialNumberKey)))
                    if (CFGetTypeID(s) == CFStringGetTypeID())
                        CFStringGetCString((CFStringRef)s, serial,
                                           sizeof(serial), kCFStringEncodingUTF8);
                uint32_t h = 2166136261u;
                auto fnv = [&h](const void* p, size_t n) {
                    for (size_t i = 0; i < n; ++i)
                        h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
                };
                fnv(&info.vendorId, 2); fnv(&info.productId, 2);
                fnv(serial, std::strlen(serial));
                fnv(info.name, std::strlen(info.name));
                info.physId = h ? h : 1;
            }
        }

        {
            std::lock_guard<std::mutex> lk(devMu);
            for (const Dev& d : devs)               // re-add of a known ref
                if (d.ref == dev) return;
            info.id = nextId++;
            devs.push_back({dev, info});
        }
        ring.push({nowNs(), info.id, EventType::DeviceAdded, 0, 0,
                   (int32_t)info.cls, 0});
    }

    void onDeviceRemoved(IOHIDDeviceRef dev) {
        DeviceInfo info{};
        {
            std::lock_guard<std::mutex> lk(devMu);
            for (size_t i = 0; i < devs.size(); ++i) {
                if (devs[i].ref == dev) {
                    info = devs[i].info;
                    devs.erase(devs.begin() + (long)i);
                    break;
                }
            }
        }
        if (info.id)
            ring.push({nowNs(), info.id, EventType::DeviceRemoved, 0, 0,
                       (int32_t)info.cls, 0});
    }

    // Cold-ish lookup (few devices, backend thread only writes).
    bool lookup(IOHIDDeviceRef dev, DeviceId* id, DeviceClass* cls) {
        std::lock_guard<std::mutex> lk(devMu);
        for (const Dev& d : devs) {
            if (d.ref == dev) { *id = d.info.id; *cls = d.info.cls; return true; }
        }
        return false;
    }

    // ── The hot path: one HID element value → one Event ─────────────────────
    void onValue(IOHIDValueRef value) {
        IOHIDElementRef elem = IOHIDValueGetElement(value);
        const uint32_t page  = IOHIDElementGetUsagePage(elem);
        const uint32_t usage = IOHIDElementGetUsage(elem);

        DeviceId    id;
        DeviceClass cls;
        if (!lookup(IOHIDElementGetDevice(elem), &id, &cls)) return;

        // Hardware/driver timestamp; 0 on some synthetic paths → now.
        uint64_t t = IOHIDValueGetTimeStamp(value);
        t = t ? ticksToNs(t) : nowNs();

        const int32_t v = (int32_t)IOHIDValueGetIntegerValue(value);

        switch (page) {
        case kHIDPage_GenericDesktop:
            if (cls == DeviceClass::Mouse) {
                // Relative counts, one axis per report element.
                if (usage == kHIDUsage_GD_X && v)
                    ring.push({t, id, EventType::MouseMotion, 0, 0, v, 0});
                else if (usage == kHIDUsage_GD_Y && v)
                    ring.push({t, id, EventType::MouseMotion, 0, 0, 0, v});
                else if (usage == kHIDUsage_GD_Wheel && v)
                    ring.push({t, id, EventType::Scroll, 0, 0, 0, v});
            } else if (cls == DeviceClass::Gamepad) {
                if (usage >= kHIDUsage_GD_X && usage <= kHIDUsage_GD_Hatswitch)
                    ring.push({t, id, EventType::Axis, 0, (uint16_t)usage, v, 0});
            }
            break;
        case kHIDPage_Button:
            // usage is 1-based button number → 0-based index.
            if (usage >= 1)
                ring.push({t, id, EventType::Button, 0,
                           (uint16_t)(usage - 1), v ? 1 : 0, 0});
            break;
        case kHIDPage_KeyboardOrKeypad:
            // 0x00–0x03 are error/rollover codes, not keys.
            if (usage >= kHIDUsage_KeyboardA && usage <= 0xE7)
                ring.push({t, id, EventType::Key, 0,
                           (uint16_t)usage, v ? 1 : 0, 0});
            break;
        case kHIDPage_Consumer:
            if (usage == kHIDUsage_Csmr_ACPan && v)   // horizontal scroll
                ring.push({t, id, EventType::Scroll, 0, 0, v, 0});
            break;
        default:
            break;
        }
    }

    // ── Backend thread body ─────────────────────────────────────────────────
    static CFMutableDictionaryRef matchDict(int page, int usage) {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 2,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFNumberRef p = CFNumberCreate(nullptr, kCFNumberIntType, &page);
        CFNumberRef u = CFNumberCreate(nullptr, kCFNumberIntType, &usage);
        CFDictionarySetValue(d, CFSTR(kIOHIDDeviceUsagePageKey), p);
        CFDictionarySetValue(d, CFSTR(kIOHIDDeviceUsageKey), u);
        CFRelease(p); CFRelease(u);
        return d;
    }

    void threadMain() {
        loop    = CFRunLoopGetCurrent();
        manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);

        // Matching set per Config.
        CFMutableArrayRef match = CFArrayCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        auto add = [&](int usage) {
            CFMutableDictionaryRef d = matchDict(kHIDPage_GenericDesktop, usage);
            CFArrayAppendValue(match, d);
            CFRelease(d);
        };
        if (cfg.mouse)    { add(kHIDUsage_GD_Mouse); add(kHIDUsage_GD_Pointer); }
        if (cfg.keyboard) { add(kHIDUsage_GD_Keyboard); add(kHIDUsage_GD_Keypad); }
        if (cfg.gamepad)  { add(kHIDUsage_GD_GamePad); add(kHIDUsage_GD_Joystick); }
        IOHIDManagerSetDeviceMatchingMultiple(manager, match);
        CFRelease(match);

        IOHIDManagerRegisterDeviceMatchingCallback(manager,
            [](void* ctx, IOReturn, void*, IOHIDDeviceRef dev) {
                ((Impl*)ctx)->onDeviceAdded(dev);
            }, this);
        IOHIDManagerRegisterDeviceRemovalCallback(manager,
            [](void* ctx, IOReturn, void*, IOHIDDeviceRef dev) {
                ((Impl*)ctx)->onDeviceRemoved(dev);
            }, this);
        IOHIDManagerRegisterInputValueCallback(manager,
            [](void* ctx, IOReturn, void*, IOHIDValueRef v) {
                ((Impl*)ctx)->onValue(v);
            }, this);

        IOHIDManagerScheduleWithRunLoop(manager, loop, kCFRunLoopDefaultMode);
        const IOReturn r = IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);

        {
            std::lock_guard<std::mutex> lk(readyMu);
            openOk = (r == kIOReturnSuccess);
            if (!openOk) {
                if (r == kIOReturnNotPermitted)
                    setError("IOHIDManagerOpen: not permitted — grant Input "
                             "Monitoring to this app (System Settings → "
                             "Privacy & Security → Input Monitoring), then "
                             "re-run");
                else
                    std::snprintf(error, sizeof(error),
                                  "IOHIDManagerOpen failed: 0x%08x", r);
            }
            ready = true;
        }
        readyCv.notify_all();
        if (!openOk) {
            IOHIDManagerUnscheduleFromRunLoop(manager, loop, kCFRunLoopDefaultMode);
            CFRelease(manager); manager = nullptr;
            return;
        }

        CFRunLoopRun();   // parks here; kernel wakes us per HID report

        // shutdown() stopped the loop — tear down on the owning thread.
        IOHIDManagerClose(manager, kIOHIDOptionsTypeNone);
        IOHIDManagerUnscheduleFromRunLoop(manager, loop, kCFRunLoopDefaultMode);
        CFRelease(manager); manager = nullptr;
    }
};

// ── Context ─────────────────────────────────────────────────────────────────
Context::Context()  = default;
Context::~Context() { shutdown(); }

bool Context::init(const Config& cfg) {
    if (m_impl) return m_impl->running;
    m_impl = new Impl();
    m_impl->cfg = cfg;
    m_impl->thread = std::thread([impl = m_impl] { impl->threadMain(); });

    std::unique_lock<std::mutex> lk(m_impl->readyMu);
    m_impl->readyCv.wait(lk, [this] { return m_impl->ready; });
    if (!m_impl->openOk) {
        lk.unlock();
        m_impl->thread.join();
        return false;   // impl kept: lastError() readable; shutdown() cleans
    }
    m_impl->running = true;
    return true;
}

void Context::shutdown() {
    if (!m_impl) return;
    if (m_impl->running && m_impl->loop) {
        CFRunLoopStop(m_impl->loop);
        m_impl->thread.join();
    } else if (m_impl->thread.joinable()) {
        m_impl->thread.join();
    }
    delete m_impl;
    m_impl = nullptr;
}

bool Context::running() const { return m_impl && m_impl->running; }

size_t Context::devices(DeviceInfo* out, size_t max) const {
    if (!m_impl) return 0;
    std::lock_guard<std::mutex> lk(m_impl->devMu);
    size_t n = m_impl->devs.size();
    if (n > max) n = max;
    for (size_t i = 0; i < n; ++i) out[i] = m_impl->devs[i].info;
    return n;
}

size_t Context::drain(Event* out, size_t max) {
    return m_impl ? m_impl->ring.drain(out, max) : 0;
}

uint64_t Context::dropped() const {
    return m_impl ? m_impl->ring.dropped() : 0;
}

const char* Context::lastError() const {
    return m_impl ? m_impl->error : "not initialized";
}

} // namespace hid
