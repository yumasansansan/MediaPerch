# SPDX-License-Identifier: GPL-3.0-or-later
#
# Which decoder reads which format, measured rather than remembered.
#
# **A coverage table nobody generates is a coverage table that is wrong.** It is
# written when a module is added and then quietly outlives the truth: a decoder
# gains a format, another loses one, and the README goes on claiming what was
# true the day somebody typed it. So this builds one file per format, shows it to
# every decoder in the tree, and writes down what each of them actually did.
#
# The interesting column is not "did it open". It is **whether the bytes are the
# file's own**. A lossless format decoded by two modules can come out as the
# integers that were encoded or as floats that merely sound the same, and which
# of those you got is the whole subject of this program -- so the cell says
# `exact` when the PCM is identical to the audio that went into the encoder, and
# names the sample type when it is not.
#
# Usage:
#   cmake -D MEDIAPERCH_PROBE=<probe> -D MEDIAPERCH_WORK=<dir>
#         -D MEDIAPERCH_README=<path> [-D MEDIAPERCH_WRITE=ON]
#         [-D MEDIAPERCH_REQUIRE_FFMPEG=ON] -P cmake/FormatMatrix.cmake
#
# Without `MEDIAPERCH_WRITE` it *checks* the table in the README and fails when
# it has drifted, which is what makes the one in the README real.
cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED MEDIAPERCH_PROBE OR NOT EXISTS "${MEDIAPERCH_PROBE}")
    message(FATAL_ERROR "MEDIAPERCH_PROBE must point at mediaperch-probe")
endif()
if(NOT DEFINED MEDIAPERCH_WORK)
    message(FATAL_ERROR "MEDIAPERCH_WORK must name a directory to work in")
endif()
if(NOT DEFINED MEDIAPERCH_README OR NOT EXISTS "${MEDIAPERCH_README}")
    message(FATAL_ERROR "MEDIAPERCH_README must point at the README to fill in")
endif()

find_program(FFMPEG ffmpeg)
if(NOT FFMPEG)
    # Locally a skip, in CI a failure: a check that silently does not run is
    # worse than no check.
    if(MEDIAPERCH_REQUIRE_FFMPEG)
        message(FATAL_ERROR "ffmpeg is required to build the format corpus and was not found")
    endif()
    message(STATUS "ffmpeg not found -- skipping the format matrix")
    return()
endif()

set(W "${MEDIAPERCH_WORK}")
file(MAKE_DIRECTORY "${W}")

# --------------------------------------------------------------- the corpus

# A short signal with something in every channel. Two seconds is plenty: this
# measures which decoder reads what, not how well, which is
# cmake/DecodeQuality.cmake's job.
# `format` is FFmpeg's sample format and `codec` its PCM encoder, which are not
# the same word: there is no `s24` sample format, only `pcm_s24le` fed from
# `s32`, and float is `flt` rather than `f32`.
function(make_source out channels rate format codec)
    if(EXISTS "${out}")
        return()
    endif()
    set(layout "${channels}c")
    if(channels EQUAL 1)
        set(layout mono)
    elseif(channels EQUAL 2)
        set(layout stereo)
    elseif(channels EQUAL 6)
        set(layout 5.1)
    endif()
    execute_process(
        COMMAND "${FFMPEG}" -hide_banner -loglevel error
                -f lavfi -i "anoisesrc=sample_rate=${rate}:duration=2:amplitude=0.25:color=pink:seed=11"
                -af "aformat=sample_fmts=${format}:channel_layouts=${layout}"
                -c:a "${codec}" "${out}" -y
        RESULT_VARIABLE status ERROR_VARIABLE why)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "could not build ${out}: ${why}")
    endif()
endfunction()

# The decoders, in the order §7 of the plan lists them. A fixed list rather than
# whatever loaded, because a decoder that is *missing* is a column of dashes and
# that is information too.
set(decoders native flac ogg mp3 aac alac mf ffmpeg)

set(rows "")      # "label|file|reference-file-or-NONE"
set(row_notes "")

