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
# MEDIAPERCH_OPUS_VERSION is set next to the submodule in
# modules/codec/opus/CMakeLists.txt, so the number and the gitlink are updated
# in one place.
function(get_package_version PACKAGE_VERSION PROJECT_VERSION)
    if(NOT MEDIAPERCH_OPUS_VERSION)
        message(FATAL_ERROR
            "MEDIAPERCH_OPUS_VERSION is not set. It is set beside the "
            "add_subdirectory for external/opus, and this file exists to use it.")
    endif()
    message(STATUS "opus ${MEDIAPERCH_OPUS_VERSION} (pinned; upstream would ask git)")
    set(PACKAGE_VERSION "${MEDIAPERCH_OPUS_VERSION}" PARENT_SCOPE)
    set(PROJECT_VERSION "${MEDIAPERCH_OPUS_VERSION}" PARENT_SCOPE)
endfunction()
