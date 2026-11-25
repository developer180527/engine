## BUG-0074 — An exception from a parallelFor body left tickets pointing at a dead stack frame
- found:     2026-11-26
- status:    fixed
- class:     memory
- where:     src/core/jobs/jobs_native.cpp
- symptom:   a `parallelFor` body that throws aborted the process (exit 134).
- cause:     a throw on the caller's own range unwound `parallelFor` while tickets for the job, which lives on the caller's stack, were still queued; a worker later ran a ticket into the freed frame. A throw on a worker's range escaped the worker thread, which is `std::terminate`. A throwing `run()` body also ended the process, with no message saying which job.
- pinned-by: tests/jobs_torture_test.cpp
- lane:      unit (quick) and stress (full)
- proof:     case `throw`. The first exception from any range is kept, no further range starts, the call waits for every ticket and then rethrows to the caller; 50 parallelFors afterwards prove the pool still works. Under ASan+UBSan and TSan, no report. A throwing `run()` body now prints that a job threw and terminates (documented in jobs.h: there is nobody to deliver it to).
