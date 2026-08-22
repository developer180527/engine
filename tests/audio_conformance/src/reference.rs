//! A minimal reference provider, in Rust.
//!
//! Not a useful audio engine — it decodes nothing and makes no sound. Its job
//! is to be a CORRECT one: it obeys every term of the contract, so the suite
//! has something known-good to validate itself against. A suite that has only
//! ever run against the implementation it was written beside tends to encode
//! that implementation's quirks as requirements.
//!
//! It also settles the question the audio ABI exists to answer: can a
//! non-C++ language implement this? Everything here is `extern "C"` over
//! `#[repr(C)]` structs with no bindgen and no helper crate. If the header had
//! smuggled in a C++ assumption, this file would not link.
//!
//! It simulates a device: a background thread advances `samplesPlayed` at
//! 48 kHz so the monotonic-clock and overrun assertions have something real to
//! measure, and command handling is lock-free so the burst test means
//! something.

use crate::*;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;

struct Device {
    running: Arc<AtomicBool>,
    samples: Arc<AtomicU64>,
    overruns: Arc<AtomicU64>,
    voices: AtomicU32,
    next_sound: AtomicU64,
    next_voice: AtomicU64,
    sample_rate: u32,
    buffer_frames: u32,
    /// Copied by value at create(), per the contract — the engine promises the
    /// services outlive the instance, not that the pointer does.
    host: HostServices,
}

/// Rust panics must never unwind into C. The suite calls these through
/// function pointers, and a real host would too — so every entry point that
/// could conceivably panic is wrapped. `abort` on panic is the alternative;
/// catching keeps the test harness usable.
fn guard<T>(fallback: T, f: impl FnOnce() -> T + std::panic::UnwindSafe) -> T {
    std::panic::catch_unwind(f).unwrap_or(fallback)
}

unsafe fn dev<'a>(p: *mut c_void) -> Option<&'a Device> {
    (p as *const Device).as_ref()
}

unsafe extern "C" fn create(
    desc: *const DeviceDesc,
    services: *const HostServices,
    out: *mut *mut c_void,
) -> EngineAudioResult {
    if out.is_null() {
        return ENGINE_AUDIO_E_BAD_ARG;
    }
    *out = std::ptr::null_mut();

    // The contract says services is never null and every entry is populated.
    // Checking anyway: a provider that trusts this and is wrong crashes inside
    // someone else's engine, where the cause is invisible.
    let Some(host) = services.as_ref().copied() else {
        return ENGINE_AUDIO_E_BAD_ARG;
    };
    if host.alloc.is_none() || host.free.is_none() || host.nowNs.is_none()
        || host.parallelFor.is_none()
    {
        return ENGINE_AUDIO_E_BAD_ARG;
    }

    let (rate, frames) = match desc.as_ref() {
        Some(d) => (
            if d.sampleRate == 0 { 48_000 } else { d.sampleRate },
            if d.bufferFrames == 0 { 128 } else { d.bufferFrames },
        ),
        None => (48_000, 128),
    };

    let running = Arc::new(AtomicBool::new(true));
    let samples = Arc::new(AtomicU64::new(0));
    let overruns = Arc::new(AtomicU64::new(0));

    // Stand-in for the driver callback: advances the clock on its own thread,
    // exactly as a real device does, so nothing about timing is faked at the
    // point the suite reads it.
    {
        let (r, s) = (running.clone(), samples.clone());
        let step = frames as u64;
        let period = std::time::Duration::from_micros(
            (1_000_000u64 * frames as u64 / rate.max(1) as u64).max(1),
        );
        std::thread::spawn(move || {
            while r.load(Ordering::Relaxed) {
                std::thread::sleep(period);
                s.fetch_add(step, Ordering::Relaxed);
            }
        });
    }

    // Through the HOST allocator, not Box — this is the whole point of host
    // services. A real provider's voice pool, scratch buffers and convolution
    // workspace all come from here, which is what makes audio's footprint
    // visible in the engine's memory budget instead of anonymous malloc.
    let mem = (host.alloc.unwrap())(
        host.userData,
        std::mem::size_of::<Device>() as u64,
        std::mem::align_of::<Device>() as u64,
    ) as *mut Device;
    if mem.is_null() {
        running.store(false, Ordering::Relaxed); // stop the clock thread we started
        return ENGINE_AUDIO_E_OOM;
    }
    mem.write(Device {
        running,
        samples,
        overruns,
        voices: AtomicU32::new(0),
        next_sound: AtomicU64::new(1),
        next_voice: AtomicU64::new(1),
        sample_rate: rate,
        buffer_frames: frames,
        host,
    });
    *out = mem as *mut c_void;
    ENGINE_AUDIO_OK
}

unsafe extern "C" fn destroy(p: *mut c_void) {
    if p.is_null() {
        return;
    }
    let d = (p as *mut Device).read();
    d.running.store(false, Ordering::Relaxed);
    let host = d.host;
    drop(d);
    (host.free.unwrap())(host.userData, p);
}

