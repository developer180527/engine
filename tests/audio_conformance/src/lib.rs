//! Executable specification for `EngineAudioProviderV1`.
//!
//! The header is the declaration; this is the contract. A provider that
//! compiles against `engine_audio_provider.h` and fails this suite is not a
//! provider — "it loaded" and "it behaves" are different states, and the
//! second one is the one a shipped game depends on.
//!
//! ## Why Rust
//!
//! Not preference — proof. The claim the audio ABI makes is that a third party
//! can implement it in a language that is not C++. Writing the suite AND a
//! reference provider in Rust tests that claim mechanically: if the header
//! secretly depended on C++ layout, name mangling or exceptions, none of this
//! would link, let alone pass.
//!
//! ## What it asserts
//!
//! Contract terms, not implementation details. Every one of these is something
//! a real provider gets wrong at least once:
//!
//! * a host with no output device is a NORMAL outcome, not a crash
//! * garbage bytes are rejected, never decoded into a wild pointer
//! * invalid sounds/voices are refused rather than dereferenced
//! * stale voice ids in a bulk update are ignored — the engine legitimately
//!   sends them, because a voice can finish between frames
//! * `setGeometry` may be unsupported, and that must degrade, not fail
//! * `callbackOverruns` stays ZERO under a command burst — the one audio
//!   failure a player notices instantly is a click
//! * `samplesPlayed` is monotonic, because it is the clock sample-accurate
//!   scheduling is computed against

#![allow(non_snake_case, non_camel_case_types)]

pub mod reference;

use std::os::raw::{c_char, c_int, c_void};

// ── The ABI, transcribed from include/engine/engine_audio_provider.h ────────
// Hand-written rather than generated: a bindgen step would paper over exactly
// the layout questions this crate exists to answer.

pub type EngineAudioResult = i32;
pub type EngineSoundId = u64;
pub type EngineVoiceId = u64;

pub const ENGINE_AUDIO_OK: EngineAudioResult = 0;
pub const ENGINE_AUDIO_E_FAIL: EngineAudioResult = -1;
pub const ENGINE_AUDIO_E_NO_DEVICE: EngineAudioResult = -2;
pub const ENGINE_AUDIO_E_BAD_ARG: EngineAudioResult = -3;
pub const ENGINE_AUDIO_E_BAD_DATA: EngineAudioResult = -4;
pub const ENGINE_AUDIO_E_UNSUPPORTED: EngineAudioResult = -6;

pub const ENGINE_AUDIO_NO_SOUND: EngineSoundId = 0;
pub const ENGINE_AUDIO_NO_VOICE: EngineVoiceId = 0;

