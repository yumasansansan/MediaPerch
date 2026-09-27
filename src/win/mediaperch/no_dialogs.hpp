// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A program that fails on stderr and in its exit code, never in a window.
//
// **A dialog is a test that never ends.** The debug CRT reports a failed check
// -- an `assert`, and every check of the STL's iterator debugging -- in a
// message box and waits for somebody to press a button, and Windows Error
// Reporting does the same for an abort and a crash. A CI runner has nobody to
// press it, so the job waits out its timeout and says nothing about why; on a
// desktop the window lands in front of whatever the person was doing. A check
// in a module the program had loaded -- mp_demux_mkv, made to read a file that
// was not Matroska -- came up that way in a Debug run that ctest reported as
// passed.
//
// So the programs the tests run, and the tests themselves, report on stderr
// instead: the CRT's errors and assertions go there (and to a debugger, if one
// is listening), an invalid parameter -- which is how the STL's checks end --
// says what it was and aborts, an abort does not call Windows Error Reporting,
// and a crash is not shown in a window. Aborting rather than exiting keeps a
// failure a failure: exit code 3 for ctest, and a crash with its input saved
// for libFuzzer. LLVM's unit test main does the same, for the same reason.

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>

#    include <crtdbg.h>
#    include <cstdint>
#    include <cstdio>
#    include <cstdlib>
#    include <cwchar>
#    include <initializer_list>
#endif

namespace mp::win {

#ifdef _WIN32
namespace detail {

/// Says what the CRT was handed, and aborts. The release CRT passes nothing
/// but nulls; the debug CRT passes all of it.
inline void invalid_parameter(const wchar_t* expression, const wchar_t* function,
                              const wchar_t* file, unsigned int line, std::uintptr_t) noexcept
{
    std::fwprintf(stderr, L"invalid parameter: %ls in %ls, %ls line %u\n",
                  expression != nullptr ? expression : L"(not said)",
                  function != nullptr ? function : L"(not said)",
                  file != nullptr ? file : L"(not said)", line);
    std::fflush(stderr);
    std::abort();
}

} // namespace detail
#endif

/// Everything above, for the whole process. Call it before anything can fail.
inline void no_dialogs() noexcept
{
#ifdef _WIN32
#    ifdef _DEBUG
    for (const int report : {_CRT_ERROR, _CRT_ASSERT}) {
        _CrtSetReportMode(report, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
        _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
    }
#    endif
    _set_error_mode(_OUT_TO_STDERR);
    _set_abort_behavior(0, _CALL_REPORTFAULT);
    _set_invalid_parameter_handler(&detail::invalid_parameter);
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
}

} // namespace mp::win
