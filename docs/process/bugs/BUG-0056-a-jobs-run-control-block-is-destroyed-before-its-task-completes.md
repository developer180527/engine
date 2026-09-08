## BUG-0056 — a jobs::run control block is destroyed before its task completes
- found:     2026-09-08
- status:    open
- class:     threading
- where:     src/runtime/jobs/jobs_enkits.cpp
- symptom:   `Assertion failed: (GetIsComplete()), function ~ICompletable, file TaskScheduler.h, line 493` — enkiTS destroying a task set that has not finished. Seen roughly once in fourteen runs of `determinism_gate_test` with four competing `stress_physics` processes saturating the machine; not seen at all without that artificial contention, and not seen in the `unit` lane.
- cause:     NOT ESTABLISHED. `jobs::run` heap-allocates a `RunTask`, adds it to the enkiTS pipe and records it in `g_inflight`, where `jobs::pumpMain()` later sweeps the finished ones. The assert says something destroys one that is not complete — either the sweep's completion test races the task, or a shutdown path clears `g_inflight` while tasks are live. Which was not narrowed: it surfaced while fixing BUG-0055 and is a different layer.
- lane:      determinism
- note:      SEPARATE FROM BUG-0055 and found underneath it. That bug was in `JoltJobsAdapter`'s use of the pool; this is in the pool facade itself, so it is reachable by any `jobs::run` caller, not just physics. The determinism gate is simply the heaviest `jobs::run` user in the tree — it now submits more `physics.job` tasks than before, because BUG-0055's fix defers nested jobs and flushes them, which is likely why the exposure went up rather than down. Reproducing it wants a TSan build under the same contention. Until it closes, `determinism_gate_gating` stays out of the `unit` lane: what it measures is ready to gate, but a test that aborts under load cannot gate anything.
