# SPDX-License-Identifier: GPL-3.0-or-later
#
# VST3, and how little of the SDK a host actually needs.
#
# **`pluginterfaces` only.** The full vst3sdk is four repositories -- base,
# pluginterfaces, public.sdk and vstgui -- and a host normally takes at least
# three of them for the module loader, the `HostApplication`, the attribute
# list and the process-data plumbing. That is around forty thousand lines to do
# what `vst3_host.cpp` and `vst3_hostapp.hpp` do in eight hundred, and it
# arrives with a build system that wants to own the tree's configuration.
#
# So this takes the interface headers, their four .cpp files, and nothing else.
# `vst3_iids.cpp` is the one file it has to write itself: a VST3 header declares
# `static const FUID iid` and something must define it, which in the SDK is
# `public.sdk/source/vst/vstinitiids.cpp`.
#
# The licence is **MIT since October 2025**, which is the reason this module can
# exist at all -- the previous one was Steinberg's proprietary VST3 licence,
# incompatible with GPLv3 in both directions.

set(vst3 "${CMAKE_SOURCE_DIR}/external/vst3sdk")

if(NOT EXISTS "${vst3}/pluginterfaces/base/funknown.h")
    message(WARNING
        "external/vst3sdk/pluginterfaces is missing -- run `git submodule "
        "update --init`. Skipping dsp_vst3; the tree still builds without it, "
        "and every other stage in modules/dsp is unaffected.")
    return()
endif()

if(NOT WIN32)
    # The loader is `LoadLibrary` and a `Contents/x86_64-win` path. Nothing else
    # in the module is platform-specific, and the day this tree has a second
    # platform this is the file that says what has to be written.
    return()
endif()

# **The four SDK sources every VST3 host and plugin here needs, compiled once
# and as somebody else's code.** They used to be listed in three targets --
# this module, the test plugin and the test binary -- and compiled with this
# tree's warnings each time. MEDIAPERCH_EXTERNAL is the whole of their
# treatment: see mediaperch_flags_everywhere in cmake/CompilerOptions.cmake.
#
# `SYSTEM` on the include path, as before: somebody else's headers, and their
# warnings are not this tree's to fix. The VST3 headers have unscoped enums, a
# `char[128]` in a struct and casts that this project's warning set would
# refuse. PUBLIC, so that whoever links the SDK gets its headers the same way.
#
# `funknown.cpp` calls CoCreateGuid, which is the only thing in the SDK that
# needs a library at all. The SDK's own switches: `DEVELOPMENT` off and
# `RELEASE` on turns off its internal assertions and its allocation tracking,
# which are for people writing plugins rather than for a host loading them.
add_library(mediaperch_vst3_sdk STATIC
    "${vst3}/pluginterfaces/base/funknown.cpp"
    "${vst3}/pluginterfaces/base/coreiids.cpp"
    "${vst3}/pluginterfaces/base/ustring.cpp"
    "${vst3}/pluginterfaces/base/conststringtable.cpp")
target_include_directories(mediaperch_vst3_sdk SYSTEM PUBLIC "${vst3}")
target_link_libraries(mediaperch_vst3_sdk PUBLIC ole32)
target_compile_definitions(mediaperch_vst3_sdk PUBLIC
    "$<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>")
set_target_properties(mediaperch_vst3_sdk PROPERTIES
    MEDIAPERCH_EXTERNAL TRUE FOLDER "external")

mediaperch_add_module(mp_dsp_vst3
    KIND dsp
    SOURCES
        dsp_vst3.cpp
        vst3_host.cpp
        vst3_iids.cpp
    LINK mediaperch_vst3_sdk)
target_include_directories(mp_dsp_vst3 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
