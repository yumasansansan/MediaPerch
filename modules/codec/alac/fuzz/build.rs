// SPDX-License-Identifier: GPL-3.0-or-later
//
// Compiles sancov_sections.c on the MSVC target, and nothing anywhere else:
// ELF linkers provide those symbols themselves.

fn main() {
    println!("cargo:rerun-if-changed=sancov_sections.c");
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        cc::Build::new()
            .file("sancov_sections.c")
            .compile("sancov_sections");
    }
}
