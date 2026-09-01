# SPDX-License-Identifier: GPL-3.0-or-later
#
# Decoders, measured against the audio that was encoded.
#
# Every other check in this tree compares one decoder with another. That answers
# "do these agree" and cannot answer "are they both wrong", so this one holds a
# decode against the uncompressed file that went into the encoder -- the only
# reference that is outside every decoder.
#
# What that can show is narrower than it sounds and more useful than it sounds.
# It cannot rank two decoders on fidelity: the encoder threw information away on
# purpose, and its loss is common to both and millions of times larger than the
# difference between them. What it can do is check the things no
# decoder-to-decoder comparison can -- length, alignment, which channel came out
# of which speaker, and a fidelity floor that assumes nothing about FFmpeg.
#
# The test signal is a sweep plus white noise, one distinct pair per channel:
#
#   * the sweep, because a signal has to be able to locate itself. A steady tone
#     correlates with itself once per period and every peak is a plausible
#     answer -- docs/plan.md records the day that cost. A sweep's correlation is
#     a spike, and the measurement reports how far above its surroundings that
#     spike stands so a future change to the signal fails loudly rather than
#     quietly measuring nothing.
#   * the noise, because it is what makes channels tell each other apart, and
#     because noise is what drives an AAC encoder to substitute noise -- the
#     code path that was silently wrong for a week. A sweep alone never reaches
#     it.
#
# Usage:
#   cmake -D MEDIAPERCH_PROBE=<probe> -D MEDIAPERCH_WORK=<dir>
#         [-D MEDIAPERCH_REQUIRE_FFMPEG=ON] -P cmake/DecodeQuality.cmake
cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED MEDIAPERCH_PROBE OR NOT EXISTS "${MEDIAPERCH_PROBE}")
    message(FATAL_ERROR "MEDIAPERCH_PROBE must point at mediaperch-probe")
endif()
if(NOT DEFINED MEDIAPERCH_WORK)
    message(FATAL_ERROR "MEDIAPERCH_WORK must name a directory to work in")
endif()

find_program(FFMPEG ffmpeg)
if(NOT FFMPEG)
    # Locally this is a skip: the tool builds and runs without FFmpeg and so
    # should its tests. In CI it is a failure, because a check that silently
    # does not run is worse than no check.
    if(MEDIAPERCH_REQUIRE_FFMPEG)
        message(FATAL_ERROR "ffmpeg is required for the decode-quality check and was not found")
    endif()
    message(STATUS "ffmpeg not found -- skipping the decode-quality check")
    return()
endif()

file(MAKE_DIRECTORY "${MEDIAPERCH_WORK}")

# ---------------------------------------------------------------- the signal

# Builds `out`: `channels` of sweep-plus-noise at `rate`, each channel with its
# own sweep start and its own noise seed.
function(make_source out channels rate seconds)
    if(EXISTS "${out}")
        return()
    endif()
    math(EXPR top "${rate} / 3")
    set(inputs "")
    set(graph "")
    set(maps "")
    math(EXPR last "${channels} - 1")
    foreach(i RANGE ${last})
        math(EXPR f0 "40 + ${i} * 130")
        math(EXPR seed "7 + ${i} * 101")
        math(EXPR a "2 * ${i}")
        math(EXPR b "2 * ${i} + 1")
        list(APPEND inputs -f lavfi -i
             "aevalsrc=0.45*sin(2*PI*(${f0}*t+(${top}-${f0})/2/${seconds}*t*t)):s=${rate}:d=${seconds}")
        list(APPEND inputs -f lavfi -i
             "anoisesrc=sample_rate=${rate}:duration=${seconds}:amplitude=0.30:color=white:seed=${seed}")
        string(APPEND graph "[${a}:a][${b}:a]amix=inputs=2:weights=1 1:normalize=0[m${i}];")
        string(APPEND maps "[m${i}]")
    endforeach()

    set(layout "${channels}c")
    if(channels EQUAL 1)
        set(layout mono)
    elseif(channels EQUAL 2)
        set(layout stereo)
    elseif(channels EQUAL 6)
        set(layout 5.1)
    elseif(channels EQUAL 8)
        set(layout "7.1(wide)")
    endif()

    execute_process(
        COMMAND "${FFMPEG}" -hide_banner -loglevel error ${inputs}
                -filter_complex
                "${graph}${maps}join=inputs=${channels}:channel_layout=${layout},aformat=sample_fmts=s16[a]"
                -map "[a]" -c:a pcm_s16le "${out}" -y
        RESULT_VARIABLE status
        ERROR_VARIABLE why)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "could not build ${out}: ${why}")
    endif()
endfunction()

function(encode source out)
    execute_process(
        COMMAND "${FFMPEG}" -hide_banner -loglevel error -i "${source}" ${ARGN} "${out}" -y
        RESULT_VARIABLE status
        ERROR_VARIABLE why)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "could not encode ${out}: ${why}")
    endif()
