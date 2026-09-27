# SPDX-License-Identifier: GPL-3.0-or-later
#
# The patches of external/patches/, applied to the submodules when CMake
# configures, so that every build has them.
#
# **A fault in a library this tree ships is fixed in the library**, by a patch
# kept beside it and applied here, rather than read around in the code that
# calls it: a workaround protects the one caller that knows about it and leaves
# the fault in place for the next, and a patch is also the thing that can be
# offered upstream. A patch goes away once it is upstream and the submodule has
# moved on to a revision that carries it.
#
# Configuring twice is not an error: a patch that is already in place is left
# alone. Editing one makes CMake configure again.

find_package(Git QUIET)
if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR
        "git is needed to patch the submodules, and was not found. "
        "It is needed for the submodules themselves as well.")
endif()

# Applies <patch>, a file of external/patches/, to the working tree of the
# submodule <directory>, unless it is already there.
function(mediaperch_patch directory patch)
    set(tree "${CMAKE_SOURCE_DIR}/${directory}")
    set(file "${CMAKE_SOURCE_DIR}/external/patches/${patch}")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "The patch external/patches/${patch} is missing.")
    endif()
    if(NOT EXISTS "${tree}/.git")
        # A submodule that is not checked out is one its module skips itself
        # for, with a warning of its own; there is nothing here to patch.
        return()
    endif()
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")

    # A submodule checked out by another user is a working tree git refuses to
    # touch unless told the ownership is expected, which is what a CI runner's
    # workspace can hand it. The setting lasts for this one command.
    set(git "${GIT_EXECUTABLE}" -c "safe.directory=*")

    # Applying it in reverse would succeed if it were already applied.
    execute_process(COMMAND ${git} apply --reverse --check "${file}"
        WORKING_DIRECTORY "${tree}" RESULT_VARIABLE in_place
        OUTPUT_QUIET ERROR_QUIET)
    if(in_place EQUAL 0)
        message(STATUS "Patch, already applied: ${patch}")
        return()
    endif()

    execute_process(COMMAND ${git} apply "${file}"
        WORKING_DIRECTORY "${tree}" RESULT_VARIABLE applied
        ERROR_VARIABLE complaint)
    if(NOT applied EQUAL 0)
        message(FATAL_ERROR
            "external/patches/${patch} does not apply to ${directory}:\n${complaint}\n"
            "The submodule may have changes of its own, or may have moved on and "
            "carry the change already. git -C ${directory} status shows what is "
            "there; git -C ${directory} checkout -- . throws local changes away.")
    endif()
    message(STATUS "Patch: ${patch}")
endfunction()

mediaperch_patch("external/Bento4" "bento4-stts-entries-the-box-holds.patch")
mediaperch_patch("external/libebml" "libebml-msvc-swap_big32.patch")
mediaperch_patch("external/libebml" "libebml-a-void-of-any-size.patch")
mediaperch_patch("external/libmatroska" "libmatroska-lace-size-that-is-no-number.patch")
mediaperch_patch("external/vorbis" "libvorbis-floor1-room-doubled-not-shifted.patch")
