---
status: reference
---
# Contracts

One file per **subsystem boundary** — the interface, C ABI, function set or data
layout that other code is written against. The point is independent
development: code written against a contract compiles and runs whether or not
the real implementation exists yet, and gets a defined "nothing" when it does
not. Why, and the survey this started from: `docs/plans/subsystem-contracts.md`.

`scripts/engine_doctor.py` reads these files, checks them against the tree, and
renders the **Contracts** chart in `ENGINE_STATUS.md`. Every column except
`state` is derived; `state` is checked against evidence, like a tier.

## Format

```yaml
---
status: as-built            # as-built once the header exists (staleness applies); target while planned
contract: renderer          # unique name
kind: interface             # interface | c-abi | functions | data
state: provisional          # planned | provisional | frozen
owner: src/render           # the area that owns the contract
header: src/render/renderer_interface.h
implementations:
  - real: src/render/renderer.h            # kind: path, or path#Symbol for a class inside a file
  - null: src/render/renderer_null.h       # `null: none` is allowed, explained under ## Nothing
tests:
  - tests/null_renderer_test.cpp
covers:                     # as-built: what staleness is measured against
  - src/render/renderer_interface.h
verified: 2026-09-27
---
```

Implementation kinds:

| kind | means |
|---|---|
| `real` | the implementation that does the job |
| `null` | does nothing, correctly, forever — "nothing" is a legitimate answer (a server has no GPU) |
| `stub` | not done yet, and SAYS so — logs once / returns a status, never silently succeeds |
| `fake` | plausible behaviour for tests, so callers can be tested before the real one exists |

## The meaning — five sections, in the body

A header carries the shape. These carry the rest, and they are what lets
someone build against the contract without asking the owner:

- `## Nothing` — what "no result" means: not ready, not supported here, or failed; and what the null does.
- `## Ownership` — who allocates and frees; how long returned pointers/handles stay valid.
- `## Threading` — which threads may call; what may block.
- `## Timing` — synchronous, or a handle/job to poll or wait on; per-frame or once.
- `## Errors` — return values, logging, poisoned states; what the caller must check.

A section whose first line is `Not yet written.` counts as missing.

## States

- **planned** — declared intent. The header may not exist; the contract appears on the chart so the dependency is visible before the code is.
- **provisional** — the header and ≥1 implementation exist. Callers may use it; it may still change.
- **frozen** — provisional, plus a null (or an explained `none`), ≥1 registered test, and all five sections written. Changes only by adding, or by versioning.
