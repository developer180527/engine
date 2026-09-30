---
status: plan
id: WO-044
title: The ABI compatibility test defends every API group (audit ABI-03)
program: providers
priority: P1
size: S
state: done
done: 2026-09-30
evidence: api_abi_compat_test (physics2 at 800, intent at 816, and a static_assert that frozen[] and offs[] list the same groups); audit ABI-03 ok with no baseline entries
touches:
  - tests/api_abi_compat_test.cpp
source: engine audit 2026-09-30; audit ABI-03's two accepted findings
---
## Why
`api_abi_compat_test`'s `frozen[]` list is a hand-kept copy of the API table's group offsets. Two frozen groups, `intent` and `physics2`, are missing from it, so nothing at run time defends where they sit. A reordered group keeps every size intact and still breaks every kit built against the old table; the frozen ABI is append-only only if a test says so.

## Done when
- [x] `intent` and `physics2` are in the compatibility test, their offsets checked like the others
- [x] audit ABI-03 holds with no accepted debt (removed from `scripts/audit_baseline.json`)

## Contract
No change to the ABI: the test defends what is already frozen.

Nothing: a kit sees the same table.

## Log
- 2026-09-30: `physics2` (offset 800, 16 bytes) and `intent` (816, 32 bytes)
  added to `frozen[]` and `offs[]`. A `static_assert` now requires the two
  lists to be the same length: they were kept in step by hand, and a group
  added to one alone would check the wrong offsets.
- **Weaker than the audit's wording suggested.** Swapping the two groups in
  `engine_api_table.h` fails the BUILD before this test runs: the header
  static-asserts every group's offset (audit ABI-01, "placed and guarded").
  So these groups were never undefended, and the runtime rows repeat what
  the header's asserts already prove, under the same compiler. The order's
  real value is the invariant: ABI-03 now holds with no exceptions, so the
  NEXT group appended must be listed too, or the audit fails. With two
  accepted exceptions, a third would have looked like more of the same.
