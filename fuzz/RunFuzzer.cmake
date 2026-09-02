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

if(NOT DEFINED FUZZER OR NOT DEFINED SEEDS OR NOT DEFINED SCRATCH)
    message(FATAL_ERROR "RunFuzzer.cmake needs FUZZER, SEEDS and SCRATCH")
endif()

file(MAKE_DIRECTORY "${SCRATCH}")
if(EXISTS "${SEEDS}")
    file(GLOB seed_files "${SEEDS}/*")
    if(seed_files)
        file(COPY ${seed_files} DESTINATION "${SCRATCH}")
    endif()
endif()

string(REPLACE "|" ";" fuzzer_args "${ARGS}")
execute_process(
    COMMAND "${FUZZER}" "${SCRATCH}" ${fuzzer_args}
    RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "${FUZZER} exited with ${status}")
endif()