# Adds a row. `reference` is the uncompressed file that went into the encoder,
# or NONE for a lossy format, where no decoder can be bit-exact and pretending
# otherwise would be the wrong claim.
function(add_row label out reference)
    execute_process(
        COMMAND "${FFMPEG}" -hide_banner -loglevel error -i "${SOURCE}" ${ARGN} "${out}" -y
        RESULT_VARIABLE status ERROR_VARIABLE why)
    if(NOT status EQUAL 0)
        # Not fatal. FFmpeg builds differ in which encoders they carry, and a
        # row this machine cannot make is a row this machine cannot measure --
        # which is honest to say and wrong to guess at.
        message(STATUS "skipping ${label}: this ffmpeg would not encode it")
        set(missing "${missing};${label}" PARENT_SCOPE)
        return()
    endif()
    list(APPEND rows "${label}|${out}|${reference}")
    set(rows "${rows}" PARENT_SCOPE)
endfunction()

set(missing "")

# **This corpus stops where FFmpeg can be trusted**, and that boundary is at 32
# bits: `docs/formats.md` records that its FLAC encoder writes 24 bits when asked
# for 32, and integer lossless at 32 bits is where its encoders are least
# dependable generally. So no 32-bit lossless row is generated here.
#
# Those rows are not unmeasured -- they are measured with the reference encoders
# and written down in docs/formats.md, under *32 bits, measured by hand*. What
# they are not is reproducible from a checkout, and a generated table that
# quietly depended on which tools a machine happened to have would be a
# generated table that means something different on every machine.
make_source("${W}/s16_44100_2.wav" 2 44100 s16 pcm_s16le)
make_source("${W}/s24_96000_2.wav" 2 96000 s32 pcm_s24le)
make_source("${W}/f32_48000_2.wav" 2 48000 flt pcm_f32le)
make_source("${W}/s16_48000_6.wav" 6 48000 s16 pcm_s16le)

# ---- uncompressed, which is its own reference. WAV is PCM: FFmpeg is only
# writing samples it was handed, so there is no encoder here to distrust.
list(APPEND rows "WAV, 16-bit 44.1 kHz stereo|${W}/s16_44100_2.wav|${W}/s16_44100_2.wav")
list(APPEND rows "WAV, 24-bit 96 kHz stereo|${W}/s24_96000_2.wav|${W}/s24_96000_2.wav")
list(APPEND rows "WAV, 32-bit float 48 kHz stereo|${W}/f32_48000_2.wav|${W}/f32_48000_2.wav")
list(APPEND rows "WAV, 16-bit 5.1 at 48 kHz|${W}/s16_48000_6.wav|${W}/s16_48000_6.wav")

set(SOURCE "${W}/s16_44100_2.wav")
add_row("FLAC, 16-bit 44.1 kHz stereo" "${W}/a.flac" "${SOURCE}" -c:a flac)
add_row("ALAC, 16-bit 44.1 kHz stereo" "${W}/a_alac.m4a" "${SOURCE}" -c:a alac)
add_row("MP3, 44.1 kHz stereo 256k" "${W}/a.mp3" NONE -c:a libmp3lame -b:a 256k)
add_row("AAC-LC in M4A, 44.1 kHz stereo" "${W}/a.m4a" NONE -c:a aac -b:a 192k)
add_row("AAC-LC raw ADTS, 44.1 kHz stereo" "${W}/a.aac" NONE -c:a aac -b:a 192k -f adts)
add_row("Vorbis in Ogg, 44.1 kHz stereo" "${W}/a.ogg" NONE -c:a libvorbis -q:a 6)
add_row("Opus in Ogg, 48 kHz stereo" "${W}/a.opus" NONE -c:a libopus -b:a 192k)
add_row("WMA v2, 44.1 kHz stereo" "${W}/a.wma" NONE -c:a wmav2 -b:a 192k)
add_row("WavPack, 16-bit 44.1 kHz stereo" "${W}/a.wv" "${SOURCE}" -c:a wavpack)
add_row("FLAC in Matroska, 16-bit stereo" "${W}/a.mka" "${SOURCE}" -c:a flac -f matroska)

set(SOURCE "${W}/s24_96000_2.wav")
add_row("FLAC, 24-bit 96 kHz stereo" "${W}/b.flac" "${SOURCE}" -c:a flac)
add_row("ALAC, 24-bit 96 kHz stereo" "${W}/b_alac.m4a" "${SOURCE}" -c:a alac)

