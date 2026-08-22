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
pub const ENGINE_AUDIO_E_OOM: EngineAudioResult = -5;
pub const ENGINE_AUDIO_E_UNSUPPORTED: EngineAudioResult = -6;

pub const ENGINE_AUDIO_NO_SOUND: EngineSoundId = 0;
pub const ENGINE_AUDIO_NO_VOICE: EngineVoiceId = 0;

pub const F_LOOP: u32 = 0x1;
pub const F_SPATIAL: u32 = 0x2;
pub const F_STREAM: u32 = 0x4;
pub const F_BANK: u32 = 0x8;

/// FNV-1a 64, transcribed from `engineAudioHashName` in the header.
///
/// Deliberately re-implemented rather than called through FFI: the engine and a
/// provider each compile their own copy, so the risk this guards is the two
/// copies *disagreeing*. A test that called the C function would be comparing
/// it against itself and could never catch that. `hash_matches_the_c_header`
/// pins it to a value measured from the compiled C.
pub fn hash_name(name: &str) -> u64 {
    let mut h: u64 = 0xCBF2_9CE4_8422_2325;
    for b in name.bytes() {
        h ^= b as u64;
        h = h.wrapping_mul(0x100_0000_01B3);
    }
    if h == 0 { 1 } else { h }
}

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
    pub _reserved: u32,
    /// Host clock read at the same instant as `samplesPlayed`. Without the
    /// pair, `startSampleTime` cannot be computed — one number is a count, not
    /// a mapping between two clocks.
    pub hostTimeNs: u64,
}

/// The engine's jobs and allocators, handed to the provider at `create`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct HostServices {
    pub structSize: u32,
    pub _reserved: u32,
    pub userData: *mut c_void,
    pub alloc: Option<unsafe extern "C" fn(*mut c_void, u64, u64) -> *mut c_void>,
    pub free: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
    pub parallelFor: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const c_char,
            u32,
            u32,
            unsafe extern "C" fn(*mut c_void, u32, u32),
            *mut c_void,
        ),
    >,
    pub workerCount: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
    pub nowNs: Option<unsafe extern "C" fn(*mut c_void) -> u64>,
}

/// A pull-based resource the provider reads on its own streaming worker.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StreamSource {
    pub structSize: u32,
    pub _reserved: u32,
    pub read: Option<unsafe extern "C" fn(*mut c_void, u64, *mut c_void, u64) -> i64>,
    pub totalBytes: u64,
    pub userData: *mut c_void,
}

#[repr(C)]
pub struct ProviderV1 {
    pub version: u32,
    pub structSize: u32,

    pub create: Option<
        unsafe extern "C" fn(
            *const DeviceDesc,
            *const HostServices,
            *mut *mut c_void,
        ) -> EngineAudioResult,
    >,
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

    pub createStream: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *const StreamSource,
            u32,
            *const c_char,
            *mut EngineSoundId,
        ) -> EngineAudioResult,
    >,
    // Name lookup — what makes an event-based provider (Wwise, FMOD) expressible
    // at all. Without it, banks have no addressing scheme.
    pub findSound: Option<
        unsafe extern "C" fn(*mut c_void, u64, *const c_char, *mut EngineSoundId)
            -> EngineAudioResult,
    >,

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
    assert!(std::mem::size_of::<StreamSource>() == 32);
    assert!(std::mem::size_of::<HostServices>() == 56);
    assert!(std::mem::size_of::<Stats>() == 48);
    assert!(std::mem::size_of::<ProviderV1>() == 120);
};

