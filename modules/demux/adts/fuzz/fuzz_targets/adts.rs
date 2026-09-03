// SPDX-License-Identifier: GPL-3.0-or-later
//
// The ADTS framer, given arbitrary bytes.
//
// The C++ framer had no fuzzer -- the AAC decoder behind it did, and the
// framer was seven bytes of header. In Rust the framer walks any `Read + Seek`,
// so a `Cursor` over the fuzzer's buffer is a whole file, and the target costs
// forty lines: open, walk the packets, seek into the middle, walk again. What a
// crash means here is a panic, which is an index the framer computed past the
// data it had.

#![no_main]

use std::io::Cursor;

use adts::{Packet, Stream, FRAME_SAMPLES};
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    let Ok(Some(mut stream)) = Stream::open(Cursor::new(data)) else {
        return;
    };
    let mut buf = vec![0u8; 8192];
    // Bounded: a crafted length field can claim frames out of very few bytes,
    // and a fuzzer that spends a minute on one input finds nothing.
    for _ in 0..64 {
        match stream.read_packet(&mut buf) {
            Ok(Packet::Frame { .. }) => {}
            _ => break,
        }
    }
    let _ = stream.seek(3 * FRAME_SAMPLES);
    let _ = stream.read_packet(&mut buf);
    let _ = stream.seek(0);
    let _ = stream.read_packet(&mut buf[..4]);
});