endfunction()

# ----------------------------------------------------------------- the check

set(failed "")
set(ran 0)
set(skipped FALSE)

# 77 is what the probe returns when it was built without the measuring
# commands, which a build that ships is. Skipping then is right locally and
# wrong in CI, so MEDIAPERCH_REQUIRE_FFMPEG doubles as "this run is not allowed
# to skip anything" -- it is set exactly where that is true.
set(MEDIAPERCH_NO_DIAGNOSTICS 77)

function(check label encoded source)
    message(STATUS "")
    message(STATUS "--- ${label}")
    execute_process(
        COMMAND "${MEDIAPERCH_PROBE}" compare --file "${encoded}" --source "${source}" ${ARGN}
        RESULT_VARIABLE status
        OUTPUT_VARIABLE said
        ERROR_VARIABLE complained)
    string(STRIP "${said}${complained}" said)
    message(STATUS "${said}")
    if(status EQUAL MEDIAPERCH_NO_DIAGNOSTICS)
        set(skipped TRUE PARENT_SCOPE)
        return()
    endif()
    math(EXPR ran "${ran} + 1")
    set(ran "${ran}" PARENT_SCOPE)
    if(NOT status EQUAL 0)
        list(APPEND failed "${label}")
        set(failed "${failed}" PARENT_SCOPE)
    endif()
endfunction()

set(W "${MEDIAPERCH_WORK}")

# Thresholds are per row because the right answer is per row. A codec given 32
# kbps for a sweep and white noise cannot preserve band energies and should not
# be asked to; it can still be asked to produce the right number of frames
# starting in the right place, which is the failure every library measured here
# actually had.
#
#   --min-snr           a floor against the source, not a target
#   --band-limit/-tol   per-band energy, which for a perceptual codec is a far
#                       tighter check than a broadband figure: the encoder is
#                       *designed* to preserve band energy while discarding
#                       waveform detail, so a band that is wrong is a bug and a
#                       waveform that is different is not
#   --min-lag-margin    the signal proved it can locate itself
#   --min-channel-margin  ... and tell its channels apart
#   --vs-rival          and the one thing the source cannot check: agreement
#                       with FFmpeg *directly*, sample for sample. A
#                       noise-substituted band is arbitrary by design, so two
#                       decoders can fill one differently and sit exactly the
#                       same distance from the source. Measured on the bug this
#                       file was written after: every source-referenced check
#                       above passed and this one failed by more than a hundred
#                       decibels

# ------------------------------------------------------------------ lossless
# The strictest rows in the file: nothing was thrown away, so the decode has to
# equal the source and 120 dB is "equal" for a float comparison.
make_source("${W}/src_2_44100.wav" 2 44100 2)
encode("${W}/src_2_44100.wav" "${W}/q.flac" -c:a flac)
check("FLAC, stereo 44.1 kHz" "${W}/q.flac" "${W}/src_2_44100.wav"
      --min-snr 120 --vs-rival 200 --band-limit 16000 --band-tol 0.01
      --min-lag-margin 8 --min-channel-margin 3)

encode("${W}/src_2_44100.wav" "${W}/q_alac.m4a" -c:a alac)
check("ALAC, stereo 44.1 kHz" "${W}/q_alac.m4a" "${W}/src_2_44100.wav"
      --min-snr 120 --vs-rival 200 --band-limit 16000 --band-tol 0.01
      --min-lag-margin 8 --min-channel-margin 3)

make_source("${W}/src_6_48000.wav" 6 48000 2)
encode("${W}/src_6_48000.wav" "${W}/q6_alac.m4a" -c:a alac)
check("ALAC, 5.1 at 48 kHz" "${W}/q6_alac.m4a" "${W}/src_6_48000.wav"
      --min-snr 120 --vs-rival 200 --band-limit 16000 --band-tol 0.01
      --min-lag-margin 8 --min-channel-margin 3)

# ----------------------------------------------------------------------- MP3
# Lossy, and the reason decode_mp3 exists is the gapless tag -- so the length
# and the start are the point of this row.
encode("${W}/src_2_44100.wav" "${W}/q.mp3" -c:a libmp3lame -b:a 256k)
check("MP3, stereo 44.1 kHz 256k" "${W}/q.mp3" "${W}/src_2_44100.wav"
      --min-snr 8 --vs-rival 100 --band-limit 8000 --band-tol 2.5
      --min-lag-margin 8 --min-channel-margin 3)

# ----------------------------------------------------------------------- AAC
encode("${W}/src_2_44100.wav" "${W}/q_2_256k.m4a" -c:a aac -b:a 256k)
check("AAC, stereo 44.1 kHz 256k" "${W}/q_2_256k.m4a" "${W}/src_2_44100.wav"
      --min-snr 10 --vs-rival 125 --band-limit 8000 --band-tol 2.5
      --min-lag-margin 8 --min-channel-margin 3)

