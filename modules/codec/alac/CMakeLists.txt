# SPDX-License-Identifier: GPL-3.0-or-later
#
# ALAC, written here, in Rust -- the first module in the tree that is not C++.
#
# There is no library beside the module any more because there is no need for
# one: the crate is both the .dll the host loads and an rlib, so the tests and
# the fuzz target reach the decoder directly, which is what `mediaperch_alac`
# existed to allow. See src/lib.rs for why Rust, and cmake/Rust.cmake for how a
# cargo build lands beside the C++ modules.

mediaperch_add_rust_module(mp_codec_alac
    KIND codec
    MANIFEST modules/codec/alac/Cargo.toml)
