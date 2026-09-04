// SPDX-License-Identifier: GPL-3.0-or-later
//
// A MediaPerch module in Rust, to find out whether the ABI is *pleasant* as
// well as possible.
//
// plan.md §2 chose C++ for v1 and then said the choice has to stay reversible,
// which it only does if somebody checks. This is the check: half a day, once,
// and then the question is settled either way. Three things it is looking for
// that the C probe cannot see:
//
//  - **`repr(C)` layout.** Every struct below is written out by hand from
//    `module.h` rather than generated, because a generator would agree with
//    itself. If a field is the wrong width or in the wrong place, the host
//    reads a vtable slot that is not a function and the process ends -- which
//    is exactly the failure this is here to find before it is somebody's music.
//  - **Calling convention.** `extern "C"` on every entry point, and the host
//    calls them through a vtable it built from a C header.
//  - **Whether a panic can be contained.** It cannot be allowed to unwind into
//    C++: that is undefined behaviour and, on Windows, a corrupted stack. So
//    every entry point wraps its body in `catch_unwind` and turns a panic into
//    `MP_ERR_INVALID`. The `panic` setting exercises that on purpose.
//
// **The verdict is in docs/plan.md.** This crate is not in CMake and not in CI:
// it is built by hand with `cargo build --release` and copied beside the
// binaries. Keeping it costs nothing and deleting it would delete the evidence.

#![allow(non_snake_case)]

use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

// ---------------------------------------------------------------------------
// The ABI, transcribed from include/mediaperch/module.h
// ---------------------------------------------------------------------------

pub type MpResult = u32;
const MP_OK: MpResult = 0;
const MP_END: MpResult = 1;
const MP_ERR_INVALID: MpResult = 2;
const MP_ERR_UNSUPPORTED: MpResult = 3;
const MP_ERR_FORMAT: MpResult = 4;

const MP_ABI_VERSION: u32 = 3;
const MP_KIND_DSP: u32 = 3;
const MP_SAMPLE_F64: u32 = 7;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct MpFormat {
    sample_rate: u32,
    channels: u32,
    channel_mask: u32,
    sample_type: u32,
    encoding: u32,
    valid_bits: u32,
    reserved: [u32; 2],
}

#[repr(C)]
pub struct MpDspVtbl {
    size: u32,
    reserved: u32,
    open: extern "C" fn(out: *mut *mut Dsp) -> MpResult,
    close: extern "C" fn(d: *mut Dsp),
    configure: extern "C" fn(
        d: *mut Dsp,
        in_format: *const MpFormat,
        max_frames: u32,
        out_format: *mut MpFormat,
        out_max_frames: *mut u32,
    ) -> MpResult,
    process: extern "C" fn(
        d: *mut Dsp,
        input: *const *const f64,
        in_frames: u32,
        output: *const *mut f64,
        out_capacity: u32,
        out_frames: *mut u32,
    ) -> MpResult,
    flush: extern "C" fn(
        d: *mut Dsp,
        output: *const *mut f64,
        out_capacity: u32,
        out_frames: *mut u32,
    ) -> MpResult,
    set: extern "C" fn(d: *mut Dsp, key: *const c_char, value: *const c_char) -> MpResult,
    describe: extern "C" fn(d: *mut Dsp, index: u32, out: *mut c_char, out_bytes: u32)
        -> MpResult,
    reset: extern "C" fn(d: *mut Dsp) -> MpResult,
}

#[repr(C)]
pub struct MpModuleDesc {
    size: u32,
    abi_version: u32,
    flags: u32,
    version: u32,
    kind: u32,
    priority: u32,
    id: *const c_char,
    name: *const c_char,
    init: extern "C" fn(host: *const c_void) -> MpResult,
    shutdown: extern "C" fn(),
    vtbl: *const c_void,
    // v2: capability declaration is data, not code. A DSP stage declares no
    // codecs, and transcribing the fields anyway is what makes this a layout
    // check rather than a hope.
    codecs: *const u32,
    codec_count: u32,
    reserved_desc: u32,
}

// The layout claims, from the other language. If any of these is wrong the
// build stops here instead of in the host's stack.
const _: () = assert!(std::mem::size_of::<MpFormat>() == 32);
const _: () = assert!(std::mem::size_of::<MpDspVtbl>() == 8 + 8 * std::mem::size_of::<usize>());
const _: () = assert!(
    std::mem::size_of::<MpModuleDesc>() == 32 + 6 * std::mem::size_of::<usize>()
);

