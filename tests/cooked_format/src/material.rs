//! The cooked material format (`.cmat`), read from outside C++.
//!
//! Layout from `modules/assetlib/src/formats/material_asset.cpp::saveMaterial`.
//! Unlike `.cooked`, this one is fully variable-length — every string is a u32
//! length followed by unterminated bytes — so it can only be read by walking it
//! in order. That is exactly why it is worth reading from here: a field added in
//! the middle shifts everything after it, and nothing outside the C++ writer has
//! ever checked that the walk still lands where it should.
//!
//! ```text
//! u32   magic 'CMAT'
//! u32   version (3)
//! str   name
//! str   shaderName
//! str   shaderPath
//! u32   featureMask
//! u32   doubleSided
//! u32   uniformCount
//!   str   name
//!   u32   floatCount
//!   f32[] values
//! u32   textureCount
//!   str   uniform
//!   u32   stage
//!   str   path
//!   str   fallback
//!   str   cooked        (v3)
//! ```

use crate::ReadError;

pub const CMAT_MAGIC: u32 = 0x5441_4D43; // 'CMAT'
pub const CMAT_VERSION: u32 = 3;

#[derive(Debug, Clone)]
pub struct MaterialUniform {
    pub name: String,
    pub values: Vec<f32>,
}

#[derive(Debug, Clone)]
pub struct MaterialTexture {
    pub uniform: String,
    pub stage: u32,
    /// Project-relative SOURCE path, as authored. Empty means "use the fallback".
    pub path: String,
    pub fallback: String,
    /// Cache-relative COOKED path, filled by `engine_build` when packaging.
    /// Empty in a dev project, where `path` resolves through the registry.
    pub cooked: String,
}

#[derive(Debug, Clone)]
pub struct MaterialAsset {
    pub version: u32,
    pub name: String,
    pub shader_name: String,
    pub shader_path: String,
    pub feature_mask: u32,
    pub double_sided: bool,
    pub uniforms: Vec<MaterialUniform>,
    pub textures: Vec<MaterialTexture>,
}

impl MaterialAsset {
    pub fn uniform(&self, name: &str) -> Option<&MaterialUniform> {
        self.uniforms.iter().find(|u| u.name == name)
    }
    pub fn texture(&self, uniform: &str) -> Option<&MaterialTexture> {
        self.textures.iter().find(|t| t.uniform == uniform)
    }
}

/// A cursor that refuses to read past the end rather than panicking, so a
/// truncated or reshaped file produces a diagnosable error instead of a slice
/// index panic that says nothing about the format.
struct Cursor<'a> {
    b: &'a [u8],
    at: usize,
}

impl<'a> Cursor<'a> {
    fn need(&self, n: usize) -> Result<(), ReadError> {
        if self.at + n > self.b.len() {
            return Err(ReadError::TooShort { need: self.at + n, have: self.b.len() });
        }
        Ok(())
    }
    fn u32(&mut self) -> Result<u32, ReadError> {
        self.need(4)?;
        let v = u32::from_le_bytes(self.b[self.at..self.at + 4].try_into().unwrap());
        self.at += 4;
        Ok(v)
    }
    fn f32(&mut self) -> Result<f32, ReadError> {
        self.need(4)?;
        let v = f32::from_le_bytes(self.b[self.at..self.at + 4].try_into().unwrap());
        self.at += 4;
        Ok(v)
    }
    fn string(&mut self) -> Result<String, ReadError> {
        let n = self.u32()? as usize;
        // A length field is the one place a corrupt or reshaped file turns into
        // an enormous allocation. Bounds-check before reserving anything.
        self.need(n)?;
        let s = String::from_utf8_lossy(&self.b[self.at..self.at + n]).into_owned();
        self.at += n;
        Ok(s)
    }
}

pub fn read_material(bytes: &[u8]) -> Result<MaterialAsset, ReadError> {
    let mut c = Cursor { b: bytes, at: 0 };
    let magic = c.u32()?;
    if magic != CMAT_MAGIC {
        return Err(ReadError::BadMagic(magic));
    }
    let version = c.u32()?;
    if version != CMAT_VERSION {
        return Err(ReadError::BadVersion { found: version, expected: CMAT_VERSION });
    }

    let name = c.string()?;
    let shader_name = c.string()?;
    let shader_path = c.string()?;
    let feature_mask = c.u32()?;
    let double_sided = c.u32()? != 0;

    let ucount = c.u32()? as usize;
    let mut uniforms = Vec::with_capacity(ucount.min(1024));
    for _ in 0..ucount {
        let name = c.string()?;
        let n = c.u32()? as usize;
        c.need(n * 4)?;
        let mut values = Vec::with_capacity(n);
        for _ in 0..n {
            values.push(c.f32()?);
        }
        uniforms.push(MaterialUniform { name, values });
    }

    let tcount = c.u32()? as usize;
    let mut textures = Vec::with_capacity(tcount.min(1024));
    for _ in 0..tcount {
        textures.push(MaterialTexture {
            uniform: c.string()?,
            stage: c.u32()?,
            path: c.string()?,
            fallback: c.string()?,
            cooked: c.string()?,
        });
    }

    // Nothing should follow. Trailing bytes mean the writer emitted a section
    // this reader does not know about — which is the same failure as a version
    // bump that was forgotten, and worth reporting rather than ignoring.
    if c.at != bytes.len() {
        return Err(ReadError::TrailingBytes { parsed: c.at, total: bytes.len() });
    }

    Ok(MaterialAsset {
        version,
        name,
        shader_name,
        shader_path,
        feature_mask,
        double_sided,
        uniforms,
        textures,
    })
}

pub fn read_material_from(path: &std::path::Path) -> Result<MaterialAsset, ReadError> {
    let bytes = std::fs::read(path).map_err(|e| ReadError::Io(e.to_string()))?;
    read_material(&bytes)
}
