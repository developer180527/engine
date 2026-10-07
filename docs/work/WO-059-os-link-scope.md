---
status: plan
id: WO-059
title: OS frameworks and X11 are linked by the target that calls them, not handed to every consumer
program: portability
priority: P2
size: S
state: todo
depends: []
touches:
  - src/CMakeLists.txt
source: platform audit 2026-10-07
---
## Why
`engine_runtime` links Cocoa, QuartzCore, Metal, IOKit and the audio frameworks on
macOS, gdi32/user32/dwmapi on Windows, and X11 **with its include directories**
on Linux, all `PUBLIC`. So every game, test and kit inherits them, though only the
platform, render and audio code calls them. The first build without X11 (web,
Android, a minimal container) fails in code that never uses a window.

## Done when
- [ ] each OS library is linked `PRIVATE` by the target whose code calls it (platform backend, renderer, audio provider, hid), with a comment naming the call
- [ ] no X11 include directory is on any public interface
- [ ] a check (the SDK-only job or an include probe) fails if a game target can include `<X11/...>` or `<Cocoa/...>`, mutation-checked
- [ ] the server build (`engine_runtime_server`) links no window, graphics or audio framework, asserted by the existing link probe

## Contract
Nothing: link scope only, no API change.
