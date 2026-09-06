# SPDX-License-Identifier: GPL-3.0-or-later
#
# **The shader computes at single precision, and something checks it.**
#
# `cmake/CorePurity.cmake` bans half precision from `src/engine` and
# `src/player`; this bans it from the one place that legitimately touches half
# at all. The Direct3D presenter renders *into* RGBA16F when a display is the
# destination, because DXGI's flip model takes 8-bit UNORM, 10-bit UNORM and
# that and nothing above -- but everything up to the write is 32-bit, and this
# is what keeps it so.
#
#   cmake -D MEDIAPERCH_SHADER_DIR=<dir>[;<dir>...] -P cmake/ShaderPrecision.cmake
#
# **What actually reduces precision, and what only looks as though it does.**
#
# `D3DCOMPILE_PARTIAL_PRECISION` is the flag people reach for this question
# with, and on the targets here it does nothing: FXC honours it for shader model
# 2 and 3 and ignores it from 4 onward, and these compile as `vs_5_0` and
# `ps_5_0`. It is banned anyway, by a `static_assert` in the module rather than
# by this script -- a flag that is inert today is a flag that is not inert after
# a move to DXC.
#
# What *would* bite is a type. `min16float` is the shader model 6.2 spelling of
# "at least sixteen bits, and sixteen will do", and `half` is its predecessor;
# either one in an expression makes that expression's precision the hardware's
# choice. Those are the real ban, and they are banned now rather than when this
# module moves to DXC, because that is a move nobody will remember to check.
#
# The behaviour is also measured -- `tests/hdr_transfer_test.cpp` resolves one
# step of a twelve-bit code, which half cannot at that magnitude -- so this is
# the second of two locks rather than the only one. A grep says what is
# intended; the measurement says what happened.

if(NOT MEDIAPERCH_SHADER_DIR)
    message(FATAL_ERROR "MEDIAPERCH_SHADER_DIR is required")
endif()

# Types, not words: `half` on its own is English and appears in comments about
# half a refresh and half a period. A vector spelling or the SM6 name is a type
# and nothing else.
set(forbidden
    "min16float"
    "float16_t"
    "\\bhalf[234]\\b"
    "\\bhalf[234]x[234]\\b"
)

# **The flag is not in that list, and that is the division of labour.**
#
# `D3DCOMPILE_PARTIAL_PRECISION` is banned by a `static_assert` in the module,
# against the named constant the flags are actually passed as -- which checks
# the *value* rather than the spelling and cannot be fooled by a rearrangement.
# A grep for the name would only ever find the assertion that bans it, which is
# how this check first failed. Each lock holds what it can hold: the compiler
# guards the flag, this guards the types, and the twelve-bit measurement in
# `hdr_transfer_test.cpp` guards the result.

set(sources "")
foreach(dir IN LISTS MEDIAPERCH_SHADER_DIR)
    # The same rule CorePurity learnt the hard way: a directory that is not
    # there is the failure, not an empty answer. A check that cannot fail is
    # worse than no check, because it is also a claim.
    if(NOT IS_DIRECTORY "${dir}")
        message(FATAL_ERROR
            "shader precision: ${dir} is not a directory. Whoever moved it has "
            "to say so here, because this check silently passes on an empty list.")
    endif()
    file(GLOB_RECURSE found "${dir}/*.cpp" "${dir}/*.hpp" "${dir}/*.hlsl")
    if(found STREQUAL "")
        message(FATAL_ERROR "shader precision: ${dir} holds no sources")
    endif()
    list(APPEND sources ${found})
endforeach()

set(violations "")
foreach(source IN LISTS sources)
    file(STRINGS "${source}" lines)
    set(line_number 0)
    foreach(line IN LISTS lines)
        math(EXPR line_number "${line_number} + 1")
        # **Comments are exempt, and finding that out was the first thing this
        # check did.** The file explains at length why it does not use
        # `min16float`, and a check that could not tell an explanation from a
        # use would make the explanation impossible to write -- which is how a
        # rule ends up undocumented.
        if(line MATCHES "^[ 	]*(//|[*]|/[*])")
            continue()
        endif()
        foreach(pattern IN LISTS forbidden)
            if(line MATCHES "${pattern}")
                list(APPEND violations "${source}:${line_number}: ${line}")
            endif()
        endforeach()
    endforeach()
endforeach()

list(LENGTH sources source_count)

if(violations)
    message("the shader would not be computing at single precision any more:")
    foreach(violation IN LISTS violations)
        message("  ${violation}")
    endforeach()
    message(FATAL_ERROR
        "RGBA16F is what a flip-model swap chain takes and is the last write. "
        "Everything before it is 32-bit on purpose -- see plan.md §9.10.")
endif()

message(STATUS "shader precision: ${source_count} files, nothing computes in half")
