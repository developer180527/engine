---
status: plan
id: WO-044
title: The ABI compatibility test defends every API group (audit ABI-03)
program: providers
priority: P1
size: S
state: todo
touches:
  - tests/api_abi_compat_test.cpp
source: engine audit 2026-09-30; audit ABI-03's two accepted findings
---
## Why
`api_abi_compat_test`'s `frozen[]` list is a hand-kept copy of the API table's group offsets. Two frozen groups, `intent` and `physics2`, are missing from it, so nothing at run time defends where they sit. A reordered group keeps every size intact and still breaks every kit built against the old table; the frozen ABI is append-only only if a test says so.

## Done when
- [ ] `intent` and `physics2` are in the compatibility test, their offsets checked like the others
- [ ] audit ABI-03 holds with no accepted debt (removed from `scripts/audit_baseline.json`)

## Contract
No change to the ABI: the test defends what is already frozen.

Nothing: a kit sees the same table.
