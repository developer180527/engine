#pragma once
// ── debug_lines — the engine's debug vertices, in this renderer's format ─────
// The engine's collector (core/debug_draw.h) is a service between providers:
// physics, scripts and kits write RGBA vertices to it without knowing which
// renderer runs (WO-087). This renderer converts them, field by field, into its
// line layout (programs.cpp, m_lineLayout: three floats, then Color0 as four
// normalised bytes). Converted, not copied as memory, so a change to either
// format cannot silently scramble the other. GPU-free, so a test can check it.
#include <cstdint>

#include "core/debug_draw.h"

namespace render {

struct LineVertex { float x, y, z; uint8_t rgba[4]; };
static_assert(sizeof(LineVertex) == 16, "m_lineLayout is 3 floats and 4 bytes");

inline void toLineVertices(const dbg::DebugVertex* in, uint32_t count, LineVertex* out) {
    for (uint32_t i = 0; i < count; ++i) {
        const dbg::DebugVertex& v = in[i];
        out[i] = { v.x, v.y, v.z, { v.color.r, v.color.g, v.color.b, v.color.a } };
    }
}

} // namespace render
