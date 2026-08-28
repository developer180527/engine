## BUG-0029 — "Windows hosts do everything" was true of x64 only
- found:     2026-08-26
- status:    fixed
- class:     build
- where:     modules/assetlib/src/formats/shader_asset.cpp
- symptom:   cooked_format_conformance failed on Windows arm64: "shaderc failed (exit 1)|Error: Unable to load DXC compiler." The whole shader cook fails, so every shader in the project, on that architecture only.
- cause:     profileCookableOnThisHost returned `p < kProfileCount` for all of _WIN32. The D3D compilers bgfx vendors are PREBUILT x86-64 DLLs — d3dcompiler_47.dll, dxcompiler.dll and dxil.dll are all "PE32+ x86-64" — and a native arm64 process cannot load an x86-64 DLL, because the emulator switches whole processes rather than libraries.
- pinned-by: tests/shader_cooker_test.cpp
- lane:      unit
- proof:     mutation FROM macOS — restoring "Windows hosts do everything" fails two table rows ("win arm64 dx11", "win arm64 dx12"); reverted, it passes. `file` confirms all three vendored DLLs are PE32+ x86-64. The rule became a PURE function over (os, arch) so every combination is checkable from any machine — the guard used to answer only for the host it was compiled for, which is why the one platform whose answer was wrong had nothing checking it. profileCookableOnThisHost is now a one-line call into the same table, asserted to agree with it.
