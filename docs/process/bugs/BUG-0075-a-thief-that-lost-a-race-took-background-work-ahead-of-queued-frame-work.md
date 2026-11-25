## BUG-0075 — A thief that lost a race took Background work ahead of queued Frame work
- found:     2026-11-26
- status:    fixed
- class:     threading
- where:     src/core/jobs/jobs_native.cpp
- symptom:   with the pool saturated, Background jobs ran while Frame jobs were still queued: 2 410 times in 30 rounds of 200 + 200 jobs on 12 threads.
- cause:     `Deque::steal` returned null both for an empty queue and for a lost compare-and-swap race, and `findTask` treated both as "nothing at this priority" and moved on to lower ones. With eleven thieves on one queue, most of them lose most races.
- pinned-by: tests/jobs_torture_test.cpp
- lane:      unit (quick) and stress (full)
- proof:     case `priority` (no Background job may start while more Frame jobs are unstarted than there are threads). 0 inversions in 30 rounds, repeated, on macOS arm64 and Linux arm64. The first fix (retry the steal on the spot) cost single-producer submission 19% (381 -> 453 ns/job, spread 1%): the losers hammered the cache line the producer reads on every push. The kept fix ends the thread's pass instead and lets it come back after its usual pause; submission cost returned to 362 ns.
