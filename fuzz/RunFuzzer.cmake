# SPDX-License-Identifier: GPL-3.0-or-later
#
# Runs one fuzz target against a scratch corpus, with the tracked seeds copied
# into it first.
#
# **The seeds are inputs, not a workspace, and passing them as a corpus
# directory does not make that true.** libFuzzer merges into its first corpus
# argument, so naming scratch first was supposed to be enough -- and mostly is,
# but not always: a run that reduces an input can write the smaller version back
# into whichever directory it came from, and then `git status` shows new files in
# `fuzz/corpus/` that nobody chose to keep. It happened twice in one afternoon.
#
# Copying instead removes the possibility rather than documenting it. The seeds
# are then reachable only for reading, which is what they are for.
#
# **What a run finds goes where it can be collected**: FINDINGS, as
# `<NAME>-crash-...`, `<NAME>-timeout-...` and the rest, rather than wherever
# the process happened to be started, which is where libFuzzer writes them
# unless told.
#
# Three variables of the environment turn the thirty seconds a push gets into
# the campaign the nightly workflow runs (.github/workflows/fuzz.yml):
#
#   MEDIAPERCH_FUZZ_SECONDS   how long, instead of the ARGS' -max_total_time
#                             and without their -runs, which would stop a long
#                             run at the count a short one is held to
#   MEDIAPERCH_FUZZ_CORPORA   a directory whose <NAME> subdirectory is the corpus,
#                             kept from one run to the next, instead of SCRATCH
#   MEDIAPERCH_FUZZ_FINDINGS  where the findings go, instead of FINDINGS

if(NOT DEFINED FUZZER OR NOT DEFINED SEEDS OR NOT DEFINED SCRATCH OR NOT DEFINED NAME
   OR NOT DEFINED FINDINGS)
    message(FATAL_ERROR "RunFuzzer.cmake needs FUZZER, SEEDS, SCRATCH, NAME and FINDINGS")
endif()

set(corpus "${SCRATCH}")
if(DEFINED ENV{MEDIAPERCH_FUZZ_CORPORA} AND NOT "$ENV{MEDIAPERCH_FUZZ_CORPORA}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{MEDIAPERCH_FUZZ_CORPORA}/${NAME}" corpus)
endif()
if(DEFINED ENV{MEDIAPERCH_FUZZ_FINDINGS} AND NOT "$ENV{MEDIAPERCH_FUZZ_FINDINGS}" STREQUAL "")
    file(TO_CMAKE_PATH "$ENV{MEDIAPERCH_FUZZ_FINDINGS}" FINDINGS)
endif()

file(MAKE_DIRECTORY "${corpus}" "${FINDINGS}")
if(EXISTS "${SEEDS}")
    file(GLOB seed_files "${SEEDS}/*")
    if(seed_files)
        file(COPY ${seed_files} DESTINATION "${corpus}")
    endif()
endif()

string(REPLACE "|" ";" fuzzer_args "${ARGS}")
if(DEFINED ENV{MEDIAPERCH_FUZZ_SECONDS} AND NOT "$ENV{MEDIAPERCH_FUZZ_SECONDS}" STREQUAL "")
    list(FILTER fuzzer_args EXCLUDE REGEX "^-(runs|max_total_time)=")
    list(APPEND fuzzer_args "-max_total_time=$ENV{MEDIAPERCH_FUZZ_SECONDS}")
endif()
list(APPEND fuzzer_args "-artifact_prefix=${FINDINGS}/${NAME}-")

execute_process(
    COMMAND "${FUZZER}" "${corpus}" ${fuzzer_args}
    RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${FUZZER} exited with ${status}")
endif()
