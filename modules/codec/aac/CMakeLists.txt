# SPDX-License-Identifier: GPL-3.0-or-later
#
# AAC-LC, written here, in Rust -- after ALAC, and for the same reasons.
#
# There is no `mediaperch_aac` library beside the module any more because
# there is no need for one: the decoder is the `aac` crate in decoder/, which
# the tests and the fuzz target reach directly, so nothing has to be linkable.
# See decoder/src/lib.rs for why it is written at all, src/lib.rs for the
# module, and cmake/Rust.cmake for how a cargo build lands beside the C++
# modules.

mediaperch_add_rust_module(mp_codec_aac
    KIND codec
    MANIFEST modules/codec/aac/Cargo.toml)
