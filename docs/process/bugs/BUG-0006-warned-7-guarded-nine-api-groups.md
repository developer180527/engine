## BUG-0006 — warned[7] guarded nine API groups
- found:     2026-08-19
- status:    fixed
- class:     memory
- where:     include/engine/engine_api_client.h
- symptom:   out-of-bounds write on every warning past the seventh group.
- cause:     the array size was a literal typed once; the group enum grew three times after it.
- pinned-by: tests/api_abi_compat_test.cpp
- lane:      unit
- proof:     found while fixing BUG-0005; the array is now sized from the enum.
