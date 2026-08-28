## BUG-0013 — the source-import upload path drops cooked roughness/metallic
- found:     2026-08-23
- status:    open
- class:     logic
- where:     src/runtime/services/async_loader/upload.cpp
- symptom:   a source-imported mesh shades with 0.7 / 0.0 regardless of what its material says.
- cause:     the path memcpy's baseColorFactor and nothing else, so the roughness and metallic values MaterialGPUData carries are never applied. Predates the migration.
- pinned-by: none
- lane:      unit
- proof:     NOT FIXED. Found while auditing what the migration changed — applying the carried values would have altered shading on every source-imported mesh, which a behaviour-preserving migration must not do. Recorded rather than fixed in passing; it needs its own before/after on real content.
