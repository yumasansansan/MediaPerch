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

# **Where the engine is, for a shell that was not installed beside it.** The
# shell starts `mediaperchd.exe` when nothing is listening and stops the one
# it started when its window closes, and it looks for the engine beside its
# own executable, which is where an install puts it. In this tree the two are
# built into different directories, and building the shell into the engine's
# was tried and is not an option: `dotnet build -o` leaves a WinUI app that
# cannot find its own XAML (`Cannot locate resource from ms-appx:///`). So
# the build writes the engine's path into a file beside the shell instead,
# and the shell reads it when it is there. An install does not ship it.
#
# The shell has two configurations where this tree has three, so the file for
# a RelWithDebInfo build names the Release engine -- `file(GENERATE)` refuses
# to write one path with two contents, and that is the honest mapping anyway:
# the shell built for RelWithDebInfo *is* the Release shell.
set(shell_config "$<IF:$<CONFIG:Debug>,Debug,Release>")
set(shell_out
    "${CMAKE_CURRENT_LIST_DIR}/bin/x64/${shell_config}/net10.0-windows10.0.26100.0/win-x64")
file(GENERATE OUTPUT "${shell_out}/mediaperch.engine"
    CONTENT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${shell_config}/mediaperchd.exe")
set_target_properties(mediaperch_shell PROPERTIES FOLDER "shell")
