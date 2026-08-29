## BUG-0040 — the test fixture's own allocator raced, under the sanitizer written to catch races
- found:     2026-08-27
- status:    fixed
- class:     threading
- where:     tests/audio_provider_asan_test.cpp
- symptom:   TSan reported races inside libc++'s red-black tree — `__tree_remove`, `__find_equal`, `__insert_node_at` — in the test written to prove the audio provider was clean.
- cause:     the fixture tracked live blocks in an unsynchronised `std::map`, and the provider is EXPLICITLY allowed to allocate off the game thread: the ABI hands it `parallelFor` and tells it to decode there. So the fixture's own bookkeeping was the racing party, not the code under test. Left alone, a corrupted tree would eventually have surfaced as "the provider freed a pointer we never handed out" — the fixture accusing the code it was written to vindicate, which is the worst possible direction for a false positive.
- pinned-by: tests/audio_provider_asan_test.cpp
- lane:      tsan
- proof:     one mutex over the whole record rather than three, because `live`, `peak` and `blocks` have to move together — separate locks would make the invariant unprovable while looking safer. The TSan lane over this test is the regression check.
- note:      recorded 2026-08-29, having been fixed two days earlier and written down only as prose in `open-questions.md`. It had no ledger entry, no id, and no `pinned-by`, so nothing connected the fix to the defect. One of four found in that file at once.

