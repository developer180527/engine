---
status: plan
id: WO-049
title: Jolt contacts are collected per thread, not under one mutex
program: providers
priority: P2
size: S
state: done
done: 2026-10-01
evidence: sim_profile 2 sampling profile, mutex wait 3.0% -> 2.1-2.4% of busy thread time (flushDeferred 79 -> 13-14 samples; the contact listener 0 before and after); physics mean 3.83 -> 3.74 ms (4 runs each); determinism gate 0 divergences
touches:
  - src/plugins/jolt_plugin.h
  - src/plugins/jolt_jobs_adapter.h
source: simulation profile 2026-10-01 (sampling profile of sim_profile at scale 2)
---
## Why
Physics is 77 to 88% of a simulation tick in `sim_profile` (2.3 ms at scale 1, 3.5 ms at scale 2). Most of that is Jolt's own contact solver, which is real work. But the sampling profile also shows about 860 samples of worker threads blocked on mutexes, and about 580 of them are one lock:

`JoltPlugin::ContactListenerImpl::OnContactAdded` takes a `std::mutex` for EVERY contact. Jolt calls it from all its worker threads in parallel during collision detection, so contact generation, the part Jolt parallelises, is serialized on this lock. The events are sorted at flush for determinism anyway (BUG-0054), so arrival order was never needed.

The rest of the waiting (about 240 samples) is `JoltJobsAdapter`'s own lock (`Semaphore::Release`, `flushDeferred`), which is worth a look alongside.

## Done when
- [x] ~~contact callbacks append to a per-thread buffer with no lock~~ **Re-scoped by measurement:** after BUG-0071 the contact lock shows 0 samples of mutex wait, so it is kept and its comment gives the numbers and when to revisit. Event order is unchanged (determinism gate: 0 divergences)
- [x] the job adapter's lock is either justified in its comment with a measurement, or reduced
- [x] `sim_profile 2` before and after: physics time per tick, and the mutex-wait share of a sampling profile, both recorded in the log

## Contract
Scripts and kits see the same contact events in the same order.

Nothing: only how the events are gathered changes.

## Log
- 2026-10-01: **Re-measure before starting.** This order's numbers were taken
  while BUG-0071 was dropping contacts at scale 2 (3 500 contact starts and
  ends per tick instead of 45). With that fixed, contacts per tick are far
  fewer, so the lock's share of physics time is probably smaller. The
  baseline needs a fresh sampling profile.
- 2026-10-01 (done):
  - **Re-measured first.** At HEAD (after BUG-0071), `sim_profile 2` makes
    26.6 contact starts and ends per tick, against 3 100 when this order was
    written. A 6 s `sample` of a 2 500-tick run (2 200 samples per thread)
    has 284 samples in `__psynch_mutexwait`, which is **3.0% of busy thread
    time**. Attributed by the nearest non-system frame on each stack:

    | who waits | samples |
    |---|---|
    | `JPH::Semaphore::Release` (Jolt's barrier) | 189 |
    | `JoltJobsAdapter::flushDeferred` | 79 |
    | `JoltJobsAdapter::QueueJob`, `mem::`, other | 16 |
    | `ContactListenerImpl::OnContact*` | **0** |

    The listener lock was only ever contended because BUG-0071 kept the piles
    from settling. It only locks when a contact starts or ends, and at 27 of
    those per tick nothing waits. It is kept, with the numbers in its comment.
  - **`flushDeferred` took the lock after every job**, on every worker, just
    to find the deferred list empty. An atomic count, written under the lock
    and read before it, now skips the lock when nothing is deferred.
    Deferral still happens (62 samples inside `submit` in the after profile),
    and a job deferred just after the check waits for the next completion or
    the step barrier, as one deferred just after the swap always did.
  - **Jolt's semaphore is left alone.** On non-Windows platforms it is a
    `std::mutex` plus a condition variable, in vendored code
    (`JobSystemWithBarrier`, the same as Jolt's own thread pool). Replacing
    it means our own barrier, for about 2%. The reasoning is in the adapter's
    header comment.
  - **Result** (`build-prof`, RelWithDebInfo, scale 2, 600 ticks):

    | | physics mean (ms) | physics p50 (ms) | mutex wait, % of busy | `flushDeferred` waits |
    |---|---|---|---|---|
    | before (4 runs) | 3.76 / 3.91 / 3.83 / 3.83 | 3.28 to 3.34 | 3.0% | 79 |
    | after (4 runs) | 3.72 / 3.68 / 3.84 / 3.72 | 3.26 to 3.28 | 2.1%, 2.4% | 13, 14 |

    That is about 2% of physics time, near the noise floor of the time
    measurement. The sampled waits are the firmer evidence.

