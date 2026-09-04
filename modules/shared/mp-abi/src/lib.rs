// SPDX-License-Identifier: GPL-3.0-or-later
//
// The module ABI, crossed from Rust -- and every `unsafe` a module needs, in
// one place, so that the modules themselves need none.
//
// `include/mediaperch/module.h` is a C header: vtables of function pointers
// that receive raw pointers and lengths. There is no way to turn
// `(*const u8, usize)` into a `&[u8]` without `unsafe`, no way to call the
// host's `log` through a raw function pointer without it, and no way to hand a
// `Box` across as an opaque handle and get it back without it. That is the
// whole list, and it is short enough to keep in one file and read in one
// sitting.
//
// **So the boundary is here and the logic is elsewhere.** A codec module
// implements the `Codec` trait and a container module the `Demux` trait --
// slices in, slices out, `Result` for errors -- and this crate supplies the
// `extern "C"` trampolines that the host calls, each of which does exactly
// three things: check the pointers, make the slices, and call the trait. The
// module crate carries `#![forbid(unsafe_code)]` on the code that parses the
// file, which the compiler enforces rather than a reviewer.
//
// **Every trampoline is wrapped in `catch_unwind`.** A panic that crossed into
// C++ would be undefined behaviour and, on Windows, a corrupted stack -- and a
// panic is also exactly what a Rust bounds check turns a crafted packet into.
// So the boundary is where it stops: logged, and returned as `MP_ERR_INTERNAL`,
// which the host treats as a stage that failed. The ABI probe in
// `abi/probe_rust` measured this before any of it was written; abi/README.md
// has the run.
//
// The structs below are transcribed from the header by hand and checked by
// size, for the reason the probe gives: a generator would agree with itself.

#![allow(clippy::missing_safety_doc)]

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicPtr, Ordering};

// ---------------------------------------------------------------------------
// Numbers from the header
// ---------------------------------------------------------------------------

/// `MpResult`, the ABI's return code.
pub type MpResult = u32;

pub mod result {
    use super::MpResult;
    pub const OK: MpResult = 0;
    pub const END: MpResult = 1;
    pub const ERR_INVALID: MpResult = 2;
    pub const ERR_UNSUPPORTED: MpResult = 3;
    pub const ERR_FORMAT: MpResult = 4;
    pub const ERR_IO: MpResult = 5;
    pub const ERR_NO_MEMORY: MpResult = 9;
    pub const ERR_INTERNAL: MpResult = 10;
}

pub const ABI_VERSION: u32 = 2;

pub mod kind {
    pub const DSP: u32 = 3;
    pub const DEMUX: u32 = 6;
    pub const CODEC: u32 = 7;
}

/// `MpCodec` values. Only the ones a Rust module names so far.
pub mod codec {
    pub const UNKNOWN: u32 = 0;
    /// Not a compression: the file's bits are the waveform. `codec_dsd`
    /// frames them as DoP, which is what a Windows endpoint can carry.
    pub const DSD: u32 = 2;
    pub const ALAC: u32 = 17;
    pub const AAC_LC: u32 = 35;
}

/// `MpSampleType`.
pub mod sample {
    pub const NONE: u32 = 0;
    pub const S16: u32 = 1;
    pub const S24_PACKED: u32 = 2;
    pub const S24_IN_32: u32 = 3;
    pub const S32: u32 = 4;
    pub const F32: u32 = 5;
}

/// `MpEncoding`.
pub mod encoding {
    pub const PCM: u32 = 0;
    /// DSD carried in 24-bit PCM frames under a 0x05/0xFA marker.
    pub const DOP: u32 = 1;
}

/// `MpStreamKind`.
pub mod stream_kind {
    pub const AUDIO: u32 = 1;
}

/// `MpStreamInfo::flags`.
pub mod stream_flag {
    pub const SELF_DECODES: u32 = 1 << 0;
    pub const DEFAULT: u32 = 1 << 1;
}

/// `MpPacket::flags`.
pub mod packet_flag {
    pub const SYNC: u32 = 1 << 0;
    pub const TIMED: u32 = 1 << 1;
}