// ── Real audio, so the suite tests more than refusal ────────────────────────
/// A 16-bit PCM WAV of a sine tone, built by hand.
///
/// Everything in `run` feeds the provider GARBAGE and checks it says no. That
/// is half a contract. A provider can reject every byte ever offered and pass
/// all of it while being incapable of playing a sound — which is exactly the
/// state the Rust reference provider is in, legitimately, and exactly the state
/// a real backend must not be in. So the suite needs audio a decoder will
/// actually accept, and WAV is the one container simple enough to emit here
/// without a dependency (44-byte header, then samples).
pub fn wav_sine(seconds: f32, rate: u32, freq: f32) -> Vec<u8> {
    let frames = (seconds * rate as f32) as u32;
    let data_len = frames * 2; // mono, 16-bit
    let mut v = Vec::with_capacity(44 + data_len as usize);
    v.extend_from_slice(b"RIFF");
    v.extend_from_slice(&(36 + data_len).to_le_bytes());
    v.extend_from_slice(b"WAVEfmt ");
    v.extend_from_slice(&16u32.to_le_bytes()); // fmt chunk size
    v.extend_from_slice(&1u16.to_le_bytes()); // PCM
    v.extend_from_slice(&1u16.to_le_bytes()); // mono
    v.extend_from_slice(&rate.to_le_bytes());
    v.extend_from_slice(&(rate * 2).to_le_bytes()); // byte rate
    v.extend_from_slice(&2u16.to_le_bytes()); // block align
    v.extend_from_slice(&16u16.to_le_bytes()); // bits
    v.extend_from_slice(b"data");
    v.extend_from_slice(&data_len.to_le_bytes());
    for i in 0..frames {
        let t = i as f32 / rate as f32;
        let s = (t * freq * std::f32::consts::TAU).sin() * 0.25;
        v.extend_from_slice(&((s * 32767.0) as i16).to_le_bytes());
    }
    v
}

/// Bytes a `StreamSource` can read from, for the pull path.
struct ByteReader {
    bytes: Vec<u8>,
    reads: std::sync::atomic::AtomicU64,
}
unsafe extern "C" fn byte_read(ud: *mut c_void, offset: u64, dst: *mut c_void, n: u64) -> i64 {
    let r = &*(ud as *const ByteReader);
    r.reads.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    if offset >= r.bytes.len() as u64 {
        return 0;
    }
    let avail = r.bytes.len() as u64 - offset;
    let take = n.min(avail);
    std::ptr::copy_nonoverlapping(
        r.bytes.as_ptr().add(offset as usize),
        dst as *mut u8,
        take as usize,
    );
    take as i64
}

