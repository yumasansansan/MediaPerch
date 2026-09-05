#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# libvpx's build: a shell script, a makefile and MSBuild in sequence.
#
# **This runs under MSYS2 and that is not a preference.** libvpx's Windows
# build is documented for MSYS2 and needs it for a reason that took a while to
# find: `libs.mk` hands the whole source list to `gen_msvs_vcxproj.sh` on one
# command line, and a native Windows GNU make runs that through cmd.exe, whose
# limit is 8191 characters. The list is about 11,800. Measured by making the
# script print its own arguments: it received 223 of them, 7917 bytes, with the
# last one cut mid-word --
#
#     ../libvpx/vpx_dsp/x86/inv_txfm
#
# -- and 7917 plus the leading options is 8191 exactly. libs.mk names vp8, vp9,
# vpx and vpx_dsp in explicit filter clauses first and sweeps the rest up in a
# filter-out clause last, so what falls off the end is always vpx_mem,
# vpx_ports, vpx_scale and vpx_util. **Nothing reports it**: the truncated
# command exits cleanly, writes a plausible project, and produces an archive
# that fails at link time on symbols from its own objects.
#
# MSYS2's make runs commands through its own bash, so the limit becomes
# CreateProcess's 32767 and the list fits. Under it the generator receives 319
# arguments and the archive goes from 110 objects to 157, with vpx_mem among
# them. `SHELL=` and `MAKESHELL=` pointing a native make at a POSIX shell do
# not help; the whole userland has to be MSYS2's, because libvpx's scripts use
# its sed and cut too.
#
# Arguments: <src> <build> <target> <checks>
set -e

# **MSYS2's own tools first, before anything else runs.** This script is started
# by CMake with the parent's PATH, which is a Windows one: `cygpath`, `sed` and
# `cut` all have to be MSYS2's, and libvpx's scripts assume so.
export PATH="/usr/bin:$PATH"

src=$(/usr/bin/cygpath -u "$1")
build=$(/usr/bin/cygpath -u "$2")
target=$3
checks=$4

# MSBuild is what libvpx's generated rules call, and MSYS2 does not inherit it
# even when the parent environment has it. The developer environment this tree
# already requires is where it comes from.
for dir in "$VSINSTALLDIR" "$VCINSTALLDIR/../"; do
    [ -n "$dir" ] || continue
    found=$(find "$(/usr/bin/cygpath -u "$dir")MSBuild" -name MSBuild.exe -path '*/Bin/*' 2>/dev/null | head -1)
    if [ -n "$found" ]; then
        PATH="$PATH:$(dirname "$found")"
        break
    fi
done
command -v MSBuild.exe >/dev/null 2>&1 || {
    echo "MSBuild is not on PATH; libvpx's generated solution cannot be built." >&2
    exit 1
}

mkdir -p "$build"
cd "$build"

if [ ! -f config.mk ]; then
    # **VP8 and VP9 decoders, both**, because libvpx is the reference for both
    # and reading only the newer one would leave half the reason it is here.
    # The encoders go: nothing in this tree encodes.
    #
    # --enable-vp9-highbitdepth is off by default and is what makes VP9
    # profiles 2 and 3 -- ten and twelve bits, 4:2:2 and 4:4:4 -- readable at
    # all. Leaving it off would let ABI v4 describe depths this decoder then
    # refused to produce.
    #
    # webm-io and libyuv are on by default and are both jobs this tree does
    # itself: containers are demuxers here and colour conversion is a shader.
    options=(
        "--target=$target"
        --as=nasm
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
    # The decoder checks its own intermediate transform coefficients. Slower,
    # and what a build being fuzzed or audited wants rather than one being
    # shipped -- see MEDIAPERCH_DECODER_CHECKS.
    [ "$checks" = "1" ] && options+=(--enable-coefficient-range-checking)

    "$src/configure" "${options[@]}"
fi

# Generates the Visual Studio projects and calls MSBuild on them. Both
# configurations get built, because that is what libvpx's rules do and there is
# no switch for it; only the Release one is linked against.
make
