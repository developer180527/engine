# hid — thin cross-platform raw-input module

Devices + timestamped raw events, nothing else. Standalone like assetlib:
zero engine dependencies, consumable by any project. This is the bottom of
the AAA input pipeline — it reads the platform's HID stream BEFORE the OS
pointer pipeline (no acceleration, no pixel quantization, full 1000–8000 Hz
report rate, hardware timestamps), and hands the consumer a lock-free ring
of 24-byte POD events on one monotonic clock.

## Scope discipline (why this module stays thin)
IN: device enumeration/hotplug, raw motion counts, buttons/keys (HID usage
codes), gamepad raw axes, hardware-timestamp normalization, the SPSC ring.
OUT (consumer policy, deliberately): action maps, contexts, UI capture,
sensitivity/dead zones, key display names, text/IME (stays on the windowing
layer — GLFW keeps the window and typing), cursor handling.

High-level engine systems are INPUT-AGNOSTIC by project rule: gameplay binds
to actions; the developer wires devices→actions at the project/engine
input-manager layer, which consumes this module.

## Architecture
```
kernel/driver ─► backend thread (parks in OS wait; one producer)
                    │  normalize: usage codes + ONE monotonic ns clock
                    ▼
              EventRing<4096> (lock-free SPSC, drop-newest + counter)
                    │  pure userspace reads — game loop makes no syscalls
                    ▼
              consumer drain() → engine InputManager (later)
```

Backends are one TU each behind the same header contract (`include/hid/hid.h`
— the module's ABI):
- `hid_iohid.cpp` — macOS IOHIDManager. LIVE.
- Win32 Raw Input (`WM_INPUT`/GameInput) — with the Windows port.
- Linux evdev — with the Linux pass.
- `hid_null.cpp` — anything else: init() fails cleanly, consumers still link.

## macOS notes
- **Input Monitoring (TCC)**: event data requires the host process to be
  granted Input Monitoring (System Settings → Privacy & Security). Without
  it `IOHIDManagerOpen` returns `kIOReturnNotPermitted` and `init()` fails
  with an actionable `lastError()`. macOS may not show a prompt for CLI
  children — add the terminal/app manually.
- Timestamps: `IOHIDValueGetTimeStamp` (mach ticks) → ns via timebase;
  `hid::nowNs()` reads the same clock.
- Trackpads appear as Mouse-class relative devices. Gamepad events are RAW
  logical values (uncalibrated); a curated pad layer (GCController /
  GameInput) can sit above later without changing this module.

## Verification
- `hid_dump --list` / `hid_dump --seconds N` — enumerate devices, stream
  events, per-second motion report rate (shows the mouse's true polling rate
  reaching userspace), drop counter.
- `hid_ring_test` — no hardware/permission needed: 2M-event concurrent FIFO
  integrity, overflow drop accounting, clock monotonicity, POD size.

## Future Work
- Win32 Raw Input backend (message-only window thread, GetRawInputBuffer
  batching) + GameInput; evdev backend.
- Gamepad curation layer (GCController/XInput mapping to a stable pad model).
- Wheel high-resolution scroll units (Windows WM_INPUT gives 1/120ths).
- Optional MMAP'd event trace dump for input-latency regression testing.