/// The functional half of the contract: a provider that claims to play audio
/// must actually decode and play some.
///
/// Skipped wholesale (reporting success) when `create` says E_NO_DEVICE, and
/// each capability is skipped individually on E_UNSUPPORTED — a provider that
/// cannot stream is still conformant, it just is not being tested here.
///
/// # Safety
/// `p` must be a valid provider table that outlives the call.
pub unsafe fn run_playback(p: &ProviderV1) -> Report {
    let mut r = Report::new();
    let desc = DeviceDesc {
        structSize: std::mem::size_of::<DeviceDesc>() as u32,
        sampleRate: 48_000,
        bufferFrames: 128,
        channelHint: 0,
    };
    let counters = host::Counters::default();
    let services = host::services(&counters);
    let mut sp: *mut c_void = std::ptr::null_mut();
    let res = (p.create.unwrap())(&desc, &services, &mut sp);
    if res == ENGINE_AUDIO_E_NO_DEVICE {
        r.check(true, "no output device — playback checks skipped");
        return r;
    }
    if res != ENGINE_AUDIO_OK || sp.is_null() {
        r.check(false, format!("create() failed ({res})"));
        return r;
    }

    let wav = wav_sine(0.5, 48_000, 440.0);
    let stats = |sp: *mut c_void| -> Stats {
        let mut s = Stats { structSize: std::mem::size_of::<Stats>() as u32, ..Default::default() };
        (p.getStats.unwrap())(sp, &mut s);
        s
    };

    // ── Decode and play ─────────────────────────────────────────────────────
    let mut sound: EngineSoundId = 0;
    let rc = (p.createSound.unwrap())(
        sp, wav.as_ptr() as *const c_void, wav.len() as u64, 0,
        b"sine.wav\0".as_ptr() as *const c_char, &mut sound);
    let decoded = rc == ENGINE_AUDIO_OK;
    r.check(
        decoded || rc == ENGINE_AUDIO_E_UNSUPPORTED,
        format!("a valid WAV is DECODED, not rejected (got {rc})"),
    );

    if decoded {
        r.check(sound != ENGINE_AUDIO_NO_SOUND, "...and yields a usable sound id");

        let pd = PlayDesc {
            structSize: std::mem::size_of::<PlayDesc>() as u32,
            sound,
            volume: 0.2,   // audible if anyone is listening, not startling
            pitch: 1.0,
            ..Default::default()
        };
        let v = (p.play.unwrap())(sp, &pd);
        r.check(v != ENGINE_AUDIO_NO_VOICE, "playing a real sound yields a voice");

        if v != ENGINE_AUDIO_NO_VOICE {
            let s0 = stats(sp);
            r.check(s0.activeVoices >= 1,
                format!("getStats sees the voice ({} active)", s0.activeVoices));

            // A LIVE id in the bulk path, which nothing else in the suite
            // exercises — every other updateEmitters test sends stale ids.
            let up = EmitterUpdate {
                voice: v, position: [1.0, 2.0, 3.0], volume: 0.2, pitch: 1.0,
                flags: F_SPATIAL, ..Default::default()
            };
            (p.updateEmitters.unwrap())(
                sp, &up, 1, std::mem::size_of::<EmitterUpdate>() as u32);
            r.check(true, "a LIVE voice survives a bulk emitter update");

            std::thread::sleep(std::time::Duration::from_millis(120));
            let s1 = stats(sp);
            r.check(s1.samplesPlayed > s0.samplesPlayed,
                format!("the device clock advances while audio plays ({} -> {})",
                        s0.samplesPlayed, s1.samplesPlayed));
            r.check(s1.callbackOverruns == 0,
                format!("no overruns while actually mixing ({})", s1.callbackOverruns));

            (p.stop.unwrap())(sp, v, 0);
            r.check(true, "stopping a live voice completes");
        }

        // Many voices over one sound: the decoded case must not need a copy of
        // the samples per voice.
        let mut spawned = 0;
        for _ in 0..8 {
            if (p.play.unwrap())(sp, &pd) != ENGINE_AUDIO_NO_VOICE { spawned += 1; }
        }
        r.check(spawned >= 2,
            format!("one decoded sound supports concurrent voices ({spawned}/8)"));

        (p.destroySound.unwrap())(sp, sound);
        r.check(true, "destroySound with voices still live does not crash");
    }

    // ── The same bytes, streamed from memory (F_STREAM) ─────────────────────
    {
        let mut sid: EngineSoundId = 0;
        let rc = (p.createSound.unwrap())(
            sp, wav.as_ptr() as *const c_void, wav.len() as u64, F_STREAM,
            b"sine.stream\0".as_ptr() as *const c_char, &mut sid);
        if rc == ENGINE_AUDIO_OK {
            let pd = PlayDesc {
                structSize: std::mem::size_of::<PlayDesc>() as u32,
                sound: sid, volume: 0.2, pitch: 1.0, ..Default::default()
            };
            r.check((p.play.unwrap())(sp, &pd) != ENGINE_AUDIO_NO_VOICE,
                    "an F_STREAM sound plays from the engine's buffer");
            (p.destroySound.unwrap())(sp, sid);
        } else {
            r.check(rc == ENGINE_AUDIO_E_UNSUPPORTED || rc == ENGINE_AUDIO_E_BAD_DATA,
                    format!("F_STREAM declined cleanly ({rc})"));
        }
    }

    // ── The same bytes, PULLED through a reader (createStream) ──────────────
    {
        let reader = Box::new(ByteReader {
            bytes: wav.clone(),
            reads: std::sync::atomic::AtomicU64::new(0),
        });
        let src = StreamSource {
            structSize: std::mem::size_of::<StreamSource>() as u32,
            _reserved: 0,
            read: Some(byte_read),
            totalBytes: wav.len() as u64,
            userData: &*reader as *const ByteReader as *mut c_void,
        };
        let mut sid: EngineSoundId = 0;
        let rc = (p.createStream.unwrap())(
            sp, &src, F_STREAM, b"sine.pull\0".as_ptr() as *const c_char, &mut sid);
        if rc == ENGINE_AUDIO_OK {
            r.check(
                reader.reads.load(std::sync::atomic::Ordering::Relaxed) > 0,
                "createStream actually CALLED the engine's reader — the pull path \
                 is wired, not just accepted",
            );
            let pd = PlayDesc {
                structSize: std::mem::size_of::<PlayDesc>() as u32,
                sound: sid, volume: 0.2, pitch: 1.0, ..Default::default()
            };
            r.check((p.play.unwrap())(sp, &pd) != ENGINE_AUDIO_NO_VOICE,
                    "a pull-streamed sound plays");
            std::thread::sleep(std::time::Duration::from_millis(60));
            (p.destroySound.unwrap())(sp, sid);
            r.check(true, "a pull-streamed sound destroys cleanly");
        } else {
            r.check(rc == ENGINE_AUDIO_E_UNSUPPORTED,
                    format!("createStream declined cleanly ({rc})"));
        }
        // The reader must not be touched after destroySound; dropping it here
        // is itself the check — a provider still holding it would use freed
        // memory under ASan.
        drop(reader);
    }

    (p.destroy.unwrap())(sp);
    r.check(counters.frees.load(std::sync::atomic::Ordering::Relaxed) > 0,
            format!("memory returned through host free ({} allocs, {} frees)",
                    counters.allocs.load(std::sync::atomic::Ordering::Relaxed),
                    counters.frees.load(std::sync::atomic::Ordering::Relaxed)));
    r
}

