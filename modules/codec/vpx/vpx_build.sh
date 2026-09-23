#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# libvpx's build: its own configure and its own makefiles, with Clang.
#
# **`x86_64-win64-gcc`, with Clang where gcc would be.** libvpx's configure
# knows two ways to build for 64-bit Windows: `-vs` targets, which generate
# Visual Studio projects for MSBuild to build with MSVC -- how this tree built
# it while MSVC was the compiler -- and `-gcc`, which is its own makefiles
# driving whatever CC names. The second takes Clang as it takes GCC. Clang's
# GNU driver targets the MSVC ABI by default on Windows, so what comes out is
# COFF objects for the same C runtime as everything else here, and the assembly
# is nasm's win64 output either way. Measured: configure turns on the x86
# assembly and run-time CPU detection through AVX-512, as it did for the `-vs`
# target, and the archive builds with no change to libvpx.
#
# **The C runtime is said, not left to the link.** Clang's GNU driver compiles
# for no particular C runtime unless it is told one, and libvpx's makefiles do
# not tell it; -fms-runtime-lib=dll is /MD, which defines _DLL for the headers
# and names msvcrt for the linker, and is what the module links.
#
# **This runs under MSYS2 and that is not a preference.** libvpx's scripts use
# MSYS2's sed and cut, and its rules put long lists on one command line. A
# native Windows GNU make runs a command through cmd.exe, whose limit is 8191
# characters; it once truncated this build's source list mid-word --
#
#     ../libvpx/vpx_dsp/x86/inv_txfm
#
# -- and the archive that came out could not resolve symbols from its own
# objects, with nothing reporting it. MSYS2's make runs commands through its
# own bash, so the limit is CreateProcess's 32767 and the lists fit.
#
# Arguments: <src> <build> <checks> <cc> <cxx> <ar> <strip> <nasm>
set -e

# **MSYS2's own tools first, before anything else runs.** This script is started
# by CMake with the parent's PATH, which is a Windows one: `cygpath`, `sed` and
# `cut` all have to be MSYS2's, and libvpx's scripts assume so.
export PATH="/usr/bin:$PATH"

src=$(/usr/bin/cygpath -u "$1")
build=$(/usr/bin/cygpath -u "$2")
checks=$3

# **The toolchain the parent build was checked against**, found by putting its
# directory first on PATH and naming each tool bare. Not by path: libvpx's
# makefiles expand $(CC) unquoted, and LLVM's directory on Windows is usually
# C:\Program Files\LLVM\bin -- a compiler called `/c/Program`. LLVM's
# directory holds no sed, cut or make, so nothing of MSYS2's is shadowed.
# `--as=nasm` looks nasm up by name the same way.
llvm_bin=$(dirname "$(/usr/bin/cygpath -u "$4")")
nasm=$(/usr/bin/cygpath -u "$8")
export PATH="$llvm_bin:$PATH:$(dirname "$nasm")"
export CC=$(basename "$4" .exe)
export CXX=$(basename "$5" .exe)
export AR=$(basename "$6" .exe)
export STRIP=$(basename "$7" .exe)
export NM=llvm-nm
export LD="$CC"
export CROSS=
export LDFLAGS=-fuse-ld=lld

mkdir -p "$build"
cd "$build"

# **VP8 and VP9 decoders, both**, because libvpx is the reference for both and
# reading only the newer one would leave half the reason it is here. The
# encoders go: nothing in this tree encodes.
#
# --enable-vp9-highbitdepth is off by default and is what makes VP9 profiles 2
# and 3 -- ten and twelve bits, 4:2:2 and 4:4:4 -- readable at all. Leaving it
# off would let ABI v4 describe depths this decoder then refused to produce.
#
# webm-io and libyuv are on by default and are both jobs this tree does itself:
# containers are demuxers here and colour conversion is a shader.
options=(
    --target=x86_64-win64-gcc
    --as=nasm
    --extra-cflags=-fms-runtime-lib=dll
    --enable-vp9-highbitdepth
    --disable-vp8-encoder
    --disable-vp9-encoder
    --disable-examples
    --disable-tools
    --disable-docs
    --disable-install-docs
    --disable-unit-tests
    --disable-webm-io
    --disable-libyuv
)
# The decoder checks its own intermediate transform coefficients. Slower, and
# what a build being fuzzed or audited wants rather than one being shipped --
# see MEDIAPERCH_DECODER_CHECKS.
[ "$checks" = "1" ] && options+=(--enable-coefficient-range-checking)

# **A build directory is configured once, and checked against what it was
# configured with.** Configuring once is what makes the next build incremental
# -- and what let a directory configured for something else go on building
# with it: a tree once built with MSVC had libvpx configured for the `-vs`
# target, whose makefiles have no rule for `libvpx.a` at all, and a change of
# MEDIAPERCH_DECODER_CHECKS never reached configure either. So the options and
# the compilers, by path, are kept beside the build, and a directory configured
# with anything else is emptied and configured again.
wanted="${options[*]} $llvm_bin/$CC $llvm_bin/$CXX $llvm_bin/$AR"
if [ ! -f config.mk ] || [ "$(cat mediaperch-configure.txt 2>/dev/null)" != "$wanted" ]; then
    find . -mindepth 1 -delete
    "$src/configure" "${options[@]}"
    printf '%s' "$wanted" > mediaperch-configure.txt
fi

# The library and nothing else: examples, tools and tests are off above.
make -j"$(nproc)" libvpx.a
