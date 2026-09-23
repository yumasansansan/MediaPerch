#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sets up a GitHub Actions runner to build MediaPerch: Clang, LLD and the LLVM
# tools of one pinned version, first on PATH for every later step.
#
# **Pinned, and checked.** The runner image carries an LLVM of its own, of
# whatever version the image was built with, and a build that took that one
# would change compiler whenever the image did -- quietly, which is the one
# way a toolchain must not change. So the release archive of LLVM's own GitHub
# Releases is downloaded, its SHA-256 is checked against the value below, and
# MEDIAPERCH_LLVM_MAJOR is exported, which cmake/LLVMToolchain.cmake holds every
# configure to. To move to another version, change the three values below:
# GitHub lists the SHA-256 of a release archive as the asset's digest.
#
# What is not downloaded is the C++ library and the Windows SDK. Clang finds the
# MSVC STL and the SDK in the Visual Studio the image already has, by itself and
# without a developer prompt, and a build links the STL dynamically.
set -euo pipefail

llvm_major=23
llvm_release=23.1.2
windows_archive=clang+llvm-$llvm_release-x86_64-pc-windows-msvc.tar.zst
windows_sha256=ceaee048142fece144752c6f6431cb0905a7a6160f78ab8cf5cf0b6216f99418

download() {  # url file sha256
    curl --fail --location --silent --show-error --retry 3 --retry-all-errors \
        --output "$2" "$1"
    local actual
    actual=$(sha256sum "$2")
    actual=${actual%% *}
    if [ "$actual" != "$3" ]; then
        echo "error: $2 has SHA-256 $actual, expected $3" >&2
        exit 1
    fi
}

release_url() {  # archive
    local name=${1//+/%2B}
    echo "https://github.com/llvm/llvm-project/releases/download/llvmorg-$llvm_release/$name"
}

# The archive, without the static libraries at the top of lib/ -- those are for
# programs built on LLVM and are most of its size. The compiler's own runtime,
# libFuzzer and the sanitizers included, lies deeper, in lib/clang/, and stays.
#
# **`--long=30`, because the archive asks for more than zstd allows by default.**
# LLVM's release archives from 23.1.2 are compressed with a window larger than
# the 128 MiB zstd decodes unless it is told otherwise, and it refuses the frame
# rather than guess. --long=30 allows a window of up to 1 GiB when decoding, and
# changes nothing for an archive that needs less.
extract() {  # archive directory
    mkdir -p "$2"
    zstd --decompress --long=30 --stdout "$1" |
        tar -x -f - -C "$2" --strip-components 1 --no-wildcards-match-slash \
            --exclude "*/lib/*.lib"
    rm -f "$1"
}

case "${RUNNER_OS:-}" in
    Windows) ;;
    *) echo "error: MediaPerch's CI runs on Windows, not '${RUNNER_OS:-}'" >&2; exit 1 ;;
esac

temp=$(cygpath --unix "$RUNNER_TEMP")
download "$(release_url "$windows_archive")" "$temp/$windows_archive" "$windows_sha256"
extract "$temp/$windows_archive" "$temp/llvm"
llvm_bin=$temp/llvm/bin

version=$("$llvm_bin/clang" --version)
version=${version%%$'\n'*}
echo "$version"
case "$version" in
    *"clang version $llvm_major."*) ;;
    *) echo "error: expected Clang $llvm_major" >&2; exit 1 ;;
esac
"$llvm_bin/lld-link" --version
cmake --version | head -1
ninja --version

cygpath --windows "$llvm_bin" >> "$GITHUB_PATH"
echo "MEDIAPERCH_LLVM_MAJOR=$llvm_major" >> "$GITHUB_ENV"
