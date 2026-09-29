#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Sets up a GitHub Actions runner to build and test MediaPerch: Clang, LLD and
# the LLVM tools of one pinned version, and on Ubuntu what the external decoders
# are built with. The tools go first on PATH for the later steps. Every download
# is checked: the apt.llvm.org signing key by its fingerprint, and the release
# archive by its SHA-256. ADLplug-Next's ci/setup.sh is the same script in the
# same shape, for more systems than the two MediaPerch's CI runs on, Windows and
# Ubuntu.
#
# **Pinned, and checked.** The runner image carries an LLVM of its own, of
# whatever version the image was built with, and a build that took that one
# would change compiler whenever the image did -- quietly, which is the one
# way a toolchain must not change. MEDIAPERCH_LLVM_MAJOR is exported, which
# cmake/LLVMToolchain.cmake holds every configure to. To move to another
# version, change the values below: GitHub lists the SHA-256 of a release
# archive as the asset's digest.
#
# What is not downloaded on Windows is the C++ library and the Windows SDK.
# Clang finds the MSVC STL and the SDK in the Visual Studio the image already
# has, by itself and without a developer prompt, and a build links the STL
# dynamically. On Ubuntu the C++ library is the system's libstdc++, linked
# dynamically as well, with the headers of the GCC its runtime comes from.
set -euo pipefail

llvm_major=23

# Windows: LLVM's release archive.
llvm_release=23.1.2
windows_archive=clang+llvm-$llvm_release-x86_64-pc-windows-msvc.tar.zst
windows_sha256=ceaee048142fece144752c6f6431cb0905a7a6160f78ab8cf5cf0b6216f99418

# Ubuntu: the packages of apt.llvm.org, signed with this key.
apt_key_fingerprint=6084F3CF814B57C1CF12EFD515CF4D18AF4F7421

# What the external decoders are built with on Ubuntu, besides the compiler:
# nasm assembles the x86 code of libaom, avm, libvpx and dav1d, Meson builds
# dav1d, make builds libvpx, and Ninja is what every preset generates for. The
# Windows jobs install theirs in steps of their own (.github/workflows/ci.yml).
linux_packages=(nasm meson ninja-build make)

# Pipelines below are written so that no command stops reading early: with
# pipefail, a writer killed by SIGPIPE would fail the script.

