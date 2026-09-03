// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ALAC codec, given arbitrary bytes.
//
// This is the fuzzer that had to exist before an ALAC decoder written here
// could be defended, and it survived the decoder's move from C++ to Rust with
// the same input shape: the first 24 bytes are a magic cookie, the rest is a
// packet, so `fuzz/corpus/alac` seeds it exactly as it seeded `alac_fuzzer.cpp`.
//
// What it is looking for changed. In C++ a missed bounds check was a read past
// a buffer that only ASan could see; here it is a panic, which libFuzzer
// reports as a crash. Every `wrapping_*` in the decoder is a place the C++
// relied on two's-complement overflow, and a panic here would be a place it
// was missed -- so the target is built with overflow checks on.

#![no_main]

use alac::{Config, Decoder};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if data.len() <= 24 {
        return;
    }
    let Ok(cfg) = Config::parse(&data[..24]) else {
        return;
    };
    let Ok(mut decoder) = Decoder::new(&cfg) else {
        return;
    };
    let mut out = vec![0i32; cfg.frame_length as usize * usize::from(cfg.channels)];
    let _ = decoder.decode(&data[24..], &mut out);
    // Again from the middle of the buffer: a second packet exercises the paths
    // that assume a frame followed a frame.
    if data.len() > 64 {
        let _ = decoder.decode(&data[32..], &mut out);
    }
});