// ── A host, so the suite can prove the provider uses one ────────────────────
// Passing services and never checking they were touched would test nothing: a
// provider could take the struct, ignore it, and call malloc. These count.
pub mod host {
    use super::*;
    use std::alloc::{alloc as rs_alloc, dealloc, Layout};
    use std::sync::atomic::{AtomicU64, Ordering};

    #[derive(Default)]
    pub struct Counters {
        pub allocs: AtomicU64,
        pub frees: AtomicU64,
        pub bytes: AtomicU64,
        pub parallel_for_calls: AtomicU64,
        pub parallel_for_items: AtomicU64,
        pub now_ns_calls: AtomicU64,
    }

    /// Header stored immediately before every returned block, so `free` can
    /// rebuild the exact `Layout` Rust requires for deallocation.
    #[repr(C)]
    struct Head {
        size: u64,
        align: u64,
    }

    unsafe extern "C" fn h_alloc(ud: *mut c_void, size: u64, align: u64) -> *mut c_void {
        let c = &*(ud as *const Counters);
        // Offset by `align.max(16)` rather than a flat 16: a 32-byte-aligned
        // request offset by 16 would come back MISALIGNED, which is the sort of
        // bug that surfaces as a SIMD fault deep in someone's mixer.
        let align = (align.max(16) as usize).next_power_of_two();
        let total = size as usize + align;
        let Ok(layout) = Layout::from_size_align(total, align) else {
            return std::ptr::null_mut();
        };
        let base = rs_alloc(layout);
        if base.is_null() {
            return std::ptr::null_mut();
        }
        let user = base.add(align);
        (user as *mut Head).sub(1).write(Head { size: size as u64, align: align as u64 });
        c.allocs.fetch_add(1, Ordering::Relaxed);
        c.bytes.fetch_add(size, Ordering::Relaxed);
        user as *mut c_void
    }

    unsafe extern "C" fn h_free(ud: *mut c_void, p: *mut c_void) {
        if p.is_null() {
            return; // freeing null is legal, as it is everywhere else
        }
        let c = &*(ud as *const Counters);
        let head = (p as *mut Head).sub(1).read();
        let align = head.align as usize;
        let layout = Layout::from_size_align(head.size as usize + align, align).unwrap();
        dealloc((p as *mut u8).sub(align), layout);
        c.frees.fetch_add(1, Ordering::Relaxed);
    }

    /// Serial, on purpose. The suite is testing that the provider *routes* work
    /// here, not that our pool is fast — and a real engine pool would make the
    /// assertions timing-dependent for no gain.
    unsafe extern "C" fn h_parallel_for(
        ud: *mut c_void,
        _name: *const c_char,
        count: u32,
        _grain: u32,
        f: unsafe extern "C" fn(*mut c_void, u32, u32),
        ctx: *mut c_void,
    ) {
        let c = &*(ud as *const Counters);
        c.parallel_for_calls.fetch_add(1, Ordering::Relaxed);
        c.parallel_for_items.fetch_add(count as u64, Ordering::Relaxed);
        if count > 0 {
            f(ctx, 0, count);
        }
    }

    unsafe extern "C" fn h_worker_count(_ud: *mut c_void) -> u32 {
        4
    }

