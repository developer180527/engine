---
status: plan
id: WO-001
title: Work-order board
program: process
priority: P1
size: S
state: active
touches:
  - scripts/work_orders.py
  - docs/work/README.md
source: conversation 2026-09-30 — "put it in a structured place, with priority work orders"
---
## Why
Every review and plan in September produced work, and it lived only in chat, so none of it survived a break.

The board makes the queue a file in the repo, checked like everything else, so coming back after a week starts with one command instead of re-reading transcripts.

## Done when
- [x] `scripts/work_orders.py` with `check`, `board`, `board --check` and `next`
- [x] `docs/work/README.md`: the format, the rules and the contract rule, in plain words
- [x] every open item from the September reviews and plans recorded as an order
- [x] `tests/work_orders_test.py` pins each rule on a scratch tree
- [ ] registered in the ctest `docs` lane (`work_orders_test` plus `board --check`). Waits for the editor/contracts work in progress to be committed, because `tests/CMakeLists.txt` is mid-edit there.

## Steps
Registration, next to `contract_registry_test` in `tests/CMakeLists.txt`:

```cmake
add_test(NAME work_orders_test
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/work_orders_test.py)
set_tests_properties(work_orders_test PROPERTIES LABELS docs TIMEOUT 60)
add_test(NAME work_board_current
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/work_orders.py board --check)
set_tests_properties(work_board_current PROPERTIES LABELS docs TIMEOUT 60)
```

## Not in scope
Replacing `ENGINE_STATUS.md`. That file answers *what is true*; this board answers *what to do next*.
