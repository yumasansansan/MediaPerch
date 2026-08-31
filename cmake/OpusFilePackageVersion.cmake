# SPDX-License-Identifier: GPL-3.0-or-later
#
# The same override as cmake/OpusPackageVersion.cmake, for opusfile. See that
# file for why upstream's version discovery cannot be relied on -- opusfile is
# the one that turns a missing version into a hard configure error rather than a
# warning.
function(get_package_version PACKAGE_VERSION PROJECT_VERSION)
    if(NOT MEDIAPERCH_OPUSFILE_VERSION)
        message(FATAL_ERROR
            "MEDIAPERCH_OPUSFILE_VERSION is not set. It is set beside the "
            "add_subdirectory for external/opusfile.")
    endif()
    message(STATUS "opusfile ${MEDIAPERCH_OPUSFILE_VERSION} (pinned; upstream would ask git)")
    set(PACKAGE_VERSION "${MEDIAPERCH_OPUSFILE_VERSION}" PARENT_SCOPE)
    set(PROJECT_VERSION "${MEDIAPERCH_OPUSFILE_VERSION}" PARENT_SCOPE)
endfunction()
