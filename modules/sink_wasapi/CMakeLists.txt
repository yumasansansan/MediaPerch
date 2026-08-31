# SPDX-License-Identifier: GPL-3.0-or-later
#
# The WASAPI sink, as a module: a DLL with one exported symbol, loaded at run
# time. It links the ABI header and nothing of ours -- not the core, not the
# head. A module that could reach into the host would not be a module.

# A leaf: MpFormat in, WAVEFORMATEXTENSIBLE out, and nothing else. Both the sink
# module and the loopback verifier need it, and duplicating a format converter
# between the two ends of a bit-exactness test is the one place duplication would
# be actively dangerous -- a drift between them would show up as a mismatch that
# is not there. Depends on the ABI header and Windows, never on the core or the
# head, so a module linking it does not gain a route to the host.
add_library(mediaperch_waveformat STATIC
    wave_format.cpp
    wave_format.hpp
    win_headers.hpp
)
target_include_directories(mediaperch_waveformat PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(mediaperch_waveformat PUBLIC MediaPerch::abi PRIVATE mediaperch_flags)
set_target_properties(mediaperch_waveformat PROPERTIES FOLDER "modules")

add_library(mp_sink_wasapi MODULE
    sink_wasapi.cpp
)

target_link_libraries(mp_sink_wasapi PRIVATE MediaPerch::abi mediaperch_waveformat
                                             mediaperch_flags)
target_link_libraries(mp_sink_wasapi PRIVATE ole32 uuid)

# Beside the executable, which is where the head looks for it.
set_target_properties(mp_sink_wasapi PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mp_sink_wasapi"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    FOLDER "modules")

foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
    string(TOUPPER "${config}" upper)
    set_target_properties(mp_sink_wasapi PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}"
        RUNTIME_OUTPUT_DIRECTORY_${upper} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()