/// `MpLogLevel`.
pub mod level {
    pub const ERROR: u32 = 0;
    pub const WARN: u32 = 1;
    pub const INFO: u32 = 2;
    pub const DEBUG: u32 = 3;
}

/// `MP_MAKE_VERSION`: ten bits of minor, twelve of patch.
pub const fn make_version(major: u32, minor: u32, patch: u32) -> u32 {
    (major << 22) | (minor << 12) | patch
}

// ---------------------------------------------------------------------------
// Structs from the header
// ---------------------------------------------------------------------------

/// `MpFormat`. Plain data, so its fields are public and a module fills it in
/// directly.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Format {
    pub sample_rate: u32,
    pub channels: u32,
    pub channel_mask: u32,
    pub sample_type: u32,
    pub encoding: u32,
    pub valid_bits: u32,
    pub reserved: [u32; 2],
}

/// `MpStreamInfo`: one stream of a container, as far as the container states
/// it. Plain data like `Format`; a demuxer fills in what it knows and leaves
/// the rest zero, which the header defines as "the container did not say".
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StreamInfo {
    /// Set by the caller and preserved by the trampoline; a module need not
    /// touch it.
    pub size: u32,
    pub index: u32,
    pub kind: u32,
    pub codec: u32,
    pub flags: u32,
    pub config_bytes: u32,
    pub format: Format,
    pub total_frames: u64,
    pub skip_frames: u64,
    pub play_frames: u64,
    pub duration_ms: u64,
    pub trim_frames: u64,
    pub reserved: [u32; 2],
}

/// `MpPacket`, as the host hands it to `read_packet`. Not something a module
/// fills in itself: `Demux::read_packet` returns a `Next` and the trampoline
/// writes this.
#[repr(C)]
pub struct Packet {
    size: u32,
    flags: u32,
    bytes: u32,
    reserved: u32,
    frame: u64,
}

/// `MpHost`: what the host lends a module. `Option` around each function
/// pointer is the FFI spelling of "may be NULL".
#[repr(C)]
pub struct Host {
    size: u32,
    reserved: u32,
    ctx: *mut c_void,
    log: Option<extern "C" fn(ctx: *mut c_void, level: u32, msg: *const c_char)>,
    alloc: Option<extern "C" fn(ctx: *mut c_void, bytes: usize) -> *mut c_void>,
    release: Option<extern "C" fn(ctx: *mut c_void, p: *mut c_void)>,
}

/// `MpCodecVtbl`. The instance pointer is `*mut c_void` because that is all
/// the host ever sees of it; the trampolines below know the real type.
#[repr(C)]
pub struct CodecVtbl {
    size: u32,
    reserved: u32,
    probe: extern "C" fn(
        codec: u32,
        config: *const u8,
        config_bytes: u32,
        out_score: *mut u32,
    ) -> MpResult,
    open: extern "C" fn(
        codec: u32,
        config: *const u8,
        config_bytes: u32,
        out: *mut *mut c_void,
    ) -> MpResult,
    get_format: extern "C" fn(c: *mut c_void, out: *mut Format) -> MpResult,
    decode: extern "C" fn(
        c: *mut c_void,
        packet: *const c_void,
        packet_bytes: usize,
        dst: *mut c_void,
        dst_bytes: usize,
        out_bytes: *mut usize,
    ) -> MpResult,
    flush: extern "C" fn(
        c: *mut c_void,
        dst: *mut c_void,
        dst_bytes: usize,
        out_bytes: *mut usize,
    ) -> MpResult,
    reset: extern "C" fn(c: *mut c_void) -> MpResult,
    close: extern "C" fn(c: *mut c_void),
}

