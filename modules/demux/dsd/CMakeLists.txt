# SPDX-License-Identifier: GPL-3.0-or-later
#
# DSF and DSDIFF, in Rust. The reading is the `dsdfile` crate in reader/, over
# any seekable stream; the module in src/ is the file and the ABI. See
# cmake/Rust.cmake for how a cargo build lands beside the C++ modules.

mediaperch_add_rust_module(mp_demux_dsd
    KIND demux
    MANIFEST modules/demux/dsd/Cargo.toml)
