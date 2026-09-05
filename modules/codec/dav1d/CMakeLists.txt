# SPDX-License-Identifier: GPL-3.0-or-later
#
# dav1d, and the one submodule in this tree that CMake cannot build.
#
# **Every other external here is `add_subdirectory` and this one cannot be.**
# dav1d builds with Meson and nothing else: 38 C sources of which 13 are
# compiled twice for the two bit depths, 47 nasm files, and no CMake anywhere
# in its tree. Writing a CMakeLists for it would mean reimplementing the
# bitdepth templating and the assembly rules and then maintaining them against
# upstream -- and dropping the assembly to make that tractable would leave a
# decoder with dav1d's name and not its speed, which is the only reason to
# prefer it over libaom in the first place.
#
# So Meson is driven as an external project. It is a real build dependency and
# is stated as one: `meson`, `ninja` and `nasm` on PATH, which on the machine
# this was written on come from a micromamba environment --
#
#   micromamba create -y -n mediaperch-build -c conda-forge python meson ninja nasm
#   micromamba activate mediaperch-build
#
# -- and the module is skipped, loudly, when they are not there. The tree still
# builds; it just has no AV1 decoder, which is the same shape every other
# optional module here takes.
#
# **The release CRT, always, and the ABI is what makes that safe.** dav1d is
# built once as a release static library rather than once per configuration,
# because building it four times is minutes for a library whose debug build
# nobody wants to step through. That leaves a Debug MediaPerch linking a
# release-CRT archive, which would be a mixed-runtime bug in ordinary code and
# is not one here: this module is a DLL of its own, and §4's rule that nothing
# is allocated across the module boundary means no CRT object ever crosses it.
# The module is pinned to the same runtime so the two agree.
#
# dav1d chooses its assembly with CPUID rather than at compile time, so
# MEDIAPERCH_ARCH does not reach it -- and is not made to. See the comment in
# codec_dav1d.cpp for the measurement behind that, which is not the obvious
# answer.

set(dav1d_src "${CMAKE_SOURCE_DIR}/external/dav1d")

if(NOT EXISTS "${dav1d_src}/meson.build")
    message(WARNING
        "external/dav1d is missing -- run `git submodule update --init`. "
        "Skipping codec_dav1d; the tree still builds without it and simply has "
        "no AV1 decoder.")
    return()
endif()

find_program(MEDIAPERCH_MESON NAMES meson meson.exe
    DOC "Meson, which builds external/dav1d. Nothing else in this tree needs it.")
find_program(MEDIAPERCH_NINJA NAMES ninja ninja.exe
    DOC "Ninja, which Meson drives to build external/dav1d.")

if(NOT MEDIAPERCH_MESON OR NOT MEDIAPERCH_NINJA)
    message(WARNING
        "meson or ninja is not on PATH, and external/dav1d builds with no other "
        "build system. Skipping codec_dav1d -- the tree still builds and simply "
        "has no AV1 decoder. See the comment at the top of this file for one way "
        "to get them.")
    return()
endif()

include(ExternalProject)

set(dav1d_prefix "${CMAKE_BINARY_DIR}/external/dav1d")
set(dav1d_install "${dav1d_prefix}/install")
# Meson names the archive by the platform's convention: `libdav1d.a` even under
# MSVC, which links it perfectly well.
set(dav1d_library "${dav1d_install}/lib/libdav1d.a")

ExternalProject_Add(dav1d_external
    SOURCE_DIR "${dav1d_src}"
    BINARY_DIR "${dav1d_prefix}/build"
    INSTALL_DIR "${dav1d_install}"
    # Through a script, because the one argument `meson setup` needs depends
    # on whether it has run in that directory before. See meson_setup.cmake.
    CONFIGURE_COMMAND "${CMAKE_COMMAND}"
        "-Dmeson=${MEDIAPERCH_MESON}"
        "-Dsrc=${dav1d_src}"
        "-Dbuild=${dav1d_prefix}/build"
        "-Dprefix=${dav1d_install}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/meson_setup.cmake"
    BUILD_COMMAND "${MEDIAPERCH_MESON}" compile -C "${dav1d_prefix}/build"
    INSTALL_COMMAND "${MEDIAPERCH_MESON}" install -C "${dav1d_prefix}/build"
    BUILD_BYPRODUCTS "${dav1d_library}"
    # **The submodule can move under this, and ExternalProject does not look.**
    # It stamps the configure and build steps and then trusts them, so a
    # `git checkout` in external/dav1d -- a version bump, or a submodule update
    # after a pull -- leaves the built library at whatever it was before, with
    # nothing saying so. That is not hypothetical: the first build here was made
    # while the submodule sat on master and stayed that way after it was pinned
    # to 1.5.4, and `vcs_version.h` was the only thing that admitted it.
    #
    # BUILD_ALWAYS re-runs the build step every time, which is what makes that
    # visible. It costs almost nothing: `meson compile` runs Ninja in dav1d's
    # own directory, Ninja notices that nothing changed, and it returns. When
    # something *has* changed it rebuilds exactly that, including re-running
    # Meson itself when meson.build moved.
    BUILD_ALWAYS ON
    USES_TERMINAL_BUILD OFF
    LOG_CONFIGURE ON
    LOG_BUILD ON
    LOG_INSTALL ON
    LOG_OUTPUT_ON_FAILURE ON)

add_library(dav1d::dav1d STATIC IMPORTED GLOBAL)
set_target_properties(dav1d::dav1d PROPERTIES
    IMPORTED_LOCATION "${dav1d_library}"
    INTERFACE_INCLUDE_DIRECTORIES "${dav1d_install}/include")
# The include directory does not exist until the external project installs, and
# CMake refuses an INTERFACE include path that is not there yet.
file(MAKE_DIRECTORY "${dav1d_install}/include")
add_dependencies(dav1d::dav1d dav1d_external)

mediaperch_add_module(mp_codec_dav1d
    KIND codec
    SOURCES codec_dav1d.cpp
    LINK dav1d::dav1d)

# See the note above: one release build of dav1d, and a module pinned to the
# runtime it was built against.
set_target_properties(mp_codec_dav1d PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")