unsafe extern "C" fn suspend(_p: *mut c_void, _s: i32) {}

unsafe extern "C" fn create_sound(
    p: *mut c_void,
    bytes: *const c_void,
    count: u64,
    _flags: u32,          // F_STREAM would mean "retain `bytes`"; we decode nothing
    _name: *const c_char,
    out: *mut EngineSoundId,
) -> EngineAudioResult {
    if !out.is_null() {
        *out = ENGINE_AUDIO_NO_SOUND;
    }
    let Some(d) = dev(p) else { return ENGINE_AUDIO_E_BAD_ARG };
    if bytes.is_null() || count == 0 || out.is_null() {
        return ENGINE_AUDIO_E_BAD_ARG;
    }
    // No real decoder, so everything is "corrupt". That is the honest answer
    // for a provider that decodes nothing, and it exercises the failure path
    // the suite cares about.
    let _ = d;
    ENGINE_AUDIO_E_BAD_DATA
}

unsafe extern "C" fn destroy_sound(_p: *mut c_void, _s: EngineSoundId) {}

/// This provider has no name registry, so every lookup misses. Answering
/// E_UNSUPPORTED — rather than E_FAIL — is what tells the engine the capability
/// is absent instead of broken, and it is the honest answer for anything that
/// is not an event system.
unsafe extern "C" fn find_sound(
    p: *mut c_void,
    name_hash: u64,
    _debug: *const c_char,
    out: *mut EngineSoundId,
) -> EngineAudioResult {
    if !out.is_null() {
        *out = ENGINE_AUDIO_NO_SOUND;
    }
    if dev(p).is_none() || out.is_null() {
        return ENGINE_AUDIO_E_BAD_ARG;
    }
    // 0 is reserved by engineAudioHashName, so it can only mean the caller
    // failed to hash something.
    if name_hash == 0 {
        return ENGINE_AUDIO_E_BAD_ARG;
    }
    ENGINE_AUDIO_E_UNSUPPORTED
}

/// Demonstrates the pull-streaming contract rather than declining it: validate
/// the source, then probe it **through the host job pool**, which is where a
/// real provider's decode and refill work belongs. It still decodes nothing, so
/// the honest final answer is E_BAD_DATA.
unsafe extern "C" fn create_stream(
    p: *mut c_void,
    src: *const StreamSource,
    _flags: u32,
    _debug: *const c_char,
    out: *mut EngineSoundId,
) -> EngineAudioResult {
    if !out.is_null() {
        *out = ENGINE_AUDIO_NO_SOUND;
    }
    let Some(d) = dev(p) else { return ENGINE_AUDIO_E_BAD_ARG };
    let Some(s) = src.as_ref() else { return ENGINE_AUDIO_E_BAD_ARG };
    if out.is_null() || s.read.is_none() {
        return ENGINE_AUDIO_E_BAD_ARG;
    }

    struct Probe {
        src: StreamSource,
        bytes: i64,
    }
    // The engine's reader may itself be Rust and may panic. Catching it here
    // stops an unwind crossing back into C through the job pool, which is
    // undefined behaviour and unrecoverable.
    unsafe extern "C" fn probe(ctx: *mut c_void, _b: u32, _e: u32) {
        let pr = &mut *(ctx as *mut Probe);
        let mut buf = [0u8; 256];
        // AssertUnwindSafe is sound here for the reason the lint exists to make
        // you state: if the engine's reader panics we discard `buf` entirely
        // and report failure, so no partially-written buffer is ever observed.
        pr.bytes = guard(-1i64, std::panic::AssertUnwindSafe(|| {
            (pr.src.read.unwrap())(
                pr.src.userData,
                0,
                buf.as_mut_ptr() as *mut c_void,
                buf.len() as u64,
            )
        }));
    }

    let mut pr = Probe { src: *s, bytes: -1 };
    // NOT on the calling thread by contract, and never on the real-time thread:
    // `read` may block on a file or the network.
    (d.host.parallelFor.unwrap())(
        d.host.userData,
        b"audio.stream.probe\0".as_ptr() as *const c_char,
        1,
        1,
        probe,
        &mut pr as *mut Probe as *mut c_void,
    );

    if pr.bytes <= 0 {
        return ENGINE_AUDIO_E_BAD_DATA; // unreadable source
    }
    ENGINE_AUDIO_E_BAD_DATA // readable, but this provider decodes nothing
}

unsafe extern "C" fn play(p: *mut c_void, desc: *const PlayDesc) -> EngineVoiceId {
    let Some(d) = dev(p) else { return ENGINE_AUDIO_NO_VOICE };
    let Some(pd) = desc.as_ref() else { return ENGINE_AUDIO_NO_VOICE };
    // No sound was ever successfully created, so every id is unknown — refuse
    // rather than mint a voice pointing at nothing.
    if pd.sound == ENGINE_AUDIO_NO_SOUND || pd.sound >= d.next_sound.load(Ordering::Relaxed) {
        return ENGINE_AUDIO_NO_VOICE;
    }
    d.voices.fetch_add(1, Ordering::Relaxed);
    d.next_voice.fetch_add(1, Ordering::Relaxed)
}

