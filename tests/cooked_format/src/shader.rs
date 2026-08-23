//! The cooked shader format (`.cshader`), read from outside C++.
//!
//! Layout from `modules/assetlib/src/formats/shader_asset.cpp::saveShader`:
//!
//! ```text
//! u32   magic 'CSHD'
//! u32   version (1)
//! str   name
//! u32   featureCount   ; str  each
//! u32   paramCount
//!   str name ; u32 type ; str uniform ; u32 offset ; f32[4] defaults
//! u32   samplerCount
//!   str name ; str uniform ; u32 stage ; str fallback
//! u32   variantCount
//!   u32 featureMask ; u32 profile
//!   u32 vsOffset ; u32 vsSize ; u32 fsOffset ; u32 fsSize
//! u32   blobSize ; u8[] blob
//! ```
//!
//! ## What is worth checking here that a round-trip cannot see
//!
//! The variants index into `blob` by OFFSET AND SIZE. Nothing in the format
//! forces those ranges to be inside the blob, or to be non-overlapping, or to
//! be present at all — and a variant pointing past the end is bytecode handed
//! to a GPU driver from beyond the allocation. The C++ reader inflates the same
//! numbers the C++ writer emitted, so a round-trip agrees with itself no matter
//! what those offsets say.
//!
//! `ShaderParam::offset` has the same shape of problem: it is a float component
//! within a shared vec4 uniform, so an offset of 7 in a 4-float uniform writes
//! into whatever register follows.

use crate::ReadError;

pub const CSHD_MAGIC: u32 = 0x4448_5343; // 'CSHD'
pub const CSHD_VERSION: u32 = 1;

/// `ShaderProfileId`. Transcribed, so a renumber on the C++ side surfaces here.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Profile {
    Metal = 0,
    Spirv = 1,
    Dx11 = 2,
    Dx12 = 3,
    Glsl = 4,
}

impl Profile {
    pub fn from_u32(v: u32) -> Option<Self> {
        use Profile::*;
        Some(match v { 0 => Metal, 1 => Spirv, 2 => Dx11, 3 => Dx12, 4 => Glsl, _ => return None })
    }
    pub fn name(self) -> &'static str {
        use Profile::*;
        match self { Metal => "metal", Spirv => "spirv", Dx11 => "dx11", Dx12 => "dx12", Glsl => "glsl" }
    }
}

/// `ShaderParamType`, and how many floats each consumes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ParamType { Float = 0, Vec2 = 1, Vec3 = 2, Vec4 = 3, Color = 4 }

impl ParamType {
    pub fn from_u32(v: u32) -> Option<Self> {
        use ParamType::*;
        Some(match v { 0 => Float, 1 => Vec2, 2 => Vec3, 3 => Vec4, 4 => Color, _ => return None })
    }
    pub fn components(self) -> u32 {
        use ParamType::*;
        match self { Float => 1, Vec2 => 2, Vec3 => 3, Vec4 | Color => 4 }
    }
}

#[derive(Debug, Clone)]
pub struct ShaderParam {
    pub name: String,
    pub ty: ParamType,
    /// The vec4 uniform this packs into ("u_params").
    pub uniform: String,
    /// Float component within that uniform.
    pub offset: u32,
    pub defaults: [f32; 4],
}

#[derive(Debug, Clone)]
pub struct ShaderSampler {
    pub name: String,
    pub uniform: String,
    pub stage: u32,
    pub fallback: String,
}

#[derive(Debug, Clone)]
pub struct ShaderVariant {
    pub feature_mask: u32,
    pub profile: u32,
    pub vs_offset: u32,
    pub vs_size: u32,
    pub fs_offset: u32,
    pub fs_size: u32,
}

#[derive(Debug, Clone)]
pub struct ShaderAsset {
    pub version: u32,
    pub name: String,
    pub features: Vec<String>,
    pub params: Vec<ShaderParam>,
    pub samplers: Vec<ShaderSampler>,
    pub variants: Vec<ShaderVariant>,
    pub blob_size: usize,
}

/// A hard cap from shader_asset.h: 32 features is 4 billion variants, and
/// anything near it means the closed-feature-list rule has been abandoned.
pub const MAX_SHADER_FEATURES: usize = 8;

impl ShaderAsset {
    pub fn variant(&self, feature_mask: u32, profile: u32) -> Option<&ShaderVariant> {
        self.variants.iter().find(|v| v.feature_mask == feature_mask && v.profile == profile)
    }
    pub fn param(&self, name: &str) -> Option<&ShaderParam> {
        self.params.iter().find(|p| p.name == name)
    }
    pub fn sampler(&self, name: &str) -> Option<&ShaderSampler> {
        self.samplers.iter().find(|s| s.name == name)
    }

