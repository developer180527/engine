//! The DDC record manifest, read from outside C++.
//!
//! A cook can produce several files — a cooked mesh plus sibling `.ctex`
//! textures. Each member is stored under its own content hash, and a small
//! manifest under the cook key names them, so a hit can never yield a mesh
//! missing its textures.
//!
//! ```text
//! ddc-manifest-v1\n
//! <blobKey>\t<name>\n     ... one per member; name "@" is the primary output
//! ```
//!
//! ## Why an independent reader, when there is already a fuzzer
//!
//! `tests/fuzz_ddc_manifest_test.cpp` throws hostile bytes at the C++ parser and
//! found a real bug doing it — a manifest with no `@` member returned success
//! having written nothing, so the caller committed Ready with a cooked path that
//! did not exist.
//!
//! Fuzzing asks "does it survive garbage". This asks the other question: given
//! a manifest the engine WROTE, does it say what it should? Those catch
//! different things. A writer that stopped emitting the primary member, or
//! started emitting a member name with a path separator in it, is well-formed
//! by every rule the fuzzer knows and wrong anyway.
//!
//! The name rule is the security-relevant one and is transcribed from
//! `isPlainFilename`: a member name becomes a filesystem path beside the primary
//! output, so a separator or a `..` in it is a write outside the intended
//! directory. The C++ side rejects rather than sanitizes; so does this.

use crate::ReadError;

pub const MANIFEST_MAGIC: &str = "ddc-manifest-v1";
pub const PRIMARY_NAME: &str = "@";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Member {
    /// Content hash the member's bytes are stored under.
    pub blob_key: String,
    /// `"@"` for the primary output, otherwise a sibling filename.
    pub name: String,
}

impl Member {
    pub fn is_primary(&self) -> bool { self.name == PRIMARY_NAME }
}

#[derive(Debug, Clone)]
pub struct Manifest {
    pub members: Vec<Member>,
}

impl Manifest {
    pub fn primary(&self) -> Option<&Member> {
        self.members.iter().find(|m| m.is_primary())
    }
    pub fn siblings(&self) -> impl Iterator<Item = &Member> {
        self.members.iter().filter(|m| !m.is_primary())
    }
}

/// Mirrors `isPlainFilename` in modules/assetlib/src/ddc/manifest.cpp — ALL of
/// it, including the final path-level check.
///
/// A member name is used as a path beside the primary output, so anything that
/// could escape that directory has to be refused rather than cleaned up — a
/// manifest we do not fully understand is a miss, not a best effort.
///
/// The point of an independent reader is that it is an independent and COMPLETE
/// transcription; one that mirrors most of a rule, while saying it mirrors the
/// rule, quietly weakens the guarantee it exists to provide. The last clause
/// below is redundant with the character rejections above on every platform we
/// currently build for — and it is transcribed anyway, because "redundant today"
/// is a property of the other checks, not of this one, and the C++ can change
/// which clause is doing the work without either side noticing.
pub fn is_plain_filename(name: &str) -> bool {
    if name.is_empty() || name.len() > 255 { return false; }
    if name == "." || name == ".." { return false; }
    // Separators of either flavour, plus the colon that carries a Windows drive
    // letter or an NTFS alternate data stream.
    if name.contains('/') || name.contains('\\') || name.contains(':') { return false; }
    // A name is a filesystem path, not a payload: control characters have no
    // business in one and are a classic way to disguise what a name really is.
    if name.bytes().any(|c| c < 0x20 || c == 0x7f) { return false; }
    // Belt and braces, from the C++: the path itself must agree it is nothing
    // but a filename.
    //   p.filename() == p        -> no directory component
    //   !p.has_root_directory()  -> not "/foo"
    //   !p.has_root_name()       -> not "C:foo" or a UNC "\\\\server"
    let p = std::path::Path::new(name);
    if p.file_name().map(|f| f.as_encoded_bytes()) != Some(name.as_bytes()) { return false; }
    if p.has_root() { return false; }
    if p.components().count() != 1 { return false; }
    true
}

#[derive(Debug)]
pub enum ManifestError {
    BadMagic(String),
    /// A line that is not `<key>\t<name>`.
    MalformedLine { line: usize, text: String },
    /// A member name that could escape the output directory.
    UnsafeName { line: usize, name: String },
    /// No `@` member. The C++ fetch treats this as a MISS, because a record
    /// that yields no primary output is not a hit however many siblings it has.
    NoPrimary,
    MultiplePrimaries(usize),
    /// The manifest is not valid UTF-8. Refused rather than lossily repaired —
    /// see `read_manifest_bytes` for why that mattered more here than anywhere
    /// else in this crate, and why refusing is deliberately stricter than C++.
    NotUtf8 { at: usize, preview: String },
}

