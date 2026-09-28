# SPDX-License-Identifier: GPL-3.0-or-later
#
# **The shaders are compiled with the build, and the modules carry the
# bytecode.**
#
# The presenter compiled its colour shader from HLSL text with d3dcompiler_47
# every time it made a device -- 170 to 200 ms of a start for five entry points
# on an i7-1360P, and 215 to 250 ms before the kernels' text left it -- and the
# scaler and the LUT stage compiled theirs, 47 and 12 ms more, every time the
# chain opened them. The compiler is the same either way: fxc.exe is the
# Windows SDK's front end to the same d3dcompiler_47, and what it makes of a
# file is what the system's copy made of the same text, byte for byte. What
# moves is the moment, from every start of every player to once per build, and
# the source is still HLSL a person can read next to the comments explaining
# it, in a `.hlsl` file beside the module rather than in a string inside it.
#
#   mediaperch_add_shaders(<target> SOURCE <file.hlsl>
#                          ENTRIES <entry>:<profile> [<entry>:<profile>...]
#                          FLAGS <fxc option>...)
#
# Each entry point becomes `<file>_<entry>.cso` in `shaders/` under the target's
# build directory, which is given to the compiler as `--embed-dir` for the
# module to `#embed`. Clang lists what it embeds in its dependency output, so
# an edit to the `.hlsl` recompiles the shader and then the source that
# carries it.
#
# **No partial precision, and something refuses it.** `/Gpp` is fxc's
# "force partial precision", the flag `D3DCOMPILE_PARTIAL_PRECISION` was at run
# time: inert for shader model 4 and later, which these are, and refused here
# anyway, because a flag that is inert today is one that is not after a move
# to DXC. cmake/ShaderPrecision.cmake bans the types that would do it instead.

if(NOT WIN32)
    return()
endif()

# **The newest SDK's fxc**, which is the SDK Clang takes too (WavPack.cmake
# finds its `ksamd64.inc` the same way).
cmake_host_system_information(RESULT mediaperch_kits_root QUERY WINDOWS_REGISTRY
    "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots" VALUE KitsRoot10)
file(GLOB mediaperch_fxc_candidates "${mediaperch_kits_root}/bin/10.*/x64/fxc.exe")
if(mediaperch_fxc_candidates)
    list(SORT mediaperch_fxc_candidates COMPARE NATURAL ORDER DESCENDING)
    list(GET mediaperch_fxc_candidates 0 mediaperch_fxc_newest)
    get_filename_component(mediaperch_fxc_dir "${mediaperch_fxc_newest}" DIRECTORY)
endif()
find_program(MEDIAPERCH_FXC NAMES fxc fxc.exe HINTS "${mediaperch_fxc_dir}" NO_DEFAULT_PATH
    DOC "fxc.exe, the Windows SDK's HLSL compiler, which compiles the shaders the modules embed")
if(NOT MEDIAPERCH_FXC)
    message(FATAL_ERROR
        "fxc.exe was not found under the Windows SDK (${mediaperch_kits_root}bin/10.*/x64): "
        "the presenter and the video stages embed shaders it compiles. The SDK that "
        "gives Clang its headers has it.")
endif()
message(STATUS "HLSL compiler: ${MEDIAPERCH_FXC}")

function(mediaperch_add_shaders target)
    cmake_parse_arguments(S "" "SOURCE" "ENTRIES;FLAGS" ${ARGN})
    if(NOT S_SOURCE OR NOT S_ENTRIES)
        message(FATAL_ERROR "mediaperch_add_shaders(${target}) needs a SOURCE and ENTRIES")
    endif()
    foreach(flag IN LISTS S_FLAGS)
        if(flag MATCHES "^[/-]Gpp$")
            message(FATAL_ERROR
                "mediaperch_add_shaders(${target}): ${flag} is partial precision, and "
                "every shader here computes at single precision -- see plan.md §9.10")
        endif()
    endforeach()
    get_filename_component(source "${S_SOURCE}" ABSOLUTE)
    get_filename_component(name "${source}" NAME_WE)
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
    set(outputs "")
    foreach(pair IN LISTS S_ENTRIES)
        string(REPLACE ":" ";" parts "${pair}")
        list(LENGTH parts count)
        if(NOT count EQUAL 2)
            message(FATAL_ERROR "mediaperch_add_shaders(${target}): ${pair} is not entry:profile")
        endif()
        list(GET parts 0 entry)
        list(GET parts 1 profile)
        set(output "${dir}/${name}_${entry}.cso")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${dir}"
            COMMAND "${MEDIAPERCH_FXC}" /nologo /T ${profile} /E ${entry} ${S_FLAGS}
                    /Fo "${output}" "${source}"
            DEPENDS "${source}"
            COMMENT "fxc ${name}.hlsl ${entry} (${profile})"
            VERBATIM)
        list(APPEND outputs "${output}")
    endforeach()
    add_custom_target(${target}_shaders DEPENDS ${outputs})
    set_target_properties(${target}_shaders PROPERTIES FOLDER "shaders")
    add_dependencies(${target} ${target}_shaders)
    target_compile_options(${target} PRIVATE "--embed-dir=${dir}")
endfunction()
