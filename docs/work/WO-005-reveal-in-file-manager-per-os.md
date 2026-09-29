---
status: plan
id: WO-005
title: "Reveal in Finder" works per OS instead of running `open` everywhere
program: portability
priority: P1
size: S
state: todo
touches:
  - src/editor/panels/asset_browser/actions.h
source: review 2026-09-29 P5 (verified 2026-09-29)
---
## Why
`revealInFinder` runs `std::system("open -R '…'")` unguarded on every OS. On Linux, `open` is a different program altogether.

## Done when
- [ ] macOS `open -R`, Windows `explorer /select,`, Linux `xdg-open` on the parent directory; any other OS returns false and logs once
- [ ] the function is renamed `revealInFileManager` and returns bool; the menu item is disabled when it returns false
- [ ] moves behind `os::` when WO-021 lands (recorded there)

## Not in scope
The rest of the `os::` layer (WO-021).
