# SPDX-License-Identifier: GPL-3.0-or-later
#
# DSD as DoP, in Rust. Nothing here decodes: the file's bits are the waveform,
# and this puts them in the frames a Windows endpoint can carry. The packing is
# the `dop` crate in packer/.

mediaperch_add_rust_module(mp_codec_dsd
    KIND codec
    MANIFEST modules/codec/dsd/Cargo.toml)