set(SOURCE "${W}/s16_48000_6.wav")
add_row("FLAC, 16-bit 5.1 at 48 kHz" "${W}/d.flac" "${SOURCE}" -c:a flac)
add_row("ALAC, 16-bit 5.1 at 48 kHz" "${W}/d_alac.m4a" "${SOURCE}" -c:a alac)

# ---------------------------------------------------------------- the asking

# Which decoders claim a file, by the same probe the resolution rules use.
# Forcing a decoder that would have declined still measures something -- that it
# copes rather than crashes -- but it is not coverage, and a table that could not
# tell the two apart would overstate every row.
function(who_claims out file)
    execute_process(
        COMMAND "${MEDIAPERCH_PROBE}" claims --file "${file}"
        RESULT_VARIABLE status OUTPUT_VARIABLE said)
    set(claimed "")
    if(status EQUAL 0)
        string(REPLACE "\n" ";" lines "${said}")
        foreach(line IN LISTS lines)
            if(line MATCHES "^decode_([a-z0-9_]+) ")
                list(APPEND claimed "${CMAKE_MATCH_1}")
            endif()
        endforeach()
    endif()
    set(${out} "${claimed}" PARENT_SCOPE)
endfunction()

# What one decoder made of one file: `exact`, a sample type, or nothing.
function(ask out_cell file decoder reference)
    execute_process(
        COMMAND "${MEDIAPERCH_PROBE}" decode --file "${file}" --decoder "${decoder}"
        RESULT_VARIABLE status OUTPUT_VARIABLE said ERROR_VARIABLE complained)
    if(NOT status EQUAL 0)
        set(${out_cell} "" PARENT_SCOPE)
        return()
    endif()
    string(REGEX MATCH "format +[0-9]+ Hz / [0-9]+ ch / ([A-Za-z0-9_]+)" _ "${said}")
    set(type "${CMAKE_MATCH_1}")
    string(REGEX MATCH "sha256 +([0-9a-f]+)" _ "${said}")
    set(hash "${CMAKE_MATCH_1}")
    # A decoder that opened the file but was forced past its own judgement can
    # still have been the wrong one; what it produced is what is recorded.
    if(NOT reference STREQUAL "NONE" AND DEFINED REFERENCE_HASH_${reference})
        if(hash STREQUAL "${REFERENCE_HASH_${reference}}")
            set(${out_cell} "exact" PARENT_SCOPE)
            return()
        endif()
    endif()
    set(${out_cell} "${type}" PARENT_SCOPE)
endfunction()

# The reference hash for a row is the PCM of the file that went into the
# encoder, read by whichever decoder opens it -- they agree on WAV, which the
# first row of the table is there to show.
set(reference_files "")
foreach(row IN LISTS rows)
    string(REPLACE "|" ";" parts "${row}")
    list(GET parts 2 reference)
    if(NOT reference STREQUAL "NONE")
        list(APPEND reference_files "${reference}")
    endif()
endforeach()
list(REMOVE_DUPLICATES reference_files)
foreach(reference IN LISTS reference_files)
    execute_process(
        COMMAND "${MEDIAPERCH_PROBE}" decode --file "${reference}"
        RESULT_VARIABLE status OUTPUT_VARIABLE said)
    if(status EQUAL 0)
        string(REGEX MATCH "sha256 +([0-9a-f]+)" _ "${said}")
        set(REFERENCE_HASH_${reference} "${CMAKE_MATCH_1}")
    endif()
endforeach()

# --------------------------------------------------------------- the table

set(header "| Format |")
set(rule "|---|")
foreach(decoder IN LISTS decoders)
    string(APPEND header " `${decoder}` |")
    string(APPEND rule "---|")
endforeach()

set(table "${header}\n${rule}\n")
set(exact_cells 0)
set(open_cells 0)

