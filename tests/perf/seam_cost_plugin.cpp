// The far side of a real dynamic-library boundary, so the call in
// seam_cost_bench.cpp genuinely cannot be inlined or devirtualised. Kept in its
// own TU and its own shared library for exactly that reason: a same-binary
// function pointer is a constant the optimiser folds, which is why that row of
// the benchmark measures ~0 and the cross-.so row does not.
#include <cstdint>

extern "C" void seamDraw(uint64_t* acc, const uint32_t* data, uint32_t i) {
    *acc += (uint64_t)data[i] * 3u + 1u;
}
