## BUG-0021 — std::memcpy without <cstring>, in four files
- found:     2026-08-24
- status:    fixed
- class:     build
- where:     src/
- symptom:   the Linux CI legs failed to compile. macOS never noticed.
- cause:     libc++ pulls <cstring> in transitively; libstdc++ does not.
- pinned-by: .github/workflows/ci.yml
- lane:      unit
- proof:     found by GREPPING for the pattern rather than by waiting for CI. The build stops at the first error, so CI could only ever report one of the four — three more red runs to find them one at a time. Re-verified after the fix: zero files under src/ or modules/ use std::memcpy/memset/memcmp/strlen/strcmp without the include.
