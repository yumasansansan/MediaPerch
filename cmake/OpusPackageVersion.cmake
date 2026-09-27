# SPDX-License-Identifier: GPL-3.0-or-later
#
# Overrides external/opus/cmake/OpusPackageVersion.cmake, by the same mechanism
# as cmake/FindOgg.cmake: this directory is on CMAKE_MODULE_PATH first, and opus
# reaches its own copy with `list(APPEND ...)`.
#
# Upstream derives its version from `git describe --tags`, and has no fallback
# that works: `configure.ac` carries the literal placeholder CURRENT_VERSION,
# which their release script fills in, and no `package_version` file is
# committed. So in any checkout without tags -- a CI runner fetching submodules
# at depth 1, a source archive, a `git clone --depth` -- the describe fails and
# the version becomes "0". That broke the configure outright while opusfile was
# in the tree, and CI is where it was found; a full clone has the tags, so it did
# not reproduce locally.
#
# The version is used for `project(VERSION)`, a SOVERSION and some macOS
# framework metadata, all of it on static libraries this build never installs.
# So pinning it costs nothing and removes git from the configure entirely.
# **The number is here, and is updated with the gitlink**: both trees that build
# libopus -- modules/codec/opus and the fuzzers -- come through this file, so
# it is the one place that serves them both.
function(get_package_version PACKAGE_VERSION PROJECT_VERSION)
    set(version "1.6.1")      # external/opus at v1.6.1
    message(STATUS "opus ${version} (pinned; upstream would ask git)")
    set(PACKAGE_VERSION "${version}" PARENT_SCOPE)
    set(PROJECT_VERSION "${version}" PARENT_SCOPE)
endfunction()
