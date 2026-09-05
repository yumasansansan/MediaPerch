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
# **Every module that reads a file**, which is now every demuxer and only them:
# MP_KIND_DECODER is gone and so are the eight modules that used it.
#
# The heading drops the `demux_` that all of them share.
set(decoders demux_wav demux_flac demux_mpa demux_adts demux_dsd demux_wavpack demux_mp4 demux_ogg
             demux_mkv demux_ffmpeg demux_mf)

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
add_row("ALAC in Matroska, 16-bit stereo" "${W}/a_alac.mka" "${SOURCE}" -c:a alac -f matroska)
add_row("PCM in Matroska, 16-bit stereo" "${W}/a_pcm.mka" "${SOURCE}" -c:a pcm_s16le -f matroska)
add_row("Vorbis in Matroska, 44.1 kHz stereo" "${W}/a_vorbis.mka" NONE -c:a libvorbis -q:a 6 -f matroska)
add_row("Opus in WebM, 48 kHz stereo" "${W}/a_opus.webm" NONE -c:a libopus -b:a 192k -f webm)

set(SOURCE "${W}/s24_96000_2.wav")
add_row("FLAC, 24-bit 96 kHz stereo" "${W}/b.flac" "${SOURCE}" -c:a flac)
add_row("ALAC, 24-bit 96 kHz stereo" "${W}/b_alac.m4a" "${SOURCE}" -c:a alac)

set(SOURCE "${W}/s16_48000_6.wav")
add_row("FLAC, 16-bit 5.1 at 48 kHz" "${W}/d.flac" "${SOURCE}" -c:a flac)
add_row("ALAC, 16-bit 5.1 at 48 kHz" "${W}/d_alac.m4a" "${SOURCE}" -c:a alac)

# ---- DSD, which is the one format here whose corpus is committed rather than
# generated. **Nothing encodes DSD** -- FFmpeg reads DSF and DSDIFF and cannot
# write either, and there is no reference encoder anywhere -- so the two files
# come from `tools/make_dsd.py`, a sigma-delta modulator, and live in the tree.
# Their provenance is that script and the check it also carries.
#
# The reference is NONE, and that is not a shrug. `exact` in this table means
# the PCM equals the audio that went into the encoder, and DSD had no PCM go
# into it: the file's bits *are* the waveform. What must be true instead is
# that those bits come out of DoP unaltered, which no column here could show
# and which `tools/make_dsd.py --check` measures directly.
set(DSD_DATA "${CMAKE_CURRENT_LIST_DIR}/../tests/data/dsd")
if(EXISTS "${DSD_DATA}/stereo_dsd64.dsf")
    list(APPEND rows "DSD64 in DSF, stereo|${DSD_DATA}/stereo_dsd64.dsf|NONE")
    list(APPEND rows "DSD64 in DSDIFF, stereo|${DSD_DATA}/stereo_dsd64.dff|NONE")
    # The same DSD compressed by WavPack's own encoder -- the reference one,
    # the way the ALAC rows in docs/formats.md come from Apple's. What makes
    # this row worth a line of its own is that it must produce the *same bytes*
    # as the two above: three containers, one audio, and the compression in the
    # third is lossless or it is not WavPack.
    list(APPEND rows "DSD64 in WavPack, stereo|${DSD_DATA}/stereo_dsd64.wv|NONE")
endif()

# ---------------------------------------------------------------- the asking