/// `MpDemuxVtbl`. `read_frames` is `Option` because a demuxer that splits
/// properly leaves it NULL, which every Rust one does.
#[repr(C)]
pub struct DemuxVtbl {
    size: u32,
    reserved: u32,
    probe: extern "C" fn(
        path: *const c_char,
        head: *const u8,
        head_bytes: usize,
        out_score: *mut u32,
    ) -> MpResult,
    open: extern "C" fn(path: *const c_char, out: *mut *mut c_void) -> MpResult,
    stream_count: extern "C" fn(d: *mut c_void, out_count: *mut u32) -> MpResult,
    stream_info: extern "C" fn(d: *mut c_void, index: u32, out: *mut StreamInfo) -> MpResult,
    stream_config: extern "C" fn(
        d: *mut c_void,
        index: u32,
        out: *mut u8,
        out_bytes: u32,
        out_needed: *mut u32,
    ) -> MpResult,
    select: extern "C" fn(d: *mut c_void, index: u32) -> MpResult,
    read_packet: extern "C" fn(
        d: *mut c_void,
        dst: *mut c_void,
        dst_bytes: usize,
        out: *mut Packet,
    ) -> MpResult,
    seek: extern "C" fn(d: *mut c_void, frame: u64) -> MpResult,
    read_frames: Option<
        extern "C" fn(
            d: *mut c_void,
            dst: *mut c_void,
            dst_bytes: usize,
            out_bytes: *mut usize,
        ) -> MpResult,
    >,
    close: extern "C" fn(d: *mut c_void),
}

/// `MpModuleDesc`, the one thing `mp_module_entry` returns.
#[repr(C)]
pub struct ModuleDesc {
    size: u32,
    abi_version: u32,
    flags: u32,
    version: u32,
    kind: u32,
    priority: u32,
    id: *const c_char,
    name: *const c_char,
    init: extern "C" fn(host: *const Host) -> MpResult,
    shutdown: extern "C" fn(),
    vtbl: *const c_void,
    codecs: *const u32,
    codec_count: u32,
    reserved_desc: u32,
}

// The layout claims. If any is wrong the build stops here rather than in the
// host's stack.
const _: () = assert!(std::mem::size_of::<Format>() == 32);
const _: () = assert!(std::mem::size_of::<StreamInfo>() == 104);
const _: () = assert!(std::mem::size_of::<Packet>() == 24);
const _: () = assert!(std::mem::size_of::<Host>() == 8 + 4 * std::mem::size_of::<usize>());
const _: () = assert!(std::mem::size_of::<CodecVtbl>() == 8 + 7 * std::mem::size_of::<usize>());
const _: () = assert!(std::mem::size_of::<DemuxVtbl>() == 8 + 10 * std::mem::size_of::<usize>());
const _: () = assert!(std::mem::size_of::<ModuleDesc>() == 32 + 6 * std::mem::size_of::<usize>());

// These hold raw pointers into other statics of the same object, never written
// after the linker laid them out. That is what `Sync` is being promised.
#[allow(unsafe_code)]
unsafe impl Sync for CodecVtbl {}
#[allow(unsafe_code)]
unsafe impl Sync for DemuxVtbl {}
#[allow(unsafe_code)]
unsafe impl Sync for ModuleDesc {}

// ---------------------------------------------------------------------------
// What a module implements
// ---------------------------------------------------------------------------

/// Why a call failed, in the ABI's own terms.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// A caller passed nonsense.
    Invalid,
    /// This module does not do that.
    Unsupported,
    /// The data is not what it claims to be.
    Format,
    Io,
    /// The caller's buffer is too small.
    NoMemory,
    Internal,
}

impl Error {
    pub const fn code(self) -> MpResult {
        match self {
            Error::Invalid => result::ERR_INVALID,
            Error::Unsupported => result::ERR_UNSUPPORTED,
            Error::Format => result::ERR_FORMAT,
            Error::Io => result::ERR_IO,
            Error::NoMemory => result::ERR_NO_MEMORY,
            Error::Internal => result::ERR_INTERNAL,
        }
    }
}

