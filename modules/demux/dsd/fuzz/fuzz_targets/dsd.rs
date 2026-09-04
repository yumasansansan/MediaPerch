// SPDX-License-Identifier: GPL-3.0-or-later
//
// The DSF and DSDIFF readers, given arbitrary bytes.
//
// **These two headers are what a fuzzer is for.** DSDIFF's chunk sizes are
// 64-bit and big-endian, its chunks nest, and every one of them is a length
// this reader has to decide whether to believe; DSF's are 64-bit too, with a
// block size and a sample count that a file is free to disagree with itself
// about. Every arithmetic bug that class of header has ever produced is an
// overflow, and in Rust an overflow that was not spelled out is a panic --
// which libFuzzer reports as a crash, and which is why this target is built
// with overflow checks on where a release build would have them off.
//
// The whole buffer is a file, so a `Cursor` over it is what the module hands a
// `File`, and the corpus is real DSF and DSDIFF files.

#![no_main]

use std::io::Cursor;

use dsdfile::Reader;
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if dsdfile::probe(data) == 0 {
        // Still worth opening: `probe` sees the first bytes and `open` reads
        // the whole header, so the two disagreeing is itself a thing to find.
    }
    let Ok(Ok(mut reader)) = Reader::open(Cursor::new(data)) else {
        return;
    };
    let mut buf = vec![0u8; reader.max_packet_bytes().min(1 << 20)];
    // Bounded: a header can claim a very long stream out of very few bytes, and
    // a fuzzer that spends a minute on one input finds nothing.
    for _ in 0..64 {
        match reader.read_packet(&mut buf) {
            Ok(Some(_)) => {}
            _ => break,
        }
    }
    // And from the middle, which is the path the block arithmetic takes with a
    // non-zero offset into a group.
    let _ = reader.seek(4097);
    let _ = reader.read_packet(&mut buf);
    let _ = reader.seek(u64::MAX);
    let _ = reader.read_packet(&mut buf);
});
