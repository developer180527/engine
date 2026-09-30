---
status: plan
id: WO-049
title: Jolt contacts are collected per thread, not under one mutex
program: providers
priority: P2
size: S
state: todo
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
- [ ] contact callbacks append to a per-thread buffer with no lock; the flush merges and sorts them exactly as today, so the event order is unchanged (determinism gate: 0 divergences)
- [ ] the job adapter's lock is either justified in its comment with a measurement, or reduced
- [ ] `sim_profile 2` before and after: physics time per tick, and the mutex-wait share of a sampling profile, both recorded in the log

## Contract
Scripts and kits see the same contact events in the same order.

Nothing: only how the events are gathered changes.

## Log
- 2026-10-01: **Re-measure before starting.** This order's numbers were taken
  while BUG-0071 was dropping contacts at scale 2 (3 500 contact starts and
  ends per tick instead of 45). With that fixed, contacts per tick are far
  fewer, so the lock's share of physics time is probably smaller. The
  baseline needs a fresh sampling profile.
