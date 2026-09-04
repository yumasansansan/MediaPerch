# SPDX-License-Identifier: GPL-3.0-or-later
#
# ASIO, and where the Steinberg SDK enters the build.
#
# **Headers only.** The SDK's `asio.cpp` is a C wrapper that keeps one global
# driver pointer, and `host/asiolist.cpp` enumerates the registry with fixed
# `char[32]` buffers; this module wants the raw `IASIO` and reads its own
# registry, so neither is compiled. Nothing is linked -- there is no ASIO
# library, only an interface and a `CoCreateInstance`.
#
# The licence is Steinberg's dual one: their ASIO licence, **or GPLv3**, which
# they added in October 2025 and which a GPL-3.0-or-later program may take. See
# docs/building.md for why the pin is a third-party copy rather than a tag.

if(NOT EXISTS "${CMAKE_SOURCE_DIR}/external/asio/common/iasiodrv.h")
    message(WARNING
        "external/asio is missing -- run `git submodule update --init`. "
        "Skipping sink_asio; the tree still builds without it, and WASAPI is "
        "the sink either way.")
    return()
endif()

mediaperch_add_module(mp_sink_asio
    KIND sink
    SOURCES sink_asio.cpp)

# `-isystem`, not `-I`: these are somebody else's headers and their warnings are
# not this tree's to fix. asio.h has a `char[512]` in a struct and casts that
# would not pass our warning set.
target_include_directories(mp_sink_asio SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/external/asio/common")
target_link_libraries(mp_sink_asio PRIVATE ole32)
