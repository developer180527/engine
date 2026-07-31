//! engine_net — the engine's networking module, written in Rust.
//!
//! # Why Rust here
//!
//! Networking parses untrusted bytes off the wire, carries a stateful
//! connection machine, and is brand-new code. That combination is exactly
//! where memory safety pays: a malformed packet must not become a heap
//! overflow. It is deliberately NOT a blanket migration — ozz-animation and
//! Jolt keep the hot numeric loops, because they touch no untrusted input and
//! rewriting them would buy nothing.
//!
//! # What is (and is not) here yet
//!
//! This is the TOOLCHAIN SKELETON. It establishes the C ABI, the build wiring
//! and the safety rules, and stops there — the transport design (raw UDP with
//! a reliability layer vs QUIC, what the engine owns vs what kits own) is an
//! open decision, and writing a protocol before that choice is made would
//! quietly make it. Every entry point below exists to exercise a *shape* the
//! real transport needs, not to be that transport.
//!
//! # The five boundary rules (mirrored in include/engine_net.h)
//!
//! 1. No allocation crosses: callers own their buffers, we write into them.
//! 2. No panic crosses: the crate is built `panic = "abort"`.
//! 3. Every call returns a status code; nothing throws.
//! 4. Init is version-checked against the header.
//! 5. Null pointers and zero lengths are validated, never assumed.

#![deny(unsafe_op_in_unsafe_fn)]
#![deny(improper_ctypes_definitions)]

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::atomic::{AtomicBool, AtomicPtr, Ordering};
use std::sync::Mutex;

pub const ENGINE_NET_ABI_VERSION: u32 = 1;

// Status codes — must match EngineNetStatus in engine_net.h.
pub const ENGINE_NET_OK: i32 = 0;
pub const ENGINE_NET_ERR_ABI: i32 = -1;
pub const ENGINE_NET_ERR_NULL_ARG: i32 = -2;
pub const ENGINE_NET_ERR_NOT_INIT: i32 = -3;
pub const ENGINE_NET_ERR_ALREADY: i32 = -4;

static INITIALISED: AtomicBool = AtomicBool::new(false);

/// C log sink. `Option<extern "C" fn>` is FFI-safe and null-representable, so
/// a null function pointer from C arrives as `None` instead of a dangling call.
pub type EngineNetLogFn = Option<extern "C" fn(user: *mut c_void, msg: *const c_char)>;

// The sink and its user pointer must move together, or a detach racing an emit
// could pair a new callback with a stale user pointer. One mutex over both.
static LOG_SINK: Mutex<Option<(EngineNetLogFn, AtomicPtrWrapper)>> = Mutex::new(None);

/// `*mut c_void` is not `Send`, but the host explicitly hands us this pointer
/// to give back verbatim; we never dereference it. Wrapping it in an atomic
/// makes that contract explicit rather than sprinkling `unsafe impl Send`.
struct AtomicPtrWrapper(AtomicPtr<c_void>);
unsafe impl Send for AtomicPtrWrapper {}

#[no_mangle]
pub extern "C" fn engine_net_abi_version() -> u32 {
    ENGINE_NET_ABI_VERSION
}

#[no_mangle]
pub extern "C" fn engine_net_status_str(status: i32) -> *const c_char {
    // Static NUL-terminated bytes: no allocation, so this is callable from any
    // error path including one that is already failing to allocate.
    let s: &'static [u8] = match status {
        ENGINE_NET_OK => b"ok\0",
        ENGINE_NET_ERR_ABI => b"ABI version mismatch between engine_net.h and the linked library\0",
        ENGINE_NET_ERR_NULL_ARG => b"required pointer argument was null\0",
        ENGINE_NET_ERR_NOT_INIT => b"engine_net_init() has not been called\0",
        ENGINE_NET_ERR_ALREADY => b"engine_net_init() called twice\0",
        _ => b"unknown engine_net status\0",
    };
    s.as_ptr() as *const c_char
}

#[no_mangle]
pub extern "C" fn engine_net_init(abi_version: u32) -> i32 {
    if abi_version != ENGINE_NET_ABI_VERSION {
        // Caught here rather than at link time: a stale static library is the
        // realistic failure (someone rebuilt C++ but not Rust), and it must be
        // loud at boot instead of a subtly wrong struct layout later.
        return ENGINE_NET_ERR_ABI;
    }
    if INITIALISED.swap(true, Ordering::AcqRel) {
        return ENGINE_NET_ERR_ALREADY;
    }
    ENGINE_NET_OK
}

