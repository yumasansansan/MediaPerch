# SPDX-License-Identifier: GPL-3.0-or-later
#
# The shell (M8), built by `dotnet` rather than by this generator.
#
# **Optional, and it says so by being skipped.** §10: an install with no shell
# is the tray menu and is usable, and a machine with no .NET SDK is a machine
# that can still build and test everything else. So this is a custom target
# guarded on `dotnet` being found, not a hard dependency of `all`.
#
# `dotnet build` is a managed build. §2's Native AOT applies at `dotnet publish`,
# which is what packaging runs; asking for it here would put an AOT toolchain in
# the way of an ordinary `cmake --build`.

find_program(MEDIAPERCH_DOTNET dotnet)
if(NOT MEDIAPERCH_DOTNET)
    message(STATUS "dotnet not found, so the WinUI shell is skipped")
    return()
endif()

set(shell_project "${CMAKE_CURRENT_LIST_DIR}/MediaPerch.Shell.csproj")

add_custom_target(mediaperch_shell ALL
    COMMAND "${MEDIAPERCH_DOTNET}" build "${shell_project}"
            -c $<IF:$<CONFIG:Debug>,Debug,Release> --nologo
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    COMMENT "Building the WinUI shell"
    VERBATIM)
set_target_properties(mediaperch_shell PROPERTIES FOLDER "shell")