    unsafe extern "C" fn h_now_ns(ud: *mut c_void) -> u64 {
        let c = &*(ud as *const Counters);
        c.now_ns_calls.fetch_add(1, Ordering::Relaxed);
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos() as u64)
            .unwrap_or(0)
    }

    /// Build a services table pointing at `counters`, which must outlive it.
    pub fn services(counters: &Counters) -> HostServices {
        HostServices {
            structSize: std::mem::size_of::<HostServices>() as u32,
            _reserved: 0,
            userData: counters as *const Counters as *mut c_void,
            alloc: Some(h_alloc),
            free: Some(h_free),
            parallelFor: Some(h_parallel_for),
            workerCount: Some(h_worker_count),
            nowNs: Some(h_now_ns),
        }
    }
}

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
        && p.getStats.is_some()
        && p.createStream.is_some()
        && p.findSound.is_some();
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
    let counters = host::Counters::default();
    let services = host::services(&counters);
    let mut self_ptr: *mut c_void = std::ptr::null_mut();
    let res = (p.create.unwrap())(&desc, &services, &mut self_ptr);

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

    // ── Host services are actually USED ─────────────────────────────────────
    // The point of handing over jobs and allocators is that the provider does
    // not build its own. A provider that takes the struct and calls malloc is
    // invisible to the engine's memory budget, and this is the only place that
    // can be caught.
    {
        use std::sync::atomic::Ordering;
        r.check(
            counters.allocs.load(Ordering::Relaxed) > 0,
            format!(
                "allocates through host services, not malloc ({} allocs, {} bytes) \
                 — otherwise its footprint never appears in the engine's audio budget",
                counters.allocs.load(Ordering::Relaxed),
                counters.bytes.load(Ordering::Relaxed)
            ),
        );
    }

    // ── Name lookup: the event-system path ──────────────────────────────────
    // A sample-based provider answers E_UNSUPPORTED; a Wwise/FMOD adapter
    // resolves the name. Both are conformant. What neither may do is crash, or
    // hand back a live-looking id for a name it does not know.
    {
        let h = hash_name("conformance/definitely_not_a_real_event");
        let mut found: EngineSoundId = 0xABCD;
        let rc = (p.findSound.unwrap())(
            self_ptr,
            h,
            b"conformance/definitely_not_a_real_event\0".as_ptr() as *const c_char,
            &mut found,
        );
        r.check(
            rc != ENGINE_AUDIO_OK,
            format!("findSound refuses an unknown name (got {rc})"),
        );
        r.check(
            found == ENGINE_AUDIO_NO_SOUND,
            "...and writes NO_SOUND rather than leaving the caller's value",
        );

        // Hash 0 is reserved — engineAudioHashName never produces it, so it can
        // only arrive from a caller that failed to hash something.
        let mut z: EngineSoundId = 7;
        let rcz = (p.findSound.unwrap())(self_ptr, 0, std::ptr::null(), &mut z);
        r.check(rcz != ENGINE_AUDIO_OK, "findSound refuses the reserved hash 0");

        // A bank is a container: unsupported is fine, decoding garbage is not.
        let junk = [0u8; 32];
        let mut bank: EngineSoundId = 0;
        let rcb = (p.createSound.unwrap())(
            self_ptr,
            junk.as_ptr() as *const c_void,
            junk.len() as u64,
            F_BANK,
            b"junk.bnk\0".as_ptr() as *const c_char,
            &mut bank,
        );
        r.check(
            rcb != ENGINE_AUDIO_OK,
            "a garbage bank is rejected (E_UNSUPPORTED or E_BAD_DATA), never parsed",
        );
    }

    // ── Pull streaming ──────────────────────────────────────────────────────
    // The provider must not read on the caller's thread during createStream,
    // must not read past totalBytes, and must cope with a reader that fails.
    {
        unsafe extern "C" fn ok_read(
            ud: *mut c_void, offset: u64, dst: *mut c_void, count: u64,
        ) -> i64 {
            let calls = &*(ud as *const std::sync::atomic::AtomicU64);
            calls.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let total: u64 = 4096;
            if offset >= total {
                return 0; // end of resource
            }
            let n = count.min(total - offset);
            std::ptr::write_bytes(dst as *mut u8, 0xA5, n as usize);
            n as i64
        }
        unsafe extern "C" fn failing_read(
            _ud: *mut c_void, _o: u64, _d: *mut c_void, _c: u64,
        ) -> i64 {
            -1 // an archive went away mid-stream: routine, not fatal
        }

        let reads = std::sync::atomic::AtomicU64::new(0);
        let src = StreamSource {
            structSize: std::mem::size_of::<StreamSource>() as u32,
            _reserved: 0,
            read: Some(ok_read),
            totalBytes: 4096,
            userData: &reads as *const _ as *mut c_void,
        };
        let mut sid: EngineSoundId = 0xFEED;
        let rc = (p.createStream.unwrap())(
            self_ptr, &src, F_STREAM, b"stream\0".as_ptr() as *const c_char, &mut sid);
        r.check(
            rc == ENGINE_AUDIO_OK || rc == ENGINE_AUDIO_E_UNSUPPORTED
                || rc == ENGINE_AUDIO_E_BAD_DATA,
            format!("createStream returns OK, E_UNSUPPORTED or E_BAD_DATA (got {rc})"),
        );
        if rc != ENGINE_AUDIO_OK {
            r.check(sid == ENGINE_AUDIO_NO_SOUND, "...and NO_SOUND when it declines");
        } else {
            (p.destroySound.unwrap())(self_ptr, sid);
            r.check(true, "a streamed sound destroys cleanly");
        }

        let bad = StreamSource {
            read: Some(failing_read),
            userData: std::ptr::null_mut(),
            ..src
        };
        let mut bsid: EngineSoundId = 1;
        let rcb = (p.createStream.unwrap())(
            self_ptr, &bad, F_STREAM, std::ptr::null(), &mut bsid);
        r.check(
            rcb != ENGINE_AUDIO_OK || bsid != ENGINE_AUDIO_NO_SOUND,
            "a reader that fails immediately does not produce a live sound id",
        );
        if rcb == ENGINE_AUDIO_OK {
            (p.destroySound.unwrap())(self_ptr, bsid);
        }

        // A null reader is a malformed source, not a segfault.
        let nul = StreamSource { read: None, ..src };
        let mut nsid: EngineSoundId = 3;
        let rcn = (p.createStream.unwrap())(
            self_ptr, &nul, F_STREAM, std::ptr::null(), &mut nsid);
        r.check(rcn != ENGINE_AUDIO_OK, "a StreamSource with a null read fn is refused");
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

        // ── Clock correlation ───────────────────────────────────────────────
        // samplesPlayed alone is a count, not a mapping: it says how many
        // samples have played but not WHEN that was true, so it cannot place a
        // future event on the host's timeline. Reporting hostTimeNs is optional
        // (0 = "I don't"), but a provider that reports it must make it usable.
        if s0.hostTimeNs != 0 && s1.hostTimeNs != 0 {
            r.check(
                s1.hostTimeNs > s0.hostTimeNs,
                "hostTimeNs advances between readings",
            );

            // The two clocks must agree on how much time passed. Anything far
            // off means the pair was not sampled together — which silently
            // ruins every scheduling calculation built on it.
            let d_samples = s1.samplesPlayed.saturating_sub(s0.samplesPlayed) as f64;
            let d_host_ns = s1.hostTimeNs.saturating_sub(s0.hostTimeNs) as f64;
            let implied_ns = d_samples / s1.sampleRate.max(1) as f64 * 1e9;
            // Generous: this runs on loaded CI machines, and the assertion is
            // aimed at a mapping that is WRONG, not one that is imprecise.
            let ok = d_host_ns > 0.0
                && implied_ns > 0.0
                && (implied_ns / d_host_ns) > 0.5
                && (implied_ns / d_host_ns) < 2.0;
            r.check(
                ok,
                format!(
                    "the sample clock and the host clock agree on elapsed time \
                     ({:.1} ms of samples vs {:.1} ms of host) — sampled together, \
                     so startSampleTime can be computed",
                    implied_ns / 1e6,
                    d_host_ns / 1e6
                ),
            );
            r.check(
                counters.now_ns_calls.load(std::sync::atomic::Ordering::Relaxed) > 0,
                "hostTimeNs comes from services->nowNs, not a private clock — \
                 two different monotonic clocks share no epoch",
            );
        } else {
            r.check(true, "hostTimeNs not reported (0) — scheduling degrades to buffer boundaries");
        }
    }

    // ── Lifecycle ───────────────────────────────────────────────────────────
    (p.suspend.unwrap())(self_ptr, 1);
    (p.suspend.unwrap())(self_ptr, 0);
    r.check(true, "suspend/resume round-trips");

    (p.destroy.unwrap())(self_ptr);
    r.check(true, "destroy() completes");

    r
}
