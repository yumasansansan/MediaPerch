# SPDX-License-Identifier: GPL-3.0-or-later
#
# Raw AAC in ADTS framing, in Rust -- the first container module that is. The
# framing is the `adts` crate in framer/, over any seekable reader; the module
# in src/ is the file and the ABI. See cmake/Rust.cmake for the build.

mediaperch_add_rust_module(mp_demux_adts
    KIND demux
    MANIFEST modules/demux/adts/Cargo.toml)