encode("${W}/src_2_44100.wav" "${W}/q_2_32k.m4a" -c:a aac -b:a 32k)
# At 32 kbps the codec keeps about four kilohertz of a signal that had sixteen,
# and a narrower signal has a broader correlation peak -- so the margin this row
# can show is smaller, and saying so is more honest than pretending the check is
# as strong here as it is above.
check("AAC, stereo 44.1 kHz 32k" "${W}/q_2_32k.m4a" "${W}/src_2_44100.wav"
      --min-snr -3 --vs-rival 125 --max-peak-ratio 3 --min-lag-margin 6 --min-channel-margin 3)

make_source("${W}/src_1_48000.wav" 1 48000 2)
encode("${W}/src_1_48000.wav" "${W}/q_1_192k.m4a" -c:a aac -b:a 192k)
check("AAC, mono 48 kHz 192k" "${W}/q_1_192k.m4a" "${W}/src_1_48000.wav"
      --min-snr 15 --vs-rival 125 --band-limit 8000 --band-tol 2.5 --min-lag-margin 8)

make_source("${W}/src_2_8000.wav" 2 8000 2)
encode("${W}/src_2_8000.wav" "${W}/q_2_8k.m4a" -c:a aac -b:a 64k)
check("AAC, stereo 8 kHz 64k" "${W}/q_2_8k.m4a" "${W}/src_2_8000.wav"
      --min-snr 12 --vs-rival 125 --band-limit 2000 --band-tol 2.5
      --min-lag-margin 8 --min-channel-margin 3)

make_source("${W}/src_2_96000.wav" 2 96000 2)
encode("${W}/src_2_96000.wav" "${W}/q_2_96k.m4a" -c:a aac -b:a 512k)
check("AAC, stereo 96 kHz 512k" "${W}/q_2_96k.m4a" "${W}/src_2_96000.wav"
      --min-snr 3 --vs-rival 125 --band-limit 8000 --band-tol 2.5
      --min-lag-margin 8 --min-channel-margin 3)

encode("${W}/src_6_48000.wav" "${W}/q_6_768k.m4a" -c:a aac -b:a 768k)
check("AAC, 5.1 at 48 kHz 768k" "${W}/q_6_768k.m4a" "${W}/src_6_48000.wav"
      --min-snr 4 --vs-rival 125 --band-limit 8000 --band-tol 2.5
      --min-lag-margin 8 --min-channel-margin 3)

# Eight channels is where FFmpeg's encoder writes channel_configuration 0 and
# puts the layout in a program config element. Getting that wrong decodes every
# channel perfectly and puts two of them in the wrong speakers, which is exactly
# what the channel column here is for.
make_source("${W}/src_8_48000.wav" 8 48000 2)
encode("${W}/src_8_48000.wav" "${W}/q_8_1024k.m4a" -c:a aac -b:a 1024k)
check("AAC, 7.1(wide) at 48 kHz 1024k" "${W}/q_8_1024k.m4a" "${W}/src_8_48000.wav"
      --min-snr 4 --vs-rival 125 --band-limit 8000 --band-tol 2.5
      --min-lag-margin 8 --min-channel-margin 3)

# Raw ADTS carries no gapless metadata, so the decode is *correctly* longer than
# the audio that was encoded -- but it still has to start in the same place, so
# every other check applies unchanged. This is the control that makes the exact
# lengths above a finding rather than a coincidence.
encode("${W}/src_2_44100.wav" "${W}/q_adts.aac" -c:a aac -b:a 256k -f adts)
check("AAC, raw ADTS stereo 44.1 kHz" "${W}/q_adts.aac" "${W}/src_2_44100.wav"
      --untrimmed --min-snr 10 --vs-rival 125 --band-limit 8000 --band-tol 2.5
      --min-lag-margin 8 --min-channel-margin 3)

# ---------------------------------------------------------------- the verdict

message(STATUS "")
if(skipped)
    if(MEDIAPERCH_REQUIRE_FFMPEG)
        message(FATAL_ERROR
            "This build has no measuring commands, so nothing was checked. "
            "Configure with -D MEDIAPERCH_DIAGNOSTICS=ON.")
    endif()
    message(STATUS "this build has no measuring commands -- nothing checked. "
                   "Configure with -D MEDIAPERCH_DIAGNOSTICS=ON, or use a Debug build.")
    return()
endif()
list(LENGTH failed how_many)
if(how_many GREATER 0)
    string(REPLACE ";" "\n  " pretty "${failed}")
    message(FATAL_ERROR "${how_many} of ${ran} rows failed:\n  ${pretty}")
endif()
message(STATUS "${ran} rows, every one against the audio that was encoded: all pass")
