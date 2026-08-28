## BUG-0031 — src/core/info.md said the allocator never pools
- found:     2026-08-27
- status:    fixed
- class:     logic
- where:     src/core/info.md
- symptom:   the doc described mem_counters as "COUNTS + forwards to malloc/free; never pools — ASan/leaks keep working", gated by ENGINE_MEM_COUNT.
- cause:     ENGINE_MEM_ROUTE defaults to 1, so the DEFAULT build routes every allocation into the tagged heaps — the opposite of "never pools". Forwarding to malloc is the ENGINE_MEM_ROUTE=0 path, which the root CMakeLists sets automatically under heap sanitizers. The doc described the fallback as though it were the default.
- pinned-by: src/core/info.md
- lane:      docs
- proof:     found by re-reading the doc rather than bumping its date when BUG-0030's staleness flag pointed at it — which is what the flag is for. The entry now also states the alignment contract that BUG-0028 turned out to hinge on, since nothing described it before.
