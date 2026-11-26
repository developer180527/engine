## BUG-0076 — A failed init() left its profiler channels and job pool behind
- found:     2026-11-26
- status:    fixed
- class:     memory
- where:     src/runtime/runtime_boot.cpp
- symptom:   a second EngineRuntime in the same process crashed (SIGBUS) after the first one's init() had returned false. Found while writing providers_test (WO-076): a refused provider choice was the first failure path the tests drove twice in one process.
- cause:     init() registers three profiler channels and starts the job pool before the platform, renderer and systems start, and every later failure returned false without undoing them. shutdown() is a no-op for an engine that never finished init(), so the destroyed runtime's channels stayed registered with the global profiler and the next boot called into freed memory. Before WO-076 the paths were real but untested: a platform or renderer that failed to start.
- pinned-by: tests/providers_test.cpp
- lane:      unit
- proof:     providers_test boots a dozen runtimes in one process, most of them refused after the platform starts (an unknown renderer, a renderer with no display, an unknown target key). Every failure path in init() after the first side effect now calls abortInit(), which removes the channels, stops the job pool and shuts the platform down.
