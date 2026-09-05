# SPDX-License-Identifier: GPL-3.0-or-later
#
# Matroska and WebM, on libebml and libmatroska from the Matroska project.
#
# Two submodules for one module, because that is how upstream ships it: libebml
# is the encoding and libmatroska is the schema written on top of it, and the
# second calls `find_package(EBML)` for the first. cmake/FindEBML.cmake answers
# with the in-tree target, which is why libebml is added here first -- the same
# arrangement as libogg and libvorbis one directory over.
#
# Both are LGPL 2.1-or-later, which a GPL-3.0 program may link. See docs/plan.md
# for the licence table.

# utfcpp is libebml's, not this module's: a header-only UTF-8 library it asks
# for with find_package and would otherwise download at configure time.
# cmake/Findutf8cpp.cmake answers with the submodule.
set(_mkv_deps
    "${CMAKE_SOURCE_DIR}/external/libebml/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/external/libmatroska/CMakeLists.txt"
    "${CMAKE_SOURCE_DIR}/external/utfcpp/CMakeLists.txt")
foreach(_dep IN LISTS _mkv_deps)
    if(NOT EXISTS "${_dep}")
        message(WARNING
            "${_dep} is missing -- run `git submodule update --init`. "
            "Skipping demux_mkv; the tree still builds without it.")
        return()
    endif()
endforeach()

# Neither ships install rules this build wants, and libmatroska's would fail the
# same way opusfile's did: an export set cannot reference a target that is not
# itself exported. Nothing in this subtree is installed.
set(CMAKE_SKIP_INSTALL_RULES ON)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(DISABLE_PKGCONFIG ON CACHE BOOL "" FORCE)
set(DISABLE_CMAKE_CONFIG ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory("${CMAKE_SOURCE_DIR}/external/libebml"
                 "${CMAKE_BINARY_DIR}/external/libebml" EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_SOURCE_DIR}/external/libmatroska"
                 "${CMAKE_BINARY_DIR}/external/libmatroska" EXCLUDE_FROM_ALL)

mediaperch_add_module(mp_demux_mkv
    KIND demux
    SOURCES demux_mkv.cpp
    LINK matroska ebml)

# Neither is ours, and neither is held to our warning set.
foreach(_ext IN ITEMS ebml matroska)
    if(TARGET ${_ext})
        if(MSVC)
            target_compile_options(${_ext} PRIVATE /W0)
        else()
            target_compile_options(${_ext} PRIVATE -w)
        endif()
        set_target_properties(${_ext} PROPERTIES FOLDER "external")
    endif()
endforeach()

# What a bit depth means, shared rather than copied a seventh time.
target_link_libraries(mp_demux_mkv PRIVATE mediaperch_pcm_format)

# The host's log, shared rather than copied a thirteenth time.
target_link_libraries(mp_demux_mkv PRIVATE mediaperch_module_log)