// ---------------------------------------------------------------------------
// The module
// ---------------------------------------------------------------------------

pub struct Dsp {
    format: MpFormat,
    blocks: u32,
    frames: u64,
    complaints: i32,
    /// Set by `set panic 1`. The next `process` panics on purpose, so that
    /// "a panic is contained" is a thing this probe demonstrates rather than
    /// a thing its author believed.
    panic_next: bool,
}

/// Every entry point goes through this. A panic that crossed back into C++
/// would be undefined behaviour and, on Windows, a corrupted stack -- so the
/// boundary is where it stops, and it stops as an ordinary error code.
fn guard<F: FnOnce() -> MpResult>(body: F) -> MpResult {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(result) => result,
        Err(_) => MP_ERR_INVALID,
    }
}

extern "C" fn dsp_open(out: *mut *mut Dsp) -> MpResult {
    guard(|| {
        if out.is_null() {
            return MP_ERR_INVALID;
        }
        let dsp = Box::new(Dsp {
            format: MpFormat::default(),
            blocks: 0,
            frames: 0,
            complaints: 0,
            panic_next: false,
        });
        unsafe { *out = Box::into_raw(dsp) };
        MP_OK
    })
}

extern "C" fn dsp_close(d: *mut Dsp) {
    let _ = guard(|| {
        if !d.is_null() {
            drop(unsafe { Box::from_raw(d) });
        }
        MP_OK
    });
}

extern "C" fn dsp_configure(
    d: *mut Dsp,
    in_format: *const MpFormat,
    max_frames: u32,
    out_format: *mut MpFormat,
    out_max_frames: *mut u32,
) -> MpResult {
    guard(|| {
        if d.is_null() || in_format.is_null() || out_format.is_null() || out_max_frames.is_null()
        {
            return MP_ERR_INVALID;
        }
        let dsp = unsafe { &mut *d };
        let wanted = unsafe { *in_format };
        // The bus is deinterleaved f64. Anything else means the host changed
        // the contract without telling the modules.
        if wanted.sample_type != MP_SAMPLE_F64 {
            dsp.complaints += 1;
            return MP_ERR_FORMAT;
        }
        if wanted.channels == 0 || wanted.sample_rate == 0 || max_frames == 0 {
            dsp.complaints += 1;
            return MP_ERR_FORMAT;
        }
        dsp.format = wanted;
        unsafe {
            *out_format = wanted;
            *out_max_frames = max_frames;
        }
        MP_OK
    })
}

extern "C" fn dsp_process(
    d: *mut Dsp,
    input: *const *const f64,
    in_frames: u32,
    output: *const *mut f64,
    out_capacity: u32,
    out_frames: *mut u32,
) -> MpResult {
    guard(|| {
        if d.is_null() || out_frames.is_null() {
            return MP_ERR_INVALID;
        }
        let dsp = unsafe { &mut *d };
        unsafe { *out_frames = 0 };
        if dsp.panic_next {
            dsp.panic_next = false;
            // Deliberate. The host should see MP_ERR_INVALID and keep running.
            panic!("the Rust probe panicked on purpose");
        }
        if in_frames == 0 {
            return MP_OK;
        }
        if input.is_null() || output.is_null() {
            dsp.complaints += 1;
            return MP_ERR_INVALID;
        }
        if in_frames > out_capacity {
            dsp.complaints += 1;
            return MP_ERR_INVALID;
        }
        for c in 0..dsp.format.channels as usize {
            let src = unsafe { *input.add(c) };
            let dst = unsafe { *output.add(c) };
            if src.is_null() || dst.is_null() {
                dsp.complaints += 1;
                return MP_ERR_INVALID;
            }
            let src = unsafe { std::slice::from_raw_parts(src, in_frames as usize) };
            let dst = unsafe { std::slice::from_raw_parts_mut(dst, in_frames as usize) };
            for (out, sample) in dst.iter_mut().zip(src) {
                *out = sample * 0.5;
            }
        }
        dsp.blocks += 1;
        dsp.frames += u64::from(in_frames);
        unsafe { *out_frames = in_frames };
        MP_OK
    })
}

extern "C" fn dsp_flush(
    _d: *mut Dsp,
    _output: *const *mut f64,
    _out_capacity: u32,
    out_frames: *mut u32,
) -> MpResult {
    guard(|| {
        if out_frames.is_null() {
            return MP_ERR_INVALID;
        }
        unsafe { *out_frames = 0 };
        MP_OK
    })
}