# Which modules claim a file, by the same probe the resolution rules use.
# Forcing a module that would have declined still measures something -- that it
# copes rather than crashes -- but it is not coverage, and a table that could not
# tell the two apart would overstate every row.
#
# `claims` lists the demuxers that scored, one per line, with the streams each
# one found indented beneath it. Only the module lines are read here.
function(who_claims out file)
    execute_process(
        COMMAND "${MEDIAPERCH_PROBE}" claims --file "${file}"
        RESULT_VARIABLE status OUTPUT_VARIABLE said)
    set(claimed "")
    if(status EQUAL 0)
        string(REPLACE "\n" ";" lines "${said}")
        foreach(line IN LISTS lines)
            if(line MATCHES "^(demux_[a-z0-9_]+) +[0-9]")
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
    # A DoP stream is 24-bit frames and is not PCM, and a cell that said
    # `S24_PACKED` would be naming the container while hiding the thing that
    # matters about it. The encoding is what a reader of this table wants.
    if(said MATCHES "format +[0-9]+ Hz / [0-9]+ ch / [A-Za-z0-9_]+[^
]* / (DoP|IEC61937)")
        set(type "${CMAKE_MATCH_1}")
    endif()
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
    string(REGEX REPLACE "^demux_" "" heading "${decoder}")
    string(APPEND header " `${heading}` |")
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
        string(REGEX MATCH "decoder +([a-z0-9_]+)" _ "${said}")
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
string(APPEND table "Every column is a demuxer -- `wav` is `demux_wav` -- and the codec each one\n")
string(APPEND table "hands its packets to is looked up rather than tried: `mediaperch-probe claims`\n")
string(APPEND table "prints the pair for any file.\n\n")
string(APPEND table "`exact` is PCM identical to the audio that went into the encoder; a\n")
string(APPEND table "sample type is a module that read the file and produced that instead; `—` is one\n")
string(APPEND table "that declined. **Bold** is the module the resolution rules pick when nobody says;\n")
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

# **The last two columns delegate, and a second machine is entitled to differ
# there.** `ffmpeg` is whatever FFmpeg is installed and `mf` is whatever Media
# Foundation has -- a desktop SKU and a server SKU do not carry the same audio
# codecs. Everything to their left is code in this repository and must agree
# everywhere. `MEDIAPERCH_MATRIX_STRICT` is on where the table is written and
# off in CI, and this is what off means: those two cells are blanked on both
# sides before the comparison, so a difference in them is not a failure and a
# difference anywhere else still is.
function(mediaperch_comparable text out)
    if(MEDIAPERCH_MATRIX_STRICT)
        set(${out} "${text}" PARENT_SCOPE)
        return()
    endif()
    set(result "")
    string(REPLACE "\n" ";;;" as_list "${text}")
    string(REPLACE ";;;" ";" as_list "${as_list}")
    foreach(line IN LISTS as_list)
        if(line MATCHES "^\\|.*\\|$" AND NOT line MATCHES "^\\|-")
            string(REGEX REPLACE " [^|]*\\| [^|]*\\|$" " ~| ~|" line "${line}")
        endif()
        string(APPEND result "${line}\n")
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

mediaperch_comparable("${filled}" filled_comparable)
mediaperch_comparable("${readme}" readme_comparable)

if(NOT filled_comparable STREQUAL readme_comparable)
    file(WRITE "${W}/README.expected.md" "${filled}")

    # **Say which rows**, because this is the one check whose failure can be
    # reproduced only on the machine that saw it. "The table differs" sent a
    # person to a file they do not have; the rows that differ fit in the log.
    string(REGEX MATCHALL "\\|[^\n]*\\|" measured_rows "${filled}")
    string(REGEX MATCHALL "\\|[^\n]*\\|" written_rows "${readme}")
    list(LENGTH measured_rows measured_count)
    list(LENGTH written_rows written_count)
    set(differences "")
    set(shown 0)
    if(measured_count EQUAL written_count)
        math(EXPR last "${measured_count} - 1")
        foreach(i RANGE 0 ${last})
            list(GET measured_rows ${i} measured_row)
            list(GET written_rows ${i} written_row)
            if(NOT measured_row STREQUAL written_row AND shown LESS 8)
                string(APPEND differences "\n  README   ${written_row}"
                                          "\n  measured ${measured_row}\n")
                math(EXPR shown "${shown} + 1")
            endif()
        endforeach()
    else()
        set(differences "\n  the table has ${measured_count} rows here and "
                        "${written_count} in the README\n")
    endif()
    if(differences STREQUAL "")
        set(differences "\n  the rows are the same; the difference is in the prose "
                        "around them\n")
    endif()

    message(FATAL_ERROR
        "the format matrix in the README is not what this machine measures.\n"
        "${differences}"
        "\nWhat it should say is in ${W}/README.expected.md, and\n"
        "  cmake -D MEDIAPERCH_WRITE=ON ... -P cmake/FormatMatrix.cmake\n"
        "writes it. A table nobody generates is a table that is wrong.")
endif()
message(STATUS "the format matrix in the README is what this machine measures")