/// A codec, as `MpCodecVtbl` sees it and with the pointers taken out.
///
/// `codec` is the `MpCodec` id the container reported and `config` is its
/// configuration blob, verbatim. Everything the header says about each entry
/// point holds here: `decode` never converts, a packet that decodes to nothing
/// returns `Ok(0)`, and `reset` forgets everything so the host can pre-roll.
pub trait Codec: Sized + 'static {
    /// How well this module decodes `codec` with this configuration: 0 for
    /// "not at all", up to 100. A question about data, not about a file.
    fn probe(codec: u32, config: &[u8]) -> u32;

    fn open(codec: u32, config: &[u8]) -> Result<Self, Error>;

    /// What this codec produces. `Err(Error::Format)` while it is not yet
    /// known, which the header allows for a codec whose configuration does
    /// not state it -- AAC with channel configuration 0 in a raw stream, where
    /// the layout arrives with the first frame.
    fn format(&self) -> Result<Format, Error>;

    /// One packet in, PCM out into `dst`. Returns the bytes written, or
    /// `Error::NoMemory` when `dst` is too small for what the packet holds.
    fn decode(&mut self, packet: &[u8], dst: &mut [u8]) -> Result<usize, Error>;

    /// What is still inside after the last packet, if anything.
    fn flush(&mut self, dst: &mut [u8]) -> Result<usize, Error>;

    fn reset(&mut self) -> Result<(), Error>;
}

/// What `Demux::read_packet` hands back, which the trampoline turns into the
/// header's three-way contract: a packet, MP_END with no bytes, or
/// MP_ERR_NO_MEMORY with the size the packet needs and nothing consumed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Next {
    /// `bytes` were written into `dst`; `frame` is the packet's position in
    /// the stream's own frames, or 0 where the container does not say;
    /// `flags` are `packet_flag` bits.
    Packet {
        bytes: usize,
        frame: u64,
        flags: u32,
    },
    /// The end of the stream. Not an error.
    End,
    /// `dst` was too small; this many bytes are needed, and nothing was
    /// consumed.
    TooSmall(usize),
}

/// A container, as `MpDemuxVtbl` sees it and with the pointers taken out.
///
/// `path` is UTF-8, as the header promises, and a Rust module opens it with
/// `std::fs::File`, which spells it in whatever the OS wants. The one thing
/// the header says that this trait cannot: `read_frames` is for a stream that
/// decodes itself, and no Rust demuxer has one, so the vtable leaves it NULL.
pub trait Demux: Sized + 'static {
    /// Whether `head`, the first bytes of the file at `path`, is this
    /// container: 0 for no, up to 100. A question about the container only.
    fn probe(path: &str, head: &[u8]) -> u32;

    fn open(path: &str) -> Result<Self, Error>;

    fn stream_count(&self) -> u32;

    /// `index`'s description. `size` and `index` are filled in by the
    /// trampoline, so a module leaves them at their defaults.
    fn stream_info(&self, index: u32) -> Result<StreamInfo, Error>;

    /// The codec's configuration blob for stream `index`, verbatim.
    fn stream_config(&self, index: u32) -> Result<&[u8], Error>;

    fn select(&mut self, index: u32) -> Result<(), Error>;

    fn read_packet(&mut self, dst: &mut [u8]) -> Result<Next, Error>;

    /// To the packet containing `frame`, or the nearest point before it from
    /// which a reset codec decodes correctly.
    fn seek(&mut self, frame: u64) -> Result<(), Error>;
}

// ---------------------------------------------------------------------------
// The boundary
// ---------------------------------------------------------------------------

/// Every entry point goes through this. A panic stops here, as an error code
/// and a log line, rather than unwinding into C++.
fn guard<F: FnOnce() -> MpResult>(body: F) -> MpResult {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(code) => code,
        Err(payload) => {
            let what = payload
                .downcast_ref::<&str>()
                .map(|s| (*s).to_string())
                .or_else(|| payload.downcast_ref::<String>().cloned())
                .unwrap_or_else(|| "panic with a payload that is not a string".to_string());
            log(
                level::ERROR,
                &format!("panic contained at the module boundary: {what}"),
            );
            result::ERR_INTERNAL
        }
    }
}

/// `(ptr, len)` as a slice. Null is the empty slice, which is what a caller
/// passing NULL and 0 means.
#[allow(unsafe_code)]
unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if ptr.is_null() || len == 0 {
        &[]
    } else {
        std::slice::from_raw_parts(ptr, len)
    }
}

