// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiles the shared sancov_sections.c on the MSVC target, and nothing
// anywhere else: ELF linkers provide those symbols themselves.

fn main() {
    let shim = "../../../shared/fuzz/sancov_sections.c";
    println!("cargo:rerun-if-changed={shim}");
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        cc::Build::new().file(shim).compile("sancov_sections");
    }
}
