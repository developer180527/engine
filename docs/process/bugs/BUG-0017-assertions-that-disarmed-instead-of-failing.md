## BUG-0017 — assertions that disarmed instead of failing
- found:     2026-08-24
- status:    fixed
- class:     coverage
- where:     tests/cooked_format/tests/cshader_ddc.rs
- symptom:   none observable — the guard would simply stop guarding.
- cause:     the four checks pinning standard.shader's parameter offsets against Material::kStd* were written as `if let Some(p) = sh.param("roughness") { assert_eq!(p.offset, 1) }`, which pins the offset when the param is there and passes SILENTLY when it is not. Renaming or dropping `roughness` removed the guard rather than tripping it.
- pinned-by: tests/cooked_format/tests/cshader_ddc.rs
- lane:      unit
- proof:     presence is asserted now, with a message naming the constant and the file to move it in. This was the self-agreeing-check pattern the whole crate exists to replace, reintroduced inside it.