foreach(row IN LISTS rows)
    string(REPLACE "|" ";" parts "${row}")
    list(GET parts 0 label)
    list(GET parts 1 file)
    list(GET parts 2 reference)

    # Which one the resolution rules pick when nobody says. It is marked in the
    # table because "several read it" and "this is the one you get" are
    # different facts and only one of them is what happens.
    execute_process(
        COMMAND "${MEDIAPERCH_PROBE}" decode --file "${file}"
        RESULT_VARIABLE status OUTPUT_VARIABLE said)
    set(chosen "")
    if(status EQUAL 0)
        string(REGEX MATCH "decoder +decode_([a-z0-9_]+)" _ "${said}")
        set(chosen "${CMAKE_MATCH_1}")
    endif()

    who_claims(claimed "${file}")

    string(APPEND table "| ${label} |")
    foreach(decoder IN LISTS decoders)
        ask(cell "${file}" "${decoder}" "${reference}")
        if(cell STREQUAL "")
            string(APPEND table " — |")
        else()
            math(EXPR open_cells "${open_cells} + 1")
            if(cell STREQUAL "exact")
                math(EXPR exact_cells "${exact_cells} + 1")
            endif()
            if(decoder STREQUAL chosen)
                string(APPEND table " **${cell}** |")
            elseif(NOT decoder IN_LIST claimed)
                # It did not claim the file. It was made to try, and this is
                # what happened -- which is robustness, not coverage.
                string(APPEND table " (${cell}) |")
            else()
                string(APPEND table " ${cell} |")
            endif()
        endif()
    endforeach()
    string(APPEND table "\n")
endforeach()

list(LENGTH rows how_many)
string(APPEND table "\n")
string(APPEND table "`exact` is PCM identical to the audio that went into the encoder; a\n")
string(APPEND table "sample type is a decoder that read the file and produced that instead; `—` is one\n")
string(APPEND table "that declined. **Bold** is the decoder the resolution rules pick when nobody says;\n")
string(APPEND table "`(brackets)` is one that did not claim the file and was forced to try anyway, which\n")
string(APPEND table "measures that it copes rather than that it covers. Lossy rows have no `exact` to\n")
string(APPEND table "reach: the encoder threw those bytes away.\n")
string(APPEND table "\nNo 32-bit lossless row is generated: FFmpeg cannot encode one to be trusted.\n")
string(APPEND table "Those are measured with the reference encoders and written down in\n")
string(APPEND table "[docs/formats.md](docs/formats.md) instead.\n")
if(missing)
    string(REPLACE ";" ", " pretty "${missing}")
    string(STRIP "${pretty}" pretty)
    string(APPEND table "\nNot measured here, because this FFmpeg would not encode them: ${pretty}.\n")
endif()

message(STATUS "${how_many} formats, ${open_cells} decoder-format pairs that opened, "
               "${exact_cells} of them bit-exact")

# ------------------------------------------------------- into the README

set(begin "<!-- formats:begin -->")
set(end "<!-- formats:end -->")
file(READ "${MEDIAPERCH_README}" readme)
string(FIND "${readme}" "${begin}" at_begin)
string(FIND "${readme}" "${end}" at_end)
if(at_begin EQUAL -1 OR at_end EQUAL -1 OR at_end LESS at_begin)
    message(FATAL_ERROR "the README has no ${begin} / ${end} pair to fill in")
endif()
string(LENGTH "${begin}" begin_length)
math(EXPR head_end "${at_begin} + ${begin_length}")
string(SUBSTRING "${readme}" 0 ${head_end} head)
string(SUBSTRING "${readme}" ${at_end} -1 tail)
set(filled "${head}\n${table}${tail}")

if(MEDIAPERCH_WRITE)
    file(WRITE "${MEDIAPERCH_README}" "${filled}")
    message(STATUS "wrote the matrix into ${MEDIAPERCH_README}")
    return()
endif()

if(NOT filled STREQUAL readme)
    file(WRITE "${W}/README.expected.md" "${filled}")
    message(FATAL_ERROR
        "the format matrix in the README is not what this machine measures.\n"
        "What it should say is in ${W}/README.expected.md, and\n"
        "  cmake -D MEDIAPERCH_WRITE=ON ... -P cmake/FormatMatrix.cmake\n"
        "writes it. A table nobody generates is a table that is wrong.")
endif()
message(STATUS "the format matrix in the README is what this machine measures")