impl std::fmt::Display for ManifestError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ManifestError::BadMagic(g) =>
                write!(f, "expected magic {MANIFEST_MAGIC:?}, found {g:?}"),
            ManifestError::MalformedLine { line, text } =>
                write!(f, "line {line} is not <blobKey>\\t<name>: {text:?}"),
            ManifestError::UnsafeName { line, name } => write!(
                f,
                "line {line} names member {name:?}, which is not a plain filename — \
                 member names become paths beside the primary output"
            ),
            ManifestError::NoPrimary => write!(
                f,
                "no \"@\" member: the record yields no primary output, which is a \
                 MISS and not a hit"
            ),
            ManifestError::MultiplePrimaries(n) =>
                write!(f, "{n} \"@\" members — the primary output is ambiguous"),
            ManifestError::NotUtf8 { at, preview } => write!(
                f,
                "not valid UTF-8 at byte {at} ({preview:?} after lossy repair) — \
                 refused rather than repaired, because the repair happens BEFORE \
                 the name-safety check and can only make a name look safer than \
                 the bytes the engine would actually use"
            ),
        }
    }
}

/// Parses and VALIDATES. The two are not separated on purpose: every rule below
/// is one the C++ fetch enforces before it will materialize anything, so a
/// parser that returned an invalid manifest would be describing a state the
/// engine never accepts.
pub fn read_manifest(text: &str) -> Result<Manifest, ManifestError> {
    let mut lines = text.split('\n');
    let magic = lines.next().unwrap_or("");
    if magic != MANIFEST_MAGIC {
        return Err(ManifestError::BadMagic(magic.to_string()));
    }

    let mut members = Vec::new();
    for (i, raw) in lines.enumerate() {
        // A trailing newline leaves one empty final field; that is the end of
        // the file, not a malformed record.
        if raw.is_empty() { continue; }
        let lineno = i + 2;
        let Some((key, name)) = raw.split_once('\t') else {
            return Err(ManifestError::MalformedLine { line: lineno, text: raw.to_string() });
        };
        if key.is_empty() || name.is_empty() {
            return Err(ManifestError::MalformedLine { line: lineno, text: raw.to_string() });
        }
        if name != PRIMARY_NAME && !is_plain_filename(name) {
            return Err(ManifestError::UnsafeName { line: lineno, name: name.to_string() });
        }
        members.push(Member { blob_key: key.to_string(), name: name.to_string() });
    }

    let primaries = members.iter().filter(|m| m.is_primary()).count();
    match primaries {
        0 => return Err(ManifestError::NoPrimary),
        1 => {}
        n => return Err(ManifestError::MultiplePrimaries(n)),
    }
    Ok(Manifest { members })
}

/// A manifest as it comes off disk.
///
/// ## Why the lossy decode here was the worst of the four
///
/// This used to be `read_manifest(&String::from_utf8_lossy(bytes))`, which meant
/// `is_plain_filename` — the check that decides whether a member name can escape
/// its directory — RAN AGAINST THE REPAIRED TEXT AND NOT THE BYTES. Every invalid
/// byte became U+FFFD, which is three bytes, none of them a separator and none of
/// them a control character, so the repair could only ever make a name look
/// safer than it was. The C++ validates the raw bytes (`for (unsigned char c :
/// name)`), so the two sides were not even inspecting the same string.
///
/// ## Deliberately stricter than the C++, and the reason
///
/// A POSIX filename is an arbitrary byte string, so `ddcFetchRecord` ACCEPTS a
/// member name that is not valid UTF-8. This refuses it. That is a divergence
/// and it is the intended one: sibling names are GENERATED by the cookers
/// (`<uuid>_t0.ctex`), never carried over from a source filename, so no manifest
/// the engine writes can trip it — while a manifest arriving from a shared store
/// with a non-UTF-8 name in it is exactly the thing worth stopping to look at
/// rather than guessing about.
pub fn read_manifest_bytes(bytes: &[u8]) -> Result<Manifest, ManifestError> {
    match std::str::from_utf8(bytes) {
        Ok(text) => read_manifest(text),
        Err(e) => Err(ManifestError::NotUtf8 {
            at: e.valid_up_to(),
            preview: String::from_utf8_lossy(bytes)
                .chars()
                .skip(e.valid_up_to().saturating_sub(16))
                .take(48)
                .collect(),
        }),
    }
}

pub fn read_manifest_from(path: &std::path::Path) -> Result<Manifest, ReadError> {
    let bytes = std::fs::read(path).map_err(|e| ReadError::Io(e.to_string()))?;
    read_manifest_bytes(&bytes).map_err(|e| ReadError::Manifest(e.to_string()))
}
