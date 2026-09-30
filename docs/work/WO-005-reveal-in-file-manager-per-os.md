---
status: plan
id: WO-005
title: "Reveal in Finder" works per OS instead of running `open` everywhere
program: portability
priority: P1
size: S
state: done
done: 2026-09-30
evidence: tests/asset_browser_model_test.cpp (every OS's command checked on any host); hostile names round-tripped through /bin/sh; 3 mutations red
touches:
  - src/editor/panels/asset_browser/actions.h
source: review 2026-09-29 P5 (verified 2026-09-29)
---
## Why
`revealInFinder` runs `std::system("open -R '…'")` unguarded on every OS. On Linux, `open` is a different program altogether.

## Done when
- [x] macOS `open -R`, Windows `explorer /select,`, Linux `xdg-open` on the parent directory; any other OS returns false and logs once
- [x] the function is renamed `revealInFileManager` and returns bool; the menu item is disabled when it returns false
- [x] moves behind `os::` when WO-021 lands (recorded there)

## Not in scope
The rest of the `os::` layer (WO-021).

## Log
- 2026-09-30: `revealCommand(path, isDir, os)` is pure, with the OS as a
  parameter, so the macOS, Windows and Linux commands are all tested on
  whichever machine runs the test. Only `revealInFileManager` touches the host.
- Beyond the order:
  - The menu item is named per OS: "Reveal in Finder", "Show in Explorer",
    "Open Containing Folder".
  - Linux runs `xdg-open` in the background. It can block for as long as the
    file manager is open, and this runs on the UI thread.
  - A Windows path containing `"` is refused rather than escaped.
- **The return value means "issued", not "opened".** `explorer.exe` exits 1
  even on success, and a backgrounded `xdg-open` always returns 0, so the
  exit code carries no information. The menu item is disabled from
  `canReveal()`, which is known before the click, rather than from a result
  that arrives after it.
- The shell quoting was checked for real, not just by string comparison: a
  name with `'`, `$()`, backticks and `;` passes through `/bin/sh` unchanged,
  with nothing executed.
