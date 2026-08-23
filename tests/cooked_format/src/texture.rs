//! The cooked texture format (`.ctex`), read from outside C++.
//!
//! Layout from `modules/assetlib/include/assetlib/texture_asset.h`:
//!
//! ```text
//! TextureHeader   32 bytes (static_assert'd)
//!   u32 magic 'TEX '
//!   u32 version (2)
//!   u32 width, height, channels
//!   u32 format      (TextureFormatId)
//!   u32 mipCount    (0 from a v1 file reads as 1)
//!   u8  _pad[4]
//! pixels          the whole mip chain, largest first
//! ```
//!
//! ## Why the mip walk is the part worth checking
//!
//! The payload is one flat blob and its interpretation depends entirely on the
//! format's BLOCK geometry. A 6×6 ASTC mip is `ceil(w/6)*ceil(h/6)` blocks, not
//! `ceil(w/4)*ceil(h/4)` — the header's own comment records that the original
//! math hard-coded 4×4 and "getting it wrong reads a neighbouring mip as pixel
//! data". Recomputing the chain here from the declared format and comparing
//! against the file's actual length is a real check on that arithmetic, and it
//! is the kind of error that produces a plausible-looking wrong image rather
//! than a failure.

use crate::ReadError;

pub const CTEX_MAGIC: u32 = 0x5445_5820; // 'TEX '
pub const CTEX_HEADER_BYTES: usize = 32;

/// `TextureFormatId` from texture_asset.h. Transcribed rather than derived, so a
/// renumber on the C++ side shows up here.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TexFormat {
    Rgba8 = 0,
    Bc7 = 1,
    Bc5 = 2,
    Bc1 = 3,
    Bc3 = 4,
    Astc4x4 = 5,
    Astc6x6 = 6,
    Astc8x8 = 7,
    Etc2 = 8,
    Etc2a = 9,
    EacRg11 = 10,
}

impl TexFormat {
    pub fn from_u32(v: u32) -> Option<Self> {
        use TexFormat::*;
        Some(match v {
            0 => Rgba8, 1 => Bc7, 2 => Bc5, 3 => Bc1, 4 => Bc3,
            5 => Astc4x4, 6 => Astc6x6, 7 => Astc8x8,
            8 => Etc2, 9 => Etc2a, 10 => EacRg11,
            _ => return None,
        })
    }

    /// (block width, block height, bytes per block) — mirrors `texBlockDims`.
    pub fn block(self) -> (u32, u32, u32) {
        use TexFormat::*;
        match self {
            Bc1 | Etc2 => (4, 4, 8),
            Bc3 | Bc5 | Bc7 | Astc4x4 | Etc2a | EacRg11 => (4, 4, 16),
            Astc6x6 => (6, 6, 16),
            Astc8x8 => (8, 8, 16),
            Rgba8 => (1, 1, 4),
        }
    }

    pub fn is_block_compressed(self) -> bool {
        self != TexFormat::Rgba8 && self.block().0 > 1
    }
}

#[derive(Debug, Clone)]
pub struct TextureAsset {
    pub version: u32,
    pub width: u32,
    pub height: u32,
    pub channels: u32,
    pub format: TexFormat,
    pub mip_count: u32,
    pub payload_bytes: usize,
}

/// Bytes one mip level occupies at the given size.
pub fn mip_bytes(fmt: TexFormat, w: u32, h: u32) -> u64 {
    let (bw, bh, bytes) = fmt.block();
    // Ceiling division: a 5-texel row in a 4-wide block format still costs two
    // blocks. Flooring here is the classic way to under-read the chain.
    let blocks_x = (w.max(1) as u64 + bw as u64 - 1) / bw as u64;
    let blocks_y = (h.max(1) as u64 + bh as u64 - 1) / bh as u64;
    blocks_x * blocks_y * bytes as u64
}

/// Total bytes for a whole mip chain starting at `w`×`h`.
pub fn chain_bytes(fmt: TexFormat, w: u32, h: u32, mips: u32) -> u64 {
    let (mut mw, mut mh) = (w, h);
    let mut total = 0u64;
    for _ in 0..mips.max(1) {
        total += mip_bytes(fmt, mw, mh);
        mw = (mw / 2).max(1);
        mh = (mh / 2).max(1);
    }
    total
}

pub fn read_texture(bytes: &[u8]) -> Result<TextureAsset, ReadError> {
    if bytes.len() < CTEX_HEADER_BYTES {
        return Err(ReadError::TooShort { need: CTEX_HEADER_BYTES, have: bytes.len() });
    }
    let u32_at = |o: usize| u32::from_le_bytes(bytes[o..o + 4].try_into().unwrap());

    let magic = u32_at(0);
    if magic != CTEX_MAGIC {
        return Err(ReadError::BadMagic(magic));
    }
    let raw_fmt = u32_at(20);
    let format = TexFormat::from_u32(raw_fmt)
        .ok_or(ReadError::UnknownFormat(raw_fmt))?;

    Ok(TextureAsset {
        version: u32_at(4),
        width: u32_at(8),
        height: u32_at(12),
        channels: u32_at(16),
        format,
        // A v1 file wrote 0 into what is now mipCount; the C++ reader treats
        // that as 1 and so does this.
        mip_count: u32_at(24).max(1),
        payload_bytes: bytes.len() - CTEX_HEADER_BYTES,
    })
}

impl TextureAsset {
    /// What the mip chain SHOULD occupy, from the declared dimensions.
    pub fn expected_payload(&self) -> u64 {
        chain_bytes(self.format, self.width, self.height, self.mip_count)
    }

    /// The check that matters: the file's actual payload must match what the
    /// declared format and mip count imply. A mismatch means the writer and this
    /// reader disagree about block geometry, and every consumer downstream is
    /// reading one mip as another.
    pub fn payload_matches(&self) -> bool {
        self.payload_bytes as u64 == self.expected_payload()
    }
}

pub fn read_texture_from(path: &std::path::Path) -> Result<TextureAsset, ReadError> {
    let bytes = std::fs::read(path).map_err(|e| ReadError::Io(e.to_string()))?;
    read_texture(&bytes)
}