#[allow(unsafe_code)]
unsafe fn bytes_mut<'a>(ptr: *mut u8, len: usize) -> &'a mut [u8] {
    if ptr.is_null() || len == 0 {
        &mut []
    } else {
        std::slice::from_raw_parts_mut(ptr, len)
    }
}

/// A NUL-terminated UTF-8 path as a `String`. Null is the empty path, and a
/// byte that is not UTF-8 -- which the header says cannot happen -- is
/// replaced rather than refused, so the module sees a path that fails to open
/// instead of a call that fails to happen.
#[allow(unsafe_code)]
unsafe fn path_string(path: *const c_char) -> String {
    if path.is_null() {
        String::new()
    } else {
        CStr::from_ptr(path).to_string_lossy().into_owned()
    }
}

// ----------------------------------------------------------------- codecs

#[allow(unsafe_code)]
extern "C" fn probe<C: Codec>(
    codec: u32,
    config: *const u8,
    config_bytes: u32,
    out_score: *mut u32,
) -> MpResult {
    guard(|| {
        if out_score.is_null() {
            return result::ERR_INVALID;
        }
        let config = unsafe { bytes(config, config_bytes as usize) };
        let score = C::probe(codec, config);
        unsafe { *out_score = score };
        result::OK
    })
}

#[allow(unsafe_code)]
extern "C" fn open<C: Codec>(
    codec: u32,
    config: *const u8,
    config_bytes: u32,
    out: *mut *mut c_void,
) -> MpResult {
    guard(|| {
        if out.is_null() {
            return result::ERR_INVALID;
        }
        unsafe { *out = std::ptr::null_mut() };
        let config = unsafe { bytes(config, config_bytes as usize) };
        match C::open(codec, config) {
            Ok(instance) => {
                unsafe { *out = Box::into_raw(Box::new(instance)).cast::<c_void>() };
                result::OK
            }
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn get_format<C: Codec>(c: *mut c_void, out: *mut Format) -> MpResult {
    guard(|| {
        if c.is_null() || out.is_null() {
            return result::ERR_INVALID;
        }
        let codec = unsafe { &*c.cast::<C>() };
        match codec.format() {
            Ok(format) => {
                unsafe { *out = format };
                result::OK
            }
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn decode<C: Codec>(
    c: *mut c_void,
    packet: *const c_void,
    packet_bytes: usize,
    dst: *mut c_void,
    dst_bytes: usize,
    out_bytes: *mut usize,
) -> MpResult {
    guard(|| {
        if c.is_null() || out_bytes.is_null() {
            return result::ERR_INVALID;
        }
        unsafe { *out_bytes = 0 };
        let codec = unsafe { &mut *c.cast::<C>() };
        let packet = unsafe { bytes(packet.cast::<u8>(), packet_bytes) };
        let dst = unsafe { bytes_mut(dst.cast::<u8>(), dst_bytes) };
        match codec.decode(packet, dst) {
            Ok(written) => {
                unsafe { *out_bytes = written };
                result::OK
            }
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn flush<C: Codec>(
    c: *mut c_void,
    dst: *mut c_void,
    dst_bytes: usize,
    out_bytes: *mut usize,
) -> MpResult {
    guard(|| {
        if c.is_null() || out_bytes.is_null() {
            return result::ERR_INVALID;
        }
        unsafe { *out_bytes = 0 };
        let codec = unsafe { &mut *c.cast::<C>() };
        let dst = unsafe { bytes_mut(dst.cast::<u8>(), dst_bytes) };
        match codec.flush(dst) {
            Ok(written) => {
                unsafe { *out_bytes = written };
                result::OK
            }
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn reset<C: Codec>(c: *mut c_void) -> MpResult {
    guard(|| {
        if c.is_null() {
            return result::ERR_INVALID;
        }
        let codec = unsafe { &mut *c.cast::<C>() };
        match codec.reset() {
            Ok(()) => result::OK,
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn close<C: Codec>(c: *mut c_void) {
    let _ = guard(|| {
        if !c.is_null() {
            drop(unsafe { Box::from_raw(c.cast::<C>()) });
        }
        result::OK
    });
}

impl CodecVtbl {
    /// The vtable for `C`, as a constant, so it can be a `static`.
    pub const fn new<C: Codec>() -> Self {
        CodecVtbl {
            size: std::mem::size_of::<CodecVtbl>() as u32,
            reserved: 0,
            probe: probe::<C>,
            open: open::<C>,
            get_format: get_format::<C>,
            decode: decode::<C>,
            flush: flush::<C>,
            reset: reset::<C>,
            close: close::<C>,
        }
    }
}

// -------------------------------------------------------------- containers

#[allow(unsafe_code)]
extern "C" fn demux_probe<D: Demux>(
    path: *const c_char,
    head: *const u8,
    head_bytes: usize,
    out_score: *mut u32,
) -> MpResult {
    guard(|| {
        if out_score.is_null() {
            return result::ERR_INVALID;
        }
        unsafe { *out_score = 0 };
        let path = unsafe { path_string(path) };
        let head = unsafe { bytes(head, head_bytes) };
        let score = D::probe(&path, head);
        unsafe { *out_score = score };
        result::OK
    })
}

#[allow(unsafe_code)]
extern "C" fn demux_open<D: Demux>(path: *const c_char, out: *mut *mut c_void) -> MpResult {
    guard(|| {
        if path.is_null() || out.is_null() {
            return result::ERR_INVALID;
        }
        unsafe { *out = std::ptr::null_mut() };
        let path = unsafe { path_string(path) };
        match D::open(&path) {
            Ok(instance) => {
                unsafe { *out = Box::into_raw(Box::new(instance)).cast::<c_void>() };
                result::OK
            }
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn demux_stream_count<D: Demux>(d: *mut c_void, out_count: *mut u32) -> MpResult {
    guard(|| {
        if d.is_null() || out_count.is_null() {
            return result::ERR_INVALID;
        }
        let demux = unsafe { &*d.cast::<D>() };
        unsafe { *out_count = demux.stream_count() };
        result::OK
    })
}

#[allow(unsafe_code)]
extern "C" fn demux_stream_info<D: Demux>(
    d: *mut c_void,
    index: u32,
    out: *mut StreamInfo,
) -> MpResult {
    guard(|| {
        if d.is_null() || out.is_null() {
            return result::ERR_INVALID;
        }
        // The caller states how much of the struct it has. A host speaking
        // this ABI version has all of it; one with less is not written past.
        let size = unsafe { (*out).size };
        if (size as usize) < std::mem::size_of::<StreamInfo>() {
            return result::ERR_INVALID;
        }
        let demux = unsafe { &*d.cast::<D>() };
        match demux.stream_info(index) {
            Ok(mut info) => {
                info.size = size;
                info.index = index;
                unsafe { *out = info };
                result::OK
            }
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn demux_stream_config<D: Demux>(
    d: *mut c_void,
    index: u32,
    out: *mut u8,
    out_bytes: u32,
    out_needed: *mut u32,
) -> MpResult {
    guard(|| {
        if d.is_null() || out_needed.is_null() {
            return result::ERR_INVALID;
        }
        unsafe { *out_needed = 0 };
        let demux = unsafe { &*d.cast::<D>() };
        let config = match demux.stream_config(index) {
            Ok(config) => config,
            Err(e) => return e.code(),
        };
        unsafe { *out_needed = config.len() as u32 };
        if out.is_null() {
            return result::OK; // asking how much, which the header allows
        }
        if (out_bytes as usize) < config.len() {
            return result::ERR_NO_MEMORY;
        }
        let dst = unsafe { bytes_mut(out, out_bytes as usize) };
        dst[..config.len()].copy_from_slice(config);
        result::OK
    })
}

#[allow(unsafe_code)]
extern "C" fn demux_select<D: Demux>(d: *mut c_void, index: u32) -> MpResult {
    guard(|| {
        if d.is_null() {
            return result::ERR_INVALID;
        }
        let demux = unsafe { &mut *d.cast::<D>() };
        match demux.select(index) {
            Ok(()) => result::OK,
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn demux_read_packet<D: Demux>(
    d: *mut c_void,
    dst: *mut c_void,
    dst_bytes: usize,
    out: *mut Packet,
) -> MpResult {
    guard(|| {
        if d.is_null() || out.is_null() {
            return result::ERR_INVALID;
        }
        let size = unsafe { (*out).size };
        unsafe {
            *out = Packet {
                size,
                flags: 0,
                bytes: 0,
                reserved: 0,
                frame: 0,
            };
        }
        let demux = unsafe { &mut *d.cast::<D>() };
        let dst = unsafe { bytes_mut(dst.cast::<u8>(), dst_bytes) };
        match demux.read_packet(dst) {
            Ok(Next::Packet {
                bytes,
                frame,
                flags,
            }) => {
                unsafe {
                    (*out).bytes = bytes as u32;
                    (*out).frame = frame;
                    (*out).flags = flags;
                }
                result::OK
            }
            Ok(Next::End) => result::END,
            Ok(Next::TooSmall(needed)) => {
                unsafe { (*out).bytes = needed as u32 };
                result::ERR_NO_MEMORY
            }
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn demux_seek<D: Demux>(d: *mut c_void, frame: u64) -> MpResult {
    guard(|| {
        if d.is_null() {
            return result::ERR_INVALID;
        }
        let demux = unsafe { &mut *d.cast::<D>() };
        match demux.seek(frame) {
            Ok(()) => result::OK,
            Err(e) => e.code(),
        }
    })
}

#[allow(unsafe_code)]
extern "C" fn demux_close<D: Demux>(d: *mut c_void) {
    let _ = guard(|| {
        if !d.is_null() {
            drop(unsafe { Box::from_raw(d.cast::<D>()) });
        }
        result::OK
    });
}

impl DemuxVtbl {
    /// The vtable for `D`, as a constant, so it can be a `static`.
    pub const fn new<D: Demux>() -> Self {
        DemuxVtbl {
            size: std::mem::size_of::<DemuxVtbl>() as u32,
            reserved: 0,
            probe: demux_probe::<D>,
            open: demux_open::<D>,
            stream_count: demux_stream_count::<D>,
            stream_info: demux_stream_info::<D>,
            stream_config: demux_stream_config::<D>,
            select: demux_select::<D>,
            read_packet: demux_read_packet::<D>,
            seek: demux_seek::<D>,
            read_frames: None,
            close: demux_close::<D>,
        }
    }
}

// ---------------------------------------------------------------------------
// The host
// ---------------------------------------------------------------------------

/// The host vtable, kept from `init` to `shutdown`. The header promises it
/// outlives the module.
static HOST: AtomicPtr<Host> = AtomicPtr::new(std::ptr::null_mut());

extern "C" fn init(host: *const Host) -> MpResult {
    HOST.store(host.cast_mut(), Ordering::Release);
    result::OK
}

extern "C" fn shutdown() {
    HOST.store(std::ptr::null_mut(), Ordering::Release);
}

/// A line to the host's log, if there is a host. Interior NULs are replaced
/// rather than refused, because a log call that fails is a log call that
/// hides something.
#[allow(unsafe_code)]
pub fn log(level: u32, msg: &str) {
    let host = HOST.load(Ordering::Acquire);
    if host.is_null() {
        return;
    }
    let host = unsafe { &*host };
    let Some(log) = host.log else {
        return;
    };
    let line = CString::new(msg.replace('\0', "?")).unwrap_or_default();
    log(host.ctx, level, line.as_ptr());
}

impl ModuleDesc {
    const fn new(
        kind: u32,
        vtbl: *const c_void,
        id: &'static [u8],
        name: &'static [u8],
        version: u32,
        priority: u32,
        codecs: &'static [u32],
    ) -> Self {
        ModuleDesc {
            size: std::mem::size_of::<ModuleDesc>() as u32,
            abi_version: ABI_VERSION,
            flags: 0,
            version,
            kind,
            priority,
            id: id.as_ptr().cast::<c_char>(),
            name: name.as_ptr().cast::<c_char>(),
            init,
            shutdown,
            vtbl,
            codecs: codecs.as_ptr(),
            codec_count: codecs.len() as u32,
            reserved_desc: 0,
        }
    }

    /// A descriptor for a codec module. `id` and `name` must be NUL-terminated;
    /// the `export_codec!` macro sees to that. `codecs` is what it decodes.
    pub const fn codec(
        vtbl: &'static CodecVtbl,
        id: &'static [u8],
        name: &'static [u8],
        version: u32,
        priority: u32,
        codecs: &'static [u32],
    ) -> Self {
        Self::new(
            kind::CODEC,
            std::ptr::from_ref(vtbl).cast::<c_void>(),
            id,
            name,
            version,
            priority,
            codecs,
        )
    }

    /// A descriptor for a container module. `codecs` is what the container
    /// can carry, which the header calls a hint for a report rather than a
    /// promise.
    pub const fn demux(
        vtbl: &'static DemuxVtbl,
        id: &'static [u8],
        name: &'static [u8],
        version: u32,
        priority: u32,
        codecs: &'static [u32],
    ) -> Self {
        Self::new(
            kind::DEMUX,
            std::ptr::from_ref(vtbl).cast::<c_void>(),
            id,
            name,
            version,
            priority,
            codecs,
        )
    }
}

/// What `mp_module_entry` returns: the descriptor, or null when the host is
/// speaking a different ABI version.
pub fn entry(host_abi: u32, desc: &'static ModuleDesc) -> *const ModuleDesc {
    if host_abi != ABI_VERSION {
        return std::ptr::null();
    }
    desc
}

/// Makes a crate a codec module: the vtable, the descriptor, and the one
/// exported symbol, by that exact name.
///
/// `#[no_mangle]` counts as unsafe code -- a colliding symbol is undefined
/// behaviour -- so the one item that carries it is allowed to, and it is the
/// only line in a module crate that is.
#[macro_export]
macro_rules! export_codec {
    (
        $codec:ty,
        id: $id:literal,
        name: $name:literal,
        version: ($major:expr, $minor:expr, $patch:expr),
        priority: $priority:expr,
        codecs: [$($code:expr),+ $(,)?]
    ) => {
        static __MP_VTBL: $crate::CodecVtbl = $crate::CodecVtbl::new::<$codec>();
        static __MP_CODECS: &[u32] = &[$($code),+];
        static __MP_DESC: $crate::ModuleDesc = $crate::ModuleDesc::codec(
            &__MP_VTBL,
            concat!($id, "\0").as_bytes(),
            concat!($name, "\0").as_bytes(),
            $crate::make_version($major, $minor, $patch),
            $priority,
            __MP_CODECS,
        );

        #[allow(unsafe_code)]
        #[no_mangle]
        pub extern "C" fn mp_module_entry(host_abi: u32) -> *const $crate::ModuleDesc {
            $crate::entry(host_abi, &__MP_DESC)
        }
    };
}

/// Makes a crate a container module, the way `export_codec!` makes one a
/// codec. `codecs` is what the container can carry.
#[macro_export]
macro_rules! export_demux {
    (
        $demux:ty,
        id: $id:literal,
        name: $name:literal,
        version: ($major:expr, $minor:expr, $patch:expr),
        priority: $priority:expr,
        codecs: [$($code:expr),+ $(,)?]
    ) => {
        static __MP_VTBL: $crate::DemuxVtbl = $crate::DemuxVtbl::new::<$demux>();
        static __MP_CODECS: &[u32] = &[$($code),+];
        static __MP_DESC: $crate::ModuleDesc = $crate::ModuleDesc::demux(
            &__MP_VTBL,
            concat!($id, "\0").as_bytes(),
            concat!($name, "\0").as_bytes(),
            $crate::make_version($major, $minor, $patch),
            $priority,
            __MP_CODECS,
        );

        #[allow(unsafe_code)]
        #[no_mangle]
        pub extern "C" fn mp_module_entry(host_abi: u32) -> *const $crate::ModuleDesc {
            $crate::entry(host_abi, &__MP_DESC)
        }
    };
}