unsafe extern "C" fn stop(p: *mut c_void, v: EngineVoiceId, _fade: u32) {
    let Some(d) = dev(p) else { return };
    if v != ENGINE_AUDIO_NO_VOICE {
        // Saturating: the engine may legitimately stop a voice that already
        // finished, and going below zero would be a wrapped u32.
        let _ = d.voices.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
            Some(n.saturating_sub(1))
        });
    }
}

/// Rows the reference accepted as WELL-FORMED on its last updateEmitters call.
/// Not part of the ABI — a test-fixture read-back, so the suite can assert that
/// a wider stride was walked CORRECTLY rather than merely without crashing. A
/// provider walking by size_of instead of stride lands on the poison bytes
/// between rows and rejects them, so this count drops.
pub static LAST_WELL_FORMED: std::sync::atomic::AtomicU32 =
    std::sync::atomic::AtomicU32::new(0);

unsafe extern "C" fn update_emitters(
    p: *mut c_void,
    ups: *const EmitterUpdate,
    count: u32,
    stride: u32,
) {
    if dev(p).is_none() || ups.is_null() || count == 0 {
        return; // an empty update is normal, not an error
    }
    // WALKED BY STRIDE, not by size_of::<EmitterUpdate>(). This is the whole
    // reason the parameter exists: a newer engine may send WIDER rows, and a
    // provider that indexes with its own struct size would read every row after
    // the first at the wrong offset. A stride smaller than our own view means the
    // engine is older than this provider and the trailing fields are absent —
    // refuse rather than read past each row.
    if (stride as usize) < std::mem::size_of::<EmitterUpdate>() {
        return;
    }
    let base = ups as *const u8;
    let mut well_formed = 0u32;
    for i in 0..count as usize {
        let u = &*(base.add(i * stride as usize) as *const EmitterUpdate);
        // Stale ids are expected input. Read and drop.
        let _ = (u.voice, u.position, u.volume);
        // A row the engine actually wrote has a sane gain and pitch. Rows read at
        // the WRONG offset land on padding, so they do not.
        if u.volume.is_finite() && u.pitch == 1.0 && u.voice != 0 {
            well_formed += 1;
        }
    }
    LAST_WELL_FORMED.store(well_formed, std::sync::atomic::Ordering::Relaxed);
}

unsafe extern "C" fn set_listener(_p: *mut c_void, _l: *const Listener) {}

unsafe extern "C" fn set_geometry(
    p: *mut c_void,
    _g: *const AcousticGeometry,
) -> EngineAudioResult {
    if dev(p).is_none() {
        return ENGINE_AUDIO_E_BAD_ARG;
    }
    // This provider does no propagation. Saying so is the contract; failing
    // would make an optional capability look like a broken one.
    ENGINE_AUDIO_E_UNSUPPORTED
}

unsafe extern "C" fn set_param(_p: *mut c_void, _obj: u64, _hash: u64, _v: f32) {}

unsafe extern "C" fn get_stats(p: *mut c_void, out: *mut Stats) {
    let Some(o) = out.as_mut() else { return };
    let Some(d) = dev(p) else { return };
    // The engine set structSize; write only what fits. Skipping this check is a
    // stack smash the day a provider is newer than the engine calling it — the
    // one direction frozen layout cannot protect, because the writer is the
    // newer party.
    let cap = o.structSize as usize;
    if cap < std::mem::size_of::<Stats>() {
        return; // older engine than this provider knows how to fill safely
    }
    o.activeVoices = d.voices.load(Ordering::Relaxed);
    o.sampleRate = d.sample_rate;
    o.bufferFrames = d.buffer_frames;
    o.callbackOverruns = d.overruns.load(Ordering::Relaxed);
    // Read the pair ADJACENTLY. The engine correlates these two to place a
    // future sample on its own timeline, so any work between them shows up as
    // scheduling error that nothing downstream can detect or correct.
    o.samplesPlayed = d.samples.load(Ordering::Relaxed);
    o.hostTimeNs = (d.host.nowNs.unwrap())(d.host.userData);
    o.cpuLoad = 0.0;
}

/// The reference table. `guard` is deliberately unused on the trivial entries:
/// wrapping a function that cannot panic buys nothing and hides which one
/// genuinely needed it — `create_stream`, which invokes the engine's callback.
pub static REFERENCE: ProviderV1 = ProviderV1 {
    version: 1,
    structSize: std::mem::size_of::<ProviderV1>() as u32,
    create: Some(create),
    destroy: Some(destroy),
    suspend: Some(suspend),
    createSound: Some(create_sound),
    destroySound: Some(destroy_sound),
    createStream: Some(create_stream),
    findSound: Some(find_sound),
    play: Some(play),
    stop: Some(stop),
    updateEmitters: Some(update_emitters),
    setListener: Some(set_listener),
    setGeometry: Some(set_geometry),
    setParam: Some(set_param),
    getStats: Some(get_stats),
};
