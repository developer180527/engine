#pragma once
// ── Embedded shader binaries, one backend at a time ─────────────────────────
//
// bgfx's cmake step compiles each .sc to a C array per profile and bin2c emits a
// header per profile (build/shaders/<profile>/<name>.sc.bin.h). This picks the
// right set for the platform and aliases the bgfx-convention names
// (vs_triangle_mtl / _dxbc / _spv) to platform-agnostic macros, so pipeline code
// never mentions a backend.
//
// INCLUDE THIS FROM EXACTLY ONE TRANSLATION UNIT. The generated arrays are
// `static`, so every additional includer gets its own copy of every shader
// binary — silent binary bloat, not a link error. Only program creation needs
// them, so pipeline/programs.cpp is that one place.
//
// It also used to sit inline in forward_pipeline.h: 130 lines of three-way #if in
// a header, recompiled into anything that included the pipeline.
// Compiled shaders — bgfx cmake compiles per-platform; pick the right binary.
// Variable names follow bgfx convention: vs_triangle_mtl, vs_triangle_spv, etc.
// We alias them to a common name so the pipeline code stays platform-agnostic.
#if defined(__APPLE__)
    #include "metal/vs_triangle.sc.bin.h"
    #include "metal/fs_triangle.sc.bin.h"
    #include "metal/vs_shadow.sc.bin.h"
    #include "metal/fs_shadow.sc.bin.h"
    #include "metal/vs_instanced.sc.bin.h"
    #include "metal/vs_skinned.sc.bin.h"
    #include "metal/vs_shadow_instanced.sc.bin.h"
    #include "metal/vs_shadow_skinned.sc.bin.h"
    #include "metal/vs_line.sc.bin.h"
    #include "metal/fs_line.sc.bin.h"
    #define VS_LINE_DATA           vs_line_mtl
    #define VS_LINE_SIZE           sizeof(vs_line_mtl)
    #define FS_LINE_DATA           fs_line_mtl
    #define FS_LINE_SIZE           sizeof(fs_line_mtl)
    #define VS_TRIANGLE_DATA       vs_triangle_mtl
    #define VS_TRIANGLE_SIZE       sizeof(vs_triangle_mtl)
    #define FS_TRIANGLE_DATA       fs_triangle_mtl
    #define FS_TRIANGLE_SIZE       sizeof(fs_triangle_mtl)
    #define VS_SHADOW_DATA         vs_shadow_mtl
    #define VS_SHADOW_SIZE         sizeof(vs_shadow_mtl)
    #define FS_SHADOW_DATA         fs_shadow_mtl
    #define FS_SHADOW_SIZE         sizeof(fs_shadow_mtl)
    #define VS_INSTANCED_DATA      vs_instanced_mtl
    #define VS_INSTANCED_SIZE      sizeof(vs_instanced_mtl)
    #define VS_SKINNED_DATA        vs_skinned_mtl
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_mtl)
    #define VS_SHADOW_INST_DATA    vs_shadow_instanced_mtl
    #define VS_SHADOW_INST_SIZE    sizeof(vs_shadow_instanced_mtl)
    #define VS_SHADOW_SKINNED_DATA vs_shadow_skinned_mtl
    #define VS_SHADOW_SKINNED_SIZE sizeof(vs_shadow_skinned_mtl)