    /// Every variant's bytecode ranges must land INSIDE the blob. Nothing in the
    /// format enforces it, and a range past the end is bytecode read from beyond
    /// the allocation and handed to a GPU driver.
    pub fn variant_ranges_in_bounds(&self) -> Result<(), String> {
        for (i, v) in self.variants.iter().enumerate() {
            for (what, off, size) in [
                ("vs", v.vs_offset, v.vs_size),
                ("fs", v.fs_offset, v.fs_size),
            ] {
                let end = off as u64 + size as u64;
                if end > self.blob_size as u64 {
                    return Err(format!(
                        "variant {i} ({} mask {:#x} profile {}) {what} range {}..{} \
                         runs past the {}-byte blob",
                        self.name, v.feature_mask, v.profile, off, end, self.blob_size
                    ));
                }
                // DELIBERATE, and a decision rather than an invariant: today
                // shader_cooker.cpp always emits both stages, so a zero-size
                // range can only be a writer bug. The day a depth-only or
                // shadow-caster variant lands — no fragment stage at all, which
                // is a legitimate thing to cook — this fires, and the right
                // response is to make the rule "vs is never empty, fs may be"
                // rather than to delete the check.
                if size == 0 {
                    return Err(format!(
                        "variant {i} ({} mask {:#x} profile {}) has an EMPTY {what} — \
                         a variant with no bytecode binds nothing and draws nothing",
                        self.name, v.feature_mask, v.profile
                    ));
                }
            }
        }
        Ok(())
    }

    /// A parameter packs into a vec4 at a float offset, so `offset + components`
    /// must fit in 4 — otherwise it writes into whatever register follows.
    ///
    /// WIDENED TO u64, like `variant_ranges_in_bounds` above. `offset` is read
    /// raw from the file, so `offset + components` in u32 overflows on exactly
    /// the input this function exists to reject: at `offset = u32::MAX` a debug
    /// build panicked ("attempt to add with overflow") and a RELEASE build
    /// wrapped to a small number and returned Ok — the bounds check accepting
    /// the most out-of-bounds value representable. Two functions apart, one had
    /// been widened and the other had not.
    pub fn params_fit_their_uniforms(&self) -> Result<(), String> {
        for p in &self.params {
            let end = p.offset as u64 + p.ty.components() as u64;
            if end > 4 {
                return Err(format!(
                    "param '{}' is {:?} at offset {} of '{}' — needs components \
                     {}..{} of a vec4, so it spills into the next register",
                    p.name, p.ty, p.offset, p.uniform, p.offset, end
                ));
            }
        }
        Ok(())
    }

    /// featureMask bits must all name a declared feature. A mask bit with no
    /// feature is a variant nothing can ever ask for — cooked, shipped, dead.
    pub fn feature_masks_are_declared(&self) -> Result<(), String> {
        let declared = if self.features.len() >= 32 { u32::MAX }
                       else { (1u32 << self.features.len()) - 1 };
        for v in &self.variants {
            if v.feature_mask & !declared != 0 {
                return Err(format!(
                    "variant mask {:#x} sets bits outside the {} declared feature(s)",
                    v.feature_mask, self.features.len()
                ));
            }
        }
        Ok(())
    }
}

struct Cursor<'a> { b: &'a [u8], at: usize }

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
        self.need(n)?;   // before any allocation — a corrupt length is otherwise a huge reserve
        // Refused, not repaired: from_utf8_lossy would substitute U+FFFD per bad
        // byte and hand back a name that looks merely unusual, after which every
        // comparison runs against text the engine never wrote.
        let s = crate::utf8(&self.b[self.at..self.at + n], self.at, "a .cshader string field")?;
        self.at += n;
        Ok(s)
    }
}

pub fn read_shader(bytes: &[u8]) -> Result<ShaderAsset, ReadError> {
    let mut c = Cursor { b: bytes, at: 0 };
    let magic = c.u32()?;
    if magic != CSHD_MAGIC {
        return Err(ReadError::BadMagic(magic));
    }
    let version = c.u32()?;
    if version != CSHD_VERSION {
        return Err(ReadError::BadVersion { found: version, expected: CSHD_VERSION });
    }

    let name = c.string()?;

    let fcount = c.u32()? as usize;
    let mut features = Vec::with_capacity(fcount.min(64));
    for _ in 0..fcount { features.push(c.string()?); }

    let pcount = c.u32()? as usize;
    let mut params = Vec::with_capacity(pcount.min(1024));
    for _ in 0..pcount {
        let name = c.string()?;
        let raw_ty = c.u32()?;
        let ty = ParamType::from_u32(raw_ty).ok_or(ReadError::UnknownFormat(raw_ty))?;
        let uniform = c.string()?;
        let offset = c.u32()?;
        let defaults = [c.f32()?, c.f32()?, c.f32()?, c.f32()?];
        params.push(ShaderParam { name, ty, uniform, offset, defaults });
    }

    let scount = c.u32()? as usize;
    let mut samplers = Vec::with_capacity(scount.min(1024));
    for _ in 0..scount {
        samplers.push(ShaderSampler {
            name: c.string()?,
            uniform: c.string()?,
            stage: c.u32()?,
            fallback: c.string()?,
        });
    }

    let vcount = c.u32()? as usize;
    let mut variants = Vec::with_capacity(vcount.min(4096));
    for _ in 0..vcount {
        variants.push(ShaderVariant {
            feature_mask: c.u32()?,
            profile: c.u32()?,
            vs_offset: c.u32()?,
            vs_size: c.u32()?,
            fs_offset: c.u32()?,
            fs_size: c.u32()?,
        });
    }

    let blob_size = c.u32()? as usize;
    c.need(blob_size)?;
    c.at += blob_size;

    if c.at != bytes.len() {
        return Err(ReadError::TrailingBytes { parsed: c.at, total: bytes.len() });
    }

    Ok(ShaderAsset { version, name, features, params, samplers, variants, blob_size })
}

pub fn read_shader_from(path: &std::path::Path) -> Result<ShaderAsset, ReadError> {
    let bytes = std::fs::read(path).map_err(|e| ReadError::Io(e.to_string()))?;
    read_shader(&bytes)
}
