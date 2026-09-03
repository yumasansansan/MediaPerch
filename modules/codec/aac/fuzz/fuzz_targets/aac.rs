// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AAC-LC parser, given arbitrary bytes.
//
// This one earned its place before the C++ decoder it descends from was
// finished. The first version of read_section_data had no end-of-packet check
// in the loop that advances through scalefactor bands, so a zero-length
// section spun forever -- found by running the parser over a real file, but
// exactly what a fuzzer exists to find and the reason this target was written
// alongside the code rather than after.
//
// The input shape survived the move to Rust with the corpus: the first two
// bytes are an AudioSpecificConfig, the rest is a frame, so `fuzz/corpus/aac`
// seeds it exactly as it seeded `aac_fuzzer.cpp`. What it is looking for
// changed. In C++ a missed bounds check was a read past a buffer that only
// ASan could see; here it is a panic, which libFuzzer reports as a crash.

#![no_main]

use aac::{Config, Decoder};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if data.len() < 3 {
        return;
    }
    let Ok(cfg) = Config::parse(&data[..2]) else {
        return;
    };
    let Ok(mut decoder) = Decoder::new(&cfg) else {
        return;
    };
    let _ = decoder.decode_frame(&data[2..]);
    // Again from a different offset: a second frame exercises the paths that
    // assume a frame already went through -- the overlap, the window shape,
    // the noise generator's position.
    if data.len() > 8 {
        let _ = decoder.decode_frame(&data[4..]);
    }
});
