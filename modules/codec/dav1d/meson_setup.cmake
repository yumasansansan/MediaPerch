# SPDX-License-Identifier: GPL-3.0-or-later
#
# `meson setup`, with the one argument that depends on whether it has run here
# before.
#
# Meson refuses a directory it has already configured and wants `--reconfigure`
# instead, while `--wipe` refuses one it has not -- so no single command line is
# right in both cases. Emptying the directory first would be, and cannot be done
# from a script ExternalProject runs *inside* that directory. Asking is what is
# left, and `meson-private/coredata.dat` is the file that answers.
#
# Driven by modules/codec/dav1d/CMakeLists.txt; see the comment there for why
# this tree has a Meson dependency at all.

foreach(required meson src build prefix)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "meson_setup.cmake needs -D${required}=")
    endif()
endforeach()

set(extra)
if(EXISTS "${build}/meson-private/coredata.dat")
    set(extra --reconfigure)
endif()

# **`--wrap-mode=nodownload` keeps the submodule clean, and it is not a
# nicety.** dav1d carries `subprojects/checkasm.wrap`, and Meson resolves wraps
# at setup time whether or not the thing that needs them is enabled -- so
# configuring downloaded checkasm and a `.wraplock` straight into
# external/dav1d, which then showed up as untracked content inside the
# submodule and would have been a network fetch during every CI configure. The
# tests that use it are off, so nothing is lost by refusing.
execute_process(
    COMMAND "${meson}" setup "${build}" "${src}"
        --backend ninja
        --buildtype release
        --default-library static
        --wrap-mode=nodownload
        --prefix "${prefix}"
        ${extra}
    RESULT_VARIABLE result)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "meson setup failed (${result}) for ${src}")
endif()

# And the lock file Meson leaves behind whether or not it downloaded anything.
# Removed rather than ignored: `submodule.<name>.ignore = untracked` in
# .gitmodules would hide this and also hide the next thing, and a submodule that
# reports itself clean when it is not is worth less than one that is.
file(REMOVE "${src}/subprojects/.wraplock")