/// A NUL-terminated C string as a `&str`, or nothing when it is not one.
unsafe fn borrow(text: *const c_char) -> Option<&'static str> {
    if text.is_null() {
        return None;
    }
    std::ffi::CStr::from_ptr(text).to_str().ok()
}

extern "C" fn dsp_set(d: *mut Dsp, key: *const c_char, value: *const c_char) -> MpResult {
    guard(|| {
        if d.is_null() {
            return MP_ERR_INVALID;
        }
        let dsp = unsafe { &mut *d };
        let (Some(key), Some(value)) = (unsafe { borrow(key) }, unsafe { borrow(value) }) else {
            return MP_ERR_INVALID;
        };
        match key {
            "reset_counts" => {
                dsp.blocks = 0;
                dsp.frames = 0;
                MP_OK
            }
            "panic" => {
                dsp.panic_next = value != "0";
                MP_OK
            }
            _ => MP_ERR_UNSUPPORTED,
        }
    })
}

/// Writes `text` into a caller's buffer, NUL-terminated, truncating rather than
/// overrunning. The C side would use `snprintf`; this is the same promise.
fn put(text: &str, out: *mut c_char, out_bytes: u32) {
    let room = out_bytes as usize;
    let bytes = text.as_bytes();
    let take = bytes.len().min(room.saturating_sub(1));
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), take);
        *out.add(take) = 0;
    }
}

extern "C" fn dsp_describe(d: *mut Dsp, index: u32, out: *mut c_char, out_bytes: u32) -> MpResult {
    guard(|| {
        if d.is_null() || out.is_null() || out_bytes < 64 {
            return MP_ERR_INVALID;
        }
        let dsp = unsafe { &*d };
        let line = match index {
            0 => "language\tRust\tthis module was compiled by rustc".to_string(),
            1 => format!("blocks\t{}\tprocess calls seen (read only)", dsp.blocks),
            2 => format!("frames\t{}\tframes seen (read only)", dsp.frames),
            3 => format!(
                "complaints\t{}\ttimes the host broke the header's word",
                dsp.complaints
            ),
            4 => "panic\t0\tset it to 1 and the next process panics, on purpose".to_string(),
            _ => return MP_END,
        };
        put(&line, out, out_bytes);
        MP_OK
    })
}

extern "C" fn dsp_reset(d: *mut Dsp) -> MpResult {
    guard(|| if d.is_null() { MP_ERR_INVALID } else { MP_OK })
}

extern "C" fn module_init(_host: *const c_void) -> MpResult {
    MP_OK
}

extern "C" fn module_shutdown() {}

static VTBL: MpDspVtbl = MpDspVtbl {
    size: std::mem::size_of::<MpDspVtbl>() as u32,
    reserved: 0,
    open: dsp_open,
    close: dsp_close,
    configure: dsp_configure,
    process: dsp_process,
    flush: dsp_flush,
    set: dsp_set,
    describe: dsp_describe,
    reset: dsp_reset,
};

static ID: &[u8] = b"dsp_probe_rust\0";
static NAME: &[u8] = b"The ABI, crossed from Rust (a probe: it halves what it is given)\0";

static DESC: MpModuleDesc = MpModuleDesc {
    size: std::mem::size_of::<MpModuleDesc>() as u32,
    abi_version: MP_ABI_VERSION,
    flags: 0,
    // MP_MAKE_VERSION(0, 1, 0): ten bits of minor, twelve of patch.
    version: (0 << 22) | (1 << 12),
    kind: MP_KIND_DSP,
    // Lowest there is. It is a probe, not a filter anybody wants chosen.
    priority: 0,
    id: ID.as_ptr().cast(),
    name: NAME.as_ptr().cast(),
    init: module_init,
    shutdown: module_shutdown,
    vtbl: &VTBL as *const MpDspVtbl as *const c_void,
    codecs: std::ptr::null(),
    codec_count: 0,
    reserved_desc: 0,
};

// `MpModuleDesc` holds raw pointers, which Rust will not call `Sync` on its
// own. They point at other statics in this same object and are never written.
unsafe impl Sync for MpModuleDesc {}
unsafe impl Sync for MpDspVtbl {}

/// The one exported symbol, by that exact name. No mangling, no decoration.
#[no_mangle]
pub extern "C" fn mp_module_entry(host_abi_version: u32) -> *const MpModuleDesc {
    if host_abi_version != MP_ABI_VERSION {
        return std::ptr::null();
    }
    &DESC
}