#[no_mangle]
pub extern "C" fn engine_net_shutdown() {
    INITIALISED.store(false, Ordering::Release);
    if let Ok(mut guard) = LOG_SINK.lock() {
        *guard = None;
    }
}

#[no_mangle]
pub extern "C" fn engine_net_set_log(cb: EngineNetLogFn, user: *mut c_void) {
    if let Ok(mut guard) = LOG_SINK.lock() {
        *guard = cb.map(|f| (Some(f), AtomicPtrWrapper(AtomicPtr::new(user))));
    }
}

/// Emit through the installed sink. Rust → C, with `msg` borrowed for the
/// duration of the call only — the same lifetime discipline the eventual
/// packet-received callback needs.
#[no_mangle]
pub extern "C" fn engine_net_log_test(msg: *const c_char) -> i32 {
    if !INITIALISED.load(Ordering::Acquire) {
        return ENGINE_NET_ERR_NOT_INIT;
    }
    if msg.is_null() {
        return ENGINE_NET_ERR_NULL_ARG;
    }
    // SAFETY: non-null checked above; the C contract states `msg` is a valid
    // NUL-terminated string for the duration of this call.
    let text = unsafe { CStr::from_ptr(msg) };

    // Prefix through an owned CString to prove a Rust-side allocation can be
    // handed out as a borrowed pointer and dropped here — memory the host
    // never owns and never frees (boundary rule 1).
    let decorated = match CString::new(format!("[engine_net] {}", text.to_string_lossy())) {
        Ok(s) => s,
        Err(_) => return ENGINE_NET_ERR_NULL_ARG, // interior NUL
    };

    let sink = match LOG_SINK.lock() {
        Ok(g) => g.as_ref().map(|(f, u)| (*f, u.0.load(Ordering::Acquire))),
        Err(_) => None,
    };
    if let Some((Some(f), user)) = sink {
        f(user, decorated.as_ptr());
    }
    ENGINE_NET_OK
}

/// FNV-1a over the caller's bytes. Not networking — the smallest honest test
/// that borrowed buffers cross correctly in the direction packets will.
#[no_mangle]
pub extern "C" fn engine_net_checksum(data: *const u8, len: usize, out: *mut u64) -> i32 {
    if out.is_null() {
        return ENGINE_NET_ERR_NULL_ARG;
    }
    // A null pointer with a non-zero length is a caller bug, not an empty
    // slice: `from_raw_parts` on null is UB even for len 0, so both are
    // rejected before any unsafe code runs.
    if data.is_null() && len != 0 {
        return ENGINE_NET_ERR_NULL_ARG;
    }

    let bytes: &[u8] = if len == 0 {
        &[]
    } else {
        // SAFETY: non-null checked; the C contract states `data` points to at
        // least `len` readable bytes, valid for this call.
        unsafe { std::slice::from_raw_parts(data, len) }
    };

    let mut hash: u64 = 0xcbf2_9ce4_8422_2325;
    for b in bytes {
        hash ^= *b as u64;
        hash = hash.wrapping_mul(0x0000_0100_0000_01b3);
    }
    // SAFETY: non-null checked above.
    unsafe { *out = hash };
    ENGINE_NET_OK
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn checksum_matches_known_fnv1a() {
        // "abc" under FNV-1a/64 — a fixed vector, so a refactor that changes
        // the hash is caught here and not by a mysterious protocol mismatch.
        let data = b"abc";
        let mut out: u64 = 0;
        assert_eq!(
            engine_net_checksum(data.as_ptr(), data.len(), &mut out),
            ENGINE_NET_OK
        );
        assert_eq!(out, 0xe71fa2190541574b);
    }

    #[test]
    fn rejects_null_out_and_null_with_len() {
        let mut out: u64 = 0;
        assert_eq!(
            engine_net_checksum(b"x".as_ptr(), 1, std::ptr::null_mut()),
            ENGINE_NET_ERR_NULL_ARG
        );
        assert_eq!(
            engine_net_checksum(std::ptr::null(), 4, &mut out),
            ENGINE_NET_ERR_NULL_ARG
        );
    }

    #[test]
    fn empty_slice_is_the_fnv_offset_basis() {
        let mut out: u64 = 0;
        assert_eq!(
            engine_net_checksum(std::ptr::null(), 0, &mut out),
            ENGINE_NET_OK
        );
        assert_eq!(out, 0xcbf29ce484222325);
    }

    #[test]
    fn abi_mismatch_is_rejected() {
        assert_eq!(engine_net_init(ENGINE_NET_ABI_VERSION + 1), ENGINE_NET_ERR_ABI);
    }
}
