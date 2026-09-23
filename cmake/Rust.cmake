# SPDX-License-Identifier: GPL-3.0-or-later
#
# A module built by cargo, placed where the C++ ones go.
#
#   mediaperch_add_rust_module(mp_codec_alac KIND codec
#                              MANIFEST modules/codec/alac/Cargo.toml)
#
# **Nothing links across the language boundary**, which is what makes this
# twenty lines rather than a bridge. A module is a .dll on disk that exports
# `mp_module_entry`, and the host cannot tell which compiler produced it; so
# cargo builds the crate into its own target directory inside the build tree,
# and the file is copied to bin/<config>/modules/<kind>/ under the same name the
# C++ layout would give it. No Corrosion, no CRT matching, no change to the C++
# flags. docs/plan.md §2 promised that adding a Rust module would cost exactly
# this and no more; this file is the promise kept.
#
# The cargo profile follows the CMake configuration: Debug builds with the dev
# profile, which has overflow checks on -- the decoder's arithmetic is spelled
# `wrapping_*` where it wraps on purpose, so a check that fires is a bug found
# -- and everything else builds `--release`.
#
# A tree with no cargo still builds: the module is skipped with a warning, the
# way a missing submodule is, and the format falls to the next reader.

find_program(MEDIAPERCH_CARGO cargo)

# **cargo links with LLD too.** rustc's MSVC target runs link.exe unless it is
# told otherwise, and a build whose linker is LLD everywhere else should not
# have one corner where it is Microsoft's. So cargo is given the lld-link Clang
# runs (cmake/LLVMToolchain.cmake) as the host target's linker, through the
# environment variable cargo reads for it. rustc still finds the C runtime's
# and the SDK's import libraries in the Visual Studio installation, which it
# asks the system for by itself, exactly as Clang does.
set(MEDIAPERCH_CARGO_ENV "")
if(MEDIAPERCH_CARGO AND WIN32)
    get_filename_component(mediaperch_cargo_dir "${MEDIAPERCH_CARGO}" DIRECTORY)
    find_program(MEDIAPERCH_RUSTC rustc HINTS "${mediaperch_cargo_dir}")
    execute_process(COMMAND "${MEDIAPERCH_RUSTC}" -vV
        OUTPUT_VARIABLE mediaperch_rustc_info ERROR_QUIET)
    if(mediaperch_rustc_info MATCHES "host: ([^\n]+)")
        set(MEDIAPERCH_RUST_HOST "${CMAKE_MATCH_1}")
        string(TOUPPER "${MEDIAPERCH_RUST_HOST}" mediaperch_host_var)
        string(REPLACE "-" "_" mediaperch_host_var "${mediaperch_host_var}")
        set(MEDIAPERCH_CARGO_ENV "CARGO_TARGET_${mediaperch_host_var}_LINKER=${MEDIAPERCH_LLD}")
    else()
        message(FATAL_ERROR "rustc -vV did not say which target it is for:\n${mediaperch_rustc_info}")
    endif()
endif()

function(mediaperch_add_rust_module name)
    cmake_parse_arguments(M "" "KIND;MANIFEST" "" ${ARGN})
    if(NOT M_KIND OR NOT M_MANIFEST)
        message(FATAL_ERROR "mediaperch_add_rust_module(${name}) needs KIND and MANIFEST")
    endif()
    if(NOT MEDIAPERCH_CARGO)
        message(WARNING
            "cargo is not on PATH -- skipping ${name}. Install a stable Rust "
            "toolchain (rustup) to build it; the tree still builds without it.")
        return()
    endif()

    get_filename_component(manifest "${M_MANIFEST}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(target_dir "${CMAKE_BINARY_DIR}/cargo")

    # The file cargo writes, and the name the host scans for. On Windows they
    # agree; elsewhere cargo prefixes `lib`, which the copy removes.
    if(WIN32)
        set(produced "${name}${CMAKE_SHARED_MODULE_SUFFIX}")
    else()
        set(produced "lib${name}${CMAKE_SHARED_MODULE_SUFFIX}")
    endif()
    set(installed "${name}${CMAKE_SHARED_MODULE_SUFFIX}")

    if(CMAKE_CONFIGURATION_TYPES)
        # Multi-config: the profile and the destination follow $<CONFIG>.
        set(profile_flag "$<$<NOT:$<CONFIG:Debug>>:--release>")
        set(profile_dir "$<IF:$<CONFIG:Debug>,debug,release>")
        set(out_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/modules/${M_KIND}")
    else()
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            set(profile_flag "")
            set(profile_dir "debug")
        else()
            set(profile_flag "--release")
            set(profile_dir "release")
        endif()
        set(out_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules/${M_KIND}")
    endif()

    # A custom target rather than a custom command with an OUTPUT, because
    # cargo owns the dependency tracking: it is run on every build and returns
    # in a moment when nothing changed.
    add_custom_target(${name} ALL
        COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${target_dir}"
                ${MEDIAPERCH_CARGO_ENV}
                "${MEDIAPERCH_CARGO}" build --quiet ${profile_flag}
                --manifest-path "${manifest}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${target_dir}/${profile_dir}/${produced}" "${out_dir}/${installed}"
        COMMAND_EXPAND_LISTS
        COMMENT "cargo: ${name}"
        VERBATIM)
    set_target_properties(${name} PROPERTIES FOLDER "modules/${M_KIND}")

    # The crate's own tests are registered by tests/CMakeLists.txt, which runs
    # after enable_testing(). An add_test issued here would be issued before
    # it, and CTest drops those without a word -- measured: the test was
    # missing from `ctest -N` and nothing said so.
    set_property(GLOBAL APPEND PROPERTY MEDIAPERCH_RUST_TESTS "${name}|${manifest}")
endfunction()

# `cargo test` over the whole Rust workspace, under ctest beside the C++ tests,
# as one entry: the decoder crates carry the tests, the module crates are glue
# with none, and a workspace run covers every module added later without a
# line here. Called from tests/CMakeLists.txt. The dev profile on purpose:
# overflow checks on, which is the profile a decoder's tests should run under
# -- the arithmetic that wraps by design says `wrapping_*`, so a check that
# fires is a bug.
function(mediaperch_add_rust_tests)
    get_property(entries GLOBAL PROPERTY MEDIAPERCH_RUST_TESTS)
    if(NOT entries)
        return()
    endif()
    add_test(NAME rust_modules
             COMMAND "${CMAKE_COMMAND}" -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/cargo"
                     ${MEDIAPERCH_CARGO_ENV}
                     "${MEDIAPERCH_CARGO}" test --quiet --workspace
                     --manifest-path "${CMAKE_SOURCE_DIR}/modules/Cargo.toml")
    set_tests_properties(rust_modules PROPERTIES TIMEOUT 600)
endfunction()
