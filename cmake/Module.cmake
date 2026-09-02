# SPDX-License-Identifier: GPL-3.0-or-later
#
# Where a module goes, said once instead of fifteen times.
#
# Every module repeated the same eight lines: no `lib` prefix, an output name,
# and an output directory per configuration because the multi-config generators
# want one. Fifteen copies of anything drift, and this one had begun to -- and
# none of them said what *kind* of module it was, which the tree now sorts by.
#
#   mediaperch_add_module(mp_dsp_gain KIND dsp SOURCES dsp_gain.cpp)
#
# The kind decides the folder on disk *and* the folder in the build output:
#
#   modules/dsp/gain/dsp_gain.cpp   ->   bin/<config>/modules/dsp/mp_dsp_gain.dll
#
# **The output layout is part of the design, not tidiness.** An install is a
# directory somebody can look at and see what is in it: which containers it
# reads, which codecs it has, which stages it can run. `modules.allow` in the
# settings file names ids, and a person deciding what to allow should be able to
# see the shape of what is there.

function(mediaperch_add_module name)
    cmake_parse_arguments(M "" "KIND" "SOURCES;LINK" ${ARGN})
    if(NOT M_KIND)
        message(FATAL_ERROR "mediaperch_add_module(${name}) needs a KIND")
    endif()
    if(NOT M_SOURCES)
        message(FATAL_ERROR "mediaperch_add_module(${name}) needs SOURCES")
    endif()

    add_library(${name} MODULE ${M_SOURCES})
    target_link_libraries(${name} PRIVATE MediaPerch::abi mediaperch_flags ${M_LINK})

    set(out "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/modules/${M_KIND}")
    set_target_properties(${name} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${out}"
        RUNTIME_OUTPUT_DIRECTORY "${out}"
        FOLDER "modules/${M_KIND}")

    # Multi-config generators ignore the plain property and want one per
    # configuration, which is the whole reason this function exists.
    foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
        string(TOUPPER "${config}" upper)
        set_target_properties(${name} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY_${upper}
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}/modules/${M_KIND}"
            RUNTIME_OUTPUT_DIRECTORY_${upper}
                "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}/modules/${M_KIND}")
    endforeach()
endfunction()