#elif defined(_WIN32)
    #include "dxbc/vs_triangle.sc.bin.h"
    #include "dxbc/fs_triangle.sc.bin.h"
    #include "dxbc/vs_shadow.sc.bin.h"
    #include "dxbc/fs_shadow.sc.bin.h"
    #include "dxbc/vs_instanced.sc.bin.h"
    #include "dxbc/vs_skinned.sc.bin.h"
    #include "dxbc/vs_shadow_instanced.sc.bin.h"
    #include "dxbc/vs_shadow_skinned.sc.bin.h"
    #include "dxbc/vs_line.sc.bin.h"
    #include "dxbc/fs_line.sc.bin.h"
    #define VS_LINE_DATA           vs_line_dxbc
    #define VS_LINE_SIZE           sizeof(vs_line_dxbc)
    #define FS_LINE_DATA           fs_line_dxbc
    #define FS_LINE_SIZE           sizeof(fs_line_dxbc)
    #define VS_TRIANGLE_DATA       vs_triangle_dxbc
    #define VS_TRIANGLE_SIZE       sizeof(vs_triangle_dxbc)
    #define FS_TRIANGLE_DATA       fs_triangle_dxbc
    #define FS_TRIANGLE_SIZE       sizeof(fs_triangle_dxbc)
    #define VS_SHADOW_DATA         vs_shadow_dxbc
    #define VS_SHADOW_SIZE         sizeof(vs_shadow_dxbc)
    #define FS_SHADOW_DATA         fs_shadow_dxbc
    #define FS_SHADOW_SIZE         sizeof(fs_shadow_dxbc)
    #define VS_INSTANCED_DATA      vs_instanced_dxbc
    #define VS_INSTANCED_SIZE      sizeof(vs_instanced_dxbc)
    #define VS_SKINNED_DATA        vs_skinned_dxbc
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_dxbc)
    #define VS_SHADOW_INST_DATA    vs_shadow_instanced_dxbc
    #define VS_SHADOW_INST_SIZE    sizeof(vs_shadow_instanced_dxbc)
    #define VS_SHADOW_SKINNED_DATA vs_shadow_skinned_dxbc
    #define VS_SHADOW_SKINNED_SIZE sizeof(vs_shadow_skinned_dxbc)
#else // Linux — Vulkan (SPIR-V)
    #include "spirv/vs_triangle.sc.bin.h"
    #include "spirv/fs_triangle.sc.bin.h"
    #include "spirv/vs_shadow.sc.bin.h"
    #include "spirv/fs_shadow.sc.bin.h"
    #include "spirv/vs_instanced.sc.bin.h"
    #include "spirv/vs_skinned.sc.bin.h"
    #include "spirv/vs_shadow_instanced.sc.bin.h"
    #include "spirv/vs_shadow_skinned.sc.bin.h"
    #include "spirv/vs_line.sc.bin.h"
    #include "spirv/fs_line.sc.bin.h"
    #define VS_LINE_DATA           vs_line_spv
    #define VS_LINE_SIZE           sizeof(vs_line_spv)
    #define FS_LINE_DATA           fs_line_spv
    #define FS_LINE_SIZE           sizeof(fs_line_spv)
    #define VS_TRIANGLE_DATA       vs_triangle_spv
    #define VS_TRIANGLE_SIZE       sizeof(vs_triangle_spv)
    #define FS_TRIANGLE_DATA       fs_triangle_spv
    #define FS_TRIANGLE_SIZE       sizeof(fs_triangle_spv)
    #define VS_SHADOW_DATA         vs_shadow_spv
    #define VS_SHADOW_SIZE         sizeof(vs_shadow_spv)
    #define FS_SHADOW_DATA         fs_shadow_spv
    #define FS_SHADOW_SIZE         sizeof(fs_shadow_spv)
    #define VS_INSTANCED_DATA      vs_instanced_spv
    #define VS_INSTANCED_SIZE      sizeof(vs_instanced_spv)
    #define VS_SKINNED_DATA        vs_skinned_spv
    #define VS_SKINNED_SIZE        sizeof(vs_skinned_spv)
    #define VS_SHADOW_INST_DATA    vs_shadow_instanced_spv
    #define VS_SHADOW_INST_SIZE    sizeof(vs_shadow_instanced_spv)
    #define VS_SHADOW_SKINNED_DATA vs_shadow_skinned_spv
    #define VS_SHADOW_SKINNED_SIZE sizeof(vs_shadow_skinned_spv)
#endif