download() {  # url file sha256
    curl --fail --location --silent --show-error --retry 3 --retry-all-errors --output "$2" "$1"
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

# Extracts a release archive into a directory, leaving out the static
# libraries at the top of lib/, which are for programs built on LLVM and take
# up most of the space. The compiler runtime, libFuzzer and the sanitizers
# included, lies deeper, in lib/clang/.
#
# From 23.1.2 on, LLVM compresses the archives with a window of 1 GiB (--long=30
# in its .github/workflows/release-binaries.yml), and zstd decompresses a window
# larger than 128 MiB only when it is told it may: without --long=30 it stops at
# the first frame and asks for it.
extract() {  # archive directory tar static-library-suffix
    mkdir -p "$2"
    zstd --decompress --long=30 --stdout "$1" |
        "$3" -x -f - -C "$2" --strip-components 1 --no-wildcards-match-slash --exclude "*/lib/*$4"
    rm -f "$1"
}

setup_linux() {
    local key=$RUNNER_TEMP/apt.llvm.org.asc
    local keyring=/usr/share/keyrings/apt.llvm.org.gpg
    curl --fail --location --silent --show-error --retry 3 --retry-all-errors --output "$key" \
        https://apt.llvm.org/llvm-snapshot.gpg.key
    local fingerprint
    fingerprint=$(gpg --show-keys --with-colons "$key" | awk -F : '$1 == "fpr" && !found { print $10; found = 1 }')
    if [ "$fingerprint" != "$apt_key_fingerprint" ]; then
        echo "error: the apt.llvm.org key has fingerprint $fingerprint, expected $apt_key_fingerprint" >&2
        exit 1
    fi
    gpg --dearmor < "$key" | sudo tee "$keyring" > /dev/null

    local codename
    codename=$(. /etc/os-release && echo "$VERSION_CODENAME")
    # The sources as well as the binaries. What apt.llvm.org offers for a
    # release is a snapshot of its branch, rebuilt often, so two builds of the
    # same version number are not the same sources: the only way to have the
    # sources of the very binaries installed here is to ask apt for them, which
    # the memory sanitizer's C++ library is built from (ci/msan-libraries.sh).
    {
        echo "deb [signed-by=$keyring] https://apt.llvm.org/$codename/ llvm-toolchain-$codename-$llvm_major main"
        echo "deb-src [signed-by=$keyring] https://apt.llvm.org/$codename/ llvm-toolchain-$codename-$llvm_major main"
    } | sudo tee /etc/apt/sources.list.d/apt.llvm.org.list > /dev/null

    # **The C++ library's headers, of the GCC its runtime comes from.** Clang
    # compiles against the headers of the newest GCC it finds them for, and an
    # image may carry the headers of an older GCC than the one its libstdc++6
    # was built from: a program then sees only what the older headers declare,
    # while it runs on the newer library. The development package of the
    # runtime's own GCC closes the gap, whichever GCC that is.
    local runtime
    runtime=$(dpkg-query --show --showformat '${source:Package}' libstdc++6)
    case "$runtime" in
        gcc-[0-9]*) ;;
        *) echo "error: libstdc++6 was built from '$runtime', not from a gcc-<N> source package" >&2; exit 1 ;;
    esac

    sudo apt-get update -qq
    # clang-tools brings clang-scan-deps, which CMake runs over every C++23
    # source for the modules it might import. libclang-rt has the sanitizers'
    # runtimes and libFuzzer, which the sanitizer and fuzzing jobs link and
    # which the compiler only recommends.
    sudo apt-get install -y -qq --no-install-recommends \
        "clang-$llvm_major" "lld-$llvm_major" "llvm-$llvm_major" "clang-tools-$llvm_major" \
        "libclang-rt-$llvm_major-dev" \
        "libstdc++-${runtime#gcc-}-dev" \
        "${linux_packages[@]}"
    llvm_from=apt
    llvm_bin=/usr/lib/llvm-$llvm_major/bin
}

setup_windows() {
    llvm_from=release
    local temp
    temp=$(cygpath --unix "$RUNNER_TEMP")
    download "$(release_url "$windows_archive")" "$temp/$windows_archive" "$windows_sha256"
    extract "$temp/$windows_archive" "$temp/llvm" tar .lib
    llvm_bin=$temp/llvm/bin
}

llvm_from=
case "${RUNNER_OS:-}" in
    Linux) setup_linux ;;
    Windows) setup_windows ;;
    *) echo "error: MediaPerch's CI runs on Windows and Ubuntu, not '${RUNNER_OS:-}'" >&2; exit 1 ;;
esac

version=$("$llvm_bin/clang" --version)
version=${version%%$'\n'*}
echo "$version"
case "$version" in
    *"clang version $llvm_major."*) ;;
    *) echo "error: expected Clang $llvm_major" >&2; exit 1 ;;
esac
cmake --version
ninja --version

if [ "$RUNNER_OS" = Windows ]; then
    cygpath --windows "$llvm_bin"
else
    echo "$llvm_bin"
fi >> "$GITHUB_PATH"

# The configuration of every build checks that its toolchain is of this
# version of LLVM (cmake/LLVMToolchain.cmake).
echo "MEDIAPERCH_LLVM_MAJOR=$llvm_major" >> "$GITHUB_ENV"

# Where LLVM came from: Ubuntu takes the packages of apt.llvm.org, and Windows
# the archive of LLVM's own GitHub Releases. It is ADLplug-Next's rule as well,
# exported under the same name there, where the memory sanitizer's C++ library
# is built from the sources of the very compiler installed rather than of a
# compiler of the same number. A release is a tag and a tag does not move, while
# what apt.llvm.org offers for a release is a snapshot of its branch, rebuilt
# often, and only apt can say which of those builds is the one installed.
if [ -z "$llvm_from" ]; then
    echo "error: the setup of this runner did not say where LLVM came from" >&2
    exit 1
fi
echo "MEDIAPERCH_LLVM_FROM=$llvm_from" >> "$GITHUB_ENV"