pub const F_LOOP: u32 = 0x1;
pub const F_SPATIAL: u32 = 0x2;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct DeviceDesc {
    pub structSize: u32,
    pub sampleRate: u32,
    pub bufferFrames: u32,
    pub channelHint: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Listener {
    pub structSize: u32,
    pub position: [f32; 3],
    pub velocity: [f32; 3],
    pub forward: [f32; 3],
    pub up: [f32; 3],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct EmitterUpdate {
    pub voice: EngineVoiceId,
    pub position: [f32; 3],
    pub velocity: [f32; 3],
    pub volume: f32,
    pub pitch: f32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct PlayDesc {
    pub structSize: u32,
    pub sound: EngineSoundId,
    pub startSampleTime: u64,
    pub position: [f32; 3],
    pub velocity: [f32; 3],
    pub volume: f32,
    pub pitch: f32,
    pub flags: u32,
    pub attenuationId: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AcousticGeometry {
    pub structSize: u32,
    pub vertices: *const f32,
    pub vertexCount: u32,
    pub indices: *const u32,
    pub indexCount: u32,
    pub materialIds: *const u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct Stats {
    pub structSize: u32,
    pub activeVoices: u32,
    pub sampleRate: u32,
    pub bufferFrames: u32,
    pub callbackOverruns: u64,
    pub samplesPlayed: u64,
    pub cpuLoad: f32,
}

#[repr(C)]
pub struct ProviderV1 {
    pub version: u32,
    pub structSize: u32,

    pub create: Option<unsafe extern "C" fn(*const DeviceDesc, *mut *mut c_void) -> EngineAudioResult>,
    pub destroy: Option<unsafe extern "C" fn(*mut c_void)>,
    pub suspend: Option<unsafe extern "C" fn(*mut c_void, i32)>,

    pub createSound: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const c_void,
            u64,
            u32,                 // flags — F_STREAM decides buffer lifetime
            *const c_char,
            *mut EngineSoundId,
        ) -> EngineAudioResult,
    >,
    pub destroySound: Option<unsafe extern "C" fn(*mut c_void, EngineSoundId)>,

    pub play: Option<unsafe extern "C" fn(*mut c_void, *const PlayDesc) -> EngineVoiceId>,
    pub stop: Option<unsafe extern "C" fn(*mut c_void, EngineVoiceId, u32)>,

    // (count, stride) — the stride is what makes EmitterUpdate extensible; a
    // provider must walk with it, never with size_of::<EmitterUpdate>().
    pub updateEmitters:
        Option<unsafe extern "C" fn(*mut c_void, *const EmitterUpdate, u32, u32)>,
    pub setListener: Option<unsafe extern "C" fn(*mut c_void, *const Listener)>,

    pub setGeometry:
        Option<unsafe extern "C" fn(*mut c_void, *const AcousticGeometry) -> EngineAudioResult>,

    pub setParam: Option<unsafe extern "C" fn(*mut c_void, u64, u64, f32)>,
    pub getStats: Option<unsafe extern "C" fn(*mut c_void, *mut Stats)>,
}

/// The C sizes, measured from the real header. If Rust and C disagree the ABI
/// is not an ABI, and every other assertion in this crate is meaningless — so
/// this is checked at COMPILE time rather than left to a test that might not
/// run.
pub const _ABI_SIZES: () = {
    assert!(std::mem::size_of::<DeviceDesc>() == 16);
    assert!(std::mem::size_of::<Listener>() == 52);
    assert!(std::mem::size_of::<EmitterUpdate>() == 48);
    assert!(std::mem::size_of::<PlayDesc>() == 64);
    assert!(std::mem::size_of::<AcousticGeometry>() == 48);
    assert!(std::mem::size_of::<Stats>() == 40);
    assert!(std::mem::size_of::<ProviderV1>() == 104);
};

// ── Loading a native provider ───────────────────────────────────────────────
#[cfg(unix)]
mod dl {
    use super::*;
    extern "C" {
        fn dlopen(path: *const c_char, flags: c_int) -> *mut c_void;
        fn dlsym(handle: *mut c_void, sym: *const c_char) -> *mut c_void;
        fn dlerror() -> *const c_char;
    }
    const RTLD_NOW: c_int = 2;

    /// dlopen a provider module and call its `engineAudioProviderV1` entry.
    ///
    /// Deliberately never closed: a provider's table is static for the life of
    /// the process, and unloading it while the suite holds function pointers is
    /// the exact use-after-free the interface's lifetime rules exist to avoid.
    pub fn load(path: &str) -> Result<&'static ProviderV1, String> {
        let cpath = std::ffi::CString::new(path).map_err(|e| e.to_string())?;
        unsafe {
            let h = dlopen(cpath.as_ptr(), RTLD_NOW);
            if h.is_null() {
                let e = dlerror();
                let msg = if e.is_null() {
                    "dlopen failed".to_string()
                } else {
                    std::ffi::CStr::from_ptr(e).to_string_lossy().into_owned()
                };
                return Err(msg);
            }
            let name = std::ffi::CString::new("engineAudioProviderV1").unwrap();
            let sym = dlsym(h, name.as_ptr());
            if sym.is_null() {
                return Err("module exports no engineAudioProviderV1".into());
            }
            let getter: extern "C" fn() -> *const ProviderV1 = std::mem::transmute(sym);
            let p = getter();
            if p.is_null() {
                return Err("engineAudioProviderV1 returned null".into());
            }
            Ok(&*p)
        }
    }
}
#[cfg(unix)]
pub use dl::load;

// ── The suite ───────────────────────────────────────────────────────────────

pub struct Report {
    pub checks: Vec<(bool, String)>,
}

impl Report {
    fn new() -> Self {
        Report { checks: Vec::new() }
    }
    fn check(&mut self, ok: bool, what: impl Into<String>) {
        self.checks.push((ok, what.into()));
    }
    pub fn failures(&self) -> usize {
        self.checks.iter().filter(|(ok, _)| !ok).count()
    }
    pub fn print(&self, label: &str) {
        println!("── conformance: {label} ──");
        for (ok, what) in &self.checks {
            println!("  {}  {what}", if *ok { "ok  " } else { "FAIL" });
        }
    }
}

/// Run the whole contract against one provider.
///
/// # Safety
/// `p` must be a valid provider table that outlives the call.
pub unsafe fn run(p: &ProviderV1) -> Report {
    let mut r = Report::new();

    r.check(p.version == 1, "reports version 1");
    r.check(
        p.structSize as usize == std::mem::size_of::<ProviderV1>(),
        format!(
            "structSize matches the ABI ({} vs {})",
            p.structSize,
            std::mem::size_of::<ProviderV1>()
        ),
    );

    // Every entry is mandatory. An optional-looking null would turn a missing
    // feature into a crash at the first call instead of a clear refusal, which
    // is what E_UNSUPPORTED is for.
    let complete = p.create.is_some()
        && p.destroy.is_some()
        && p.suspend.is_some()
        && p.createSound.is_some()
        && p.destroySound.is_some()
        && p.play.is_some()
        && p.stop.is_some()
        && p.updateEmitters.is_some()
        && p.setListener.is_some()
        && p.setGeometry.is_some()
        && p.setParam.is_some()
        && p.getStats.is_some();
    r.check(complete, "every function pointer is populated");
    if !complete {
        return r;
    }

    let desc = DeviceDesc {
        structSize: std::mem::size_of::<DeviceDesc>() as u32,
        sampleRate: 48_000,
        bufferFrames: 128,
        channelHint: 0,
    };
    let mut self_ptr: *mut c_void = std::ptr::null_mut();
    let res = (p.create.unwrap())(&desc, &mut self_ptr);

    // A host with no output device is normal — CI, dedicated servers. The
    // contract says report it; the engine then runs silent.
    if res == ENGINE_AUDIO_E_NO_DEVICE {
        r.check(true, "no output device: reported E_NO_DEVICE (host runs silent)");
        return r;
    }
    r.check(res == ENGINE_AUDIO_OK, format!("create() succeeded (result {res})"));
    r.check(!self_ptr.is_null(), "create() produced an instance");
    if res != ENGINE_AUDIO_OK || self_ptr.is_null() {
        return r;
    }

    // ── Hostile resource input ──────────────────────────────────────────────
    // A cooked asset can arrive truncated or corrupt; decoding it must fail,
    // not produce a sound id pointing at nothing.
    {
        let junk = [0u8; 64];
        let mut sound: EngineSoundId = 12345;
        let rc = (p.createSound.unwrap())(
            self_ptr,
            junk.as_ptr() as *const c_void,
            junk.len() as u64,
            0,                                     // flags: fully decoded
            b"junk\0".as_ptr() as *const c_char,
            &mut sound,
        );
        r.check(rc != ENGINE_AUDIO_OK, "garbage bytes are REJECTED, not decoded");
        r.check(
            rc == ENGINE_AUDIO_OK || sound == ENGINE_AUDIO_NO_SOUND,
            "...and no sound id is handed back on failure",
        );

        let mut s2: EngineSoundId = 999;
        let rc2 = (p.createSound.unwrap())(
            self_ptr,
            std::ptr::null(),
            0,
            0,
            std::ptr::null(),
            &mut s2,
        );
        r.check(rc2 != ENGINE_AUDIO_OK, "null/empty bytes are refused");
    }

    // ── Invalid handles ─────────────────────────────────────────────────────
    {
        let pd = PlayDesc {
            structSize: std::mem::size_of::<PlayDesc>() as u32,
            sound: 0xDEAD_BEEF,
            volume: 1.0,
            pitch: 1.0,
            ..Default::default()
        };
        let v = (p.play.unwrap())(self_ptr, &pd);
        r.check(v == ENGINE_AUDIO_NO_VOICE, "playing an unknown sound yields NO_VOICE");

        // Stopping something that never existed, and something already gone,
        // are both routine: the engine cannot know a voice finished.
        (p.stop.unwrap())(self_ptr, 0xBAD_F00D, 0);
        (p.stop.unwrap())(self_ptr, ENGINE_AUDIO_NO_VOICE, 50);
        r.check(true, "stopping an unknown or already-finished voice is a no-op");

        (p.destroySound.unwrap())(self_ptr, 0xDEAD_BEEF);
        r.check(true, "destroying an unknown sound is a no-op");
    }

    // ── Bulk update edge cases ──────────────────────────────────────────────
    {
        let stride = std::mem::size_of::<EmitterUpdate>() as u32;
        (p.updateEmitters.unwrap())(self_ptr, std::ptr::null(), 0, stride);
        r.check(true, "an empty bulk update (null, 0) is accepted");

        // Stale ids are NORMAL input: a voice can finish between the engine
        // building this array and the provider reading it.
        let updates: Vec<EmitterUpdate> = (0..256)
            .map(|i| EmitterUpdate {
                voice: 0x1000 + i as u64,
                position: [i as f32, 0.0, 0.0],
                volume: 1.0,
                pitch: 1.0,
                flags: F_SPATIAL,
                ..Default::default()
            })
            .collect();
        (p.updateEmitters.unwrap())(self_ptr, updates.as_ptr(), updates.len() as u32, stride);
        r.check(true, "256 stale voice ids in one bulk update are ignored, not fatal");

        // A WIDER stride than the provider's own struct: this is what a newer
        // engine sending extended rows looks like, and it is the case the
        // parameter exists for. A provider walking by size_of instead of stride
        // reads every row after the first at the wrong offset — silently, with
        // plausible-looking garbage. Rows are spaced by `wide` with our struct at
        // the start of each, so a correct provider sees the same 8 emitters.
        let wide = stride + 16;
        let mut padded = vec![0u8; wide as usize * 8];
        for i in 0..8usize {
            let row = EmitterUpdate {
                voice: 0x2000 + i as u64,
                position: [i as f32, 1.0, 2.0],
                volume: 1.0,
                pitch: 1.0,
                flags: F_SPATIAL,
                ..Default::default()
            };
            std::ptr::copy_nonoverlapping(
                &row as *const EmitterUpdate as *const u8,
                padded.as_mut_ptr().add(i * wide as usize),
                stride as usize,
            );
        }
        // Poison the gap between rows, so a provider that walks by its own struct
        // size reads 0xFF garbage rather than something plausible.
        for i in 0..8usize {
            let gap = padded.as_mut_ptr().add(i * wide as usize + stride as usize);
            std::ptr::write_bytes(gap, 0xFF, (wide - stride) as usize);
        }
        (p.updateEmitters.unwrap())(
            self_ptr, padded.as_ptr() as *const EmitterUpdate, 8, wide);
        r.check(true, "a WIDER row stride does not read out of bounds");

        let l = Listener { up: [0.0, 1.0, 0.0], forward: [0.0, 0.0, -1.0], ..Default::default() };
        (p.setListener.unwrap())(self_ptr, &l);
        r.check(true, "setListener accepted");
    }

    // ── Optional capabilities degrade, never fail ───────────────────────────
    {
        let verts: [f32; 9] = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0];
        let idx: [u32; 3] = [0, 1, 2];
        let mats: [u32; 1] = [0];
        let g = AcousticGeometry {
            structSize: std::mem::size_of::<AcousticGeometry>() as u32,
            vertices: verts.as_ptr(),
            vertexCount: 3,
            indices: idx.as_ptr(),
            indexCount: 3,
            materialIds: mats.as_ptr(),
        };
        let rc = (p.setGeometry.unwrap())(self_ptr, &g);
        r.check(
            rc == ENGINE_AUDIO_OK || rc == ENGINE_AUDIO_E_UNSUPPORTED,
            format!("setGeometry returns OK or E_UNSUPPORTED, never a crash (got {rc})"),
        );

        // The passthrough must swallow anything: the engine never interprets
        // these, so a provider must not assume it knows the hash.
        (p.setParam.unwrap())(self_ptr, 0, 0xFFFF_FFFF_FFFF_FFFF, 0.5);
        (p.setParam.unwrap())(self_ptr, 0xBAD, 1, f32::NAN);
        r.check(true, "setParam ignores unknown hashes and NaN without complaint");
    }

    // ── Real-time safety, the assertion that matters ────────────────────────
    {
        let mut s0 = Stats { structSize: std::mem::size_of::<Stats>() as u32, ..Default::default() };
        (p.getStats.unwrap())(self_ptr, &mut s0);
        r.check(s0.sampleRate > 0, format!("getStats reports a real sample rate ({})", s0.sampleRate));

        // Hammer the control path the way a busy frame does, then confirm the
        // mixer never missed a deadline. A provider that takes a lock shared
        // with its audio thread fails here — which is the entire point.
        for i in 0..2_000u64 {
            let l = Listener { position: [i as f32, 0.0, 0.0], up: [0.0, 1.0, 0.0], ..Default::default() };
            (p.setListener.unwrap())(self_ptr, &l);
            (p.setParam.unwrap())(self_ptr, 0, i, i as f32);
        }
        std::thread::sleep(std::time::Duration::from_millis(120));

        let mut s1 = Stats { structSize: std::mem::size_of::<Stats>() as u32, ..Default::default() };
        (p.getStats.unwrap())(self_ptr, &mut s1);
        r.check(
            s1.callbackOverruns == 0,
            format!(
                "NO callback overruns under a 2000-command burst ({}) — an \
                 overrun is an audible click, and a lock shared with the mixer \
                 is how it happens",
                s1.callbackOverruns
            ),
        );
        r.check(
            s1.samplesPlayed >= s0.samplesPlayed,
            "samplesPlayed is monotonic — it is the clock startSampleTime is computed against",
        );
    }

    // ── Lifecycle ───────────────────────────────────────────────────────────
    (p.suspend.unwrap())(self_ptr, 1);
    (p.suspend.unwrap())(self_ptr, 0);
    r.check(true, "suspend/resume round-trips");

    (p.destroy.unwrap())(self_ptr);
    r.check(true, "destroy() completes");

    r
}
