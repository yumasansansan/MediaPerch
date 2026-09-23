# SPDX-License-Identifier: GPL-3.0-or-later
#
# **The toolchain is LLVM's, all of it, and one version of it.**
#
# Checked when the build is configured -- before anything is built, so that a
# wrong tool is a sentence at the top of the log rather than a binary nobody
# can account for: the C and C++ compilers, which are Clang's GNU driver; the
# dependency scanner CMake runs over C++23 sources; the LLD that Clang runs for
# -fuse-ld=lld, which is lld-link on Windows and ld.lld elsewhere; the
# archivers, including the ones CMake uses for link-time optimisation; the
# resource compiler and the binary tools the presets name; and, by
# `mediaperch_check_llvm_tool` from the list file that enables it, the
# MASM-compatible assembler. A tool that reports a version has to report the
# compilers' exactly. llvm-rc, llvm-ml and llvm-dlltool report none, so they
# have to lie beside the C++ compiler instead.
#
# **Why the check exists at all.** This tree once built cleanly, for a whole
# CI run, with MinGW's GCC: the Ninja generator outside a developer prompt found
# `cc` on PATH, and nothing in the log admitted the toolchain was wrong. The
# presets name every tool now, and this is what makes naming them a guarantee
# rather than a hope. When MEDIAPERCH_LLVM_MAJOR is set in the environment --
# CI sets it, from ci/setup.sh -- the compilers have to be of that major
# version as well, so a runner image that changes under the workflow is a red
# configure rather than a quiet upgrade.
#
# **A tool named by the presets is pinned to a path here.** The presets name
# tools the way a person types them -- `llvm-ar`, `llvm-rc` -- and a bare name
# is looked up again by everything that runs it: by Ninja through PATH, and by
# the fresh CMake inside each external project, which takes a relative FILEPATH
# to be a file in its own build directory. That second one is not
# hypothetical: libde265, libaom, avm and HM all failed to archive on the first
# build here, each looking for an `llvm-ar` in its own build directory. So each
# name is found here, once, in the directory the compilers came from, and the
# cache holds the full path from then on -- the tool that was checked is the
# tool that runs.
#
# **The C++ library is the one part that is not LLVM's.** On Windows it is the
# MSVC STL, linked dynamically: msvcp140.dll, which the system updates and
# every Visual C++ program already asks for. That is the one piece of Visual
# Studio a build here still uses, and it uses it as a library rather than as a
# compiler. It is recorded with its version rather than required to be of one:
# what a build produces asks the system for it at load time, so a fault in it is
# the system's to fix and not a reason to release again, and its version is a
# thing to know and to show.
#
# Everything checked is written to llvm-toolchain.txt in the build directory,
# one line per tool -- its part, its version and its program, separated by
# tabs -- which CI prints in the log of every build job.
#
# Included by cmake/CompilerOptions.cmake, after project().

set(MEDIAPERCH_LLVM_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
get_filename_component(MEDIAPERCH_LLVM_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
file(REAL_PATH "${MEDIAPERCH_LLVM_BIN}" MEDIAPERCH_LLVM_BIN)

# **An LLVM updated in place is a build directory to configure again.** CMake
# asks a compiler its version once, on the first configure, and keeps the
# answer; the installer then replaces the programs at the same paths, and every
# check below would compare the new tools with the old number. Said here, once,
# with what to do about it, rather than as a disagreement between two tools.
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" --version
    OUTPUT_VARIABLE mediaperch_compiler_says ERROR_QUIET TIMEOUT 60)
if(mediaperch_compiler_says MATCHES "clang version ([0-9]+\\.[0-9]+\\.[0-9]+)"
   AND NOT CMAKE_MATCH_1 VERSION_EQUAL MEDIAPERCH_LLVM_VERSION)
    message(FATAL_ERROR
        "${CMAKE_CXX_COMPILER} is LLVM ${CMAKE_MATCH_1} now, and this build directory "
        "was configured with LLVM ${MEDIAPERCH_LLVM_VERSION}, which CMake remembers. "
        "Configure it again with --fresh (cmake --preset <name> --fresh) -- or remove "
        "it, if it has external projects in it, whose own caches --fresh does not reach.")
endif()

set(MEDIAPERCH_LLVM_TOOLCHAIN_FILE "${CMAKE_BINARY_DIR}/llvm-toolchain.txt")
file(WRITE "${MEDIAPERCH_LLVM_TOOLCHAIN_FILE}" "")

if(DEFINED ENV{MEDIAPERCH_LLVM_MAJOR})
    string(REGEX MATCH "^[0-9]+" MEDIAPERCH_LLVM_MAJOR "${MEDIAPERCH_LLVM_VERSION}")
    if(NOT MEDIAPERCH_LLVM_MAJOR STREQUAL "$ENV{MEDIAPERCH_LLVM_MAJOR}")
        message(FATAL_ERROR
            "The compilers are LLVM ${MEDIAPERCH_LLVM_VERSION}, and MEDIAPERCH_LLVM_MAJOR "
            "asks for LLVM $ENV{MEDIAPERCH_LLVM_MAJOR}.")
    endif()
    message(STATUS "LLVM ${MEDIAPERCH_LLVM_MAJOR}, the major version MEDIAPERCH_LLVM_MAJOR asks for")
endif()

# One tool: its file name has to match `pattern`, and its version the
# compilers' -- or, for a tool that reports none, its directory the compilers'.
# A bare name is looked for beside the compilers and nowhere else.
function(mediaperch_check_llvm_tool part program pattern)
    if(NOT program)
        message(FATAL_ERROR "No program is set for the ${part}.")
    endif()
    if(NOT IS_ABSOLUTE "${program}")
        find_program(MEDIAPERCH_LLVM_TOOL_PATH NAMES "${program}"
            HINTS "${MEDIAPERCH_LLVM_BIN}" NO_DEFAULT_PATH NO_CACHE)
        if(NOT MEDIAPERCH_LLVM_TOOL_PATH)
            message(FATAL_ERROR
                "The ${part}, ${program}, is not beside the compilers in ${MEDIAPERCH_LLVM_BIN}.")
        endif()
        set(program "${MEDIAPERCH_LLVM_TOOL_PATH}")
    endif()

    get_filename_component(name "${program}" NAME)
    if(NOT name MATCHES "${pattern}")
        message(FATAL_ERROR "The ${part} is ${program}, which is not one of LLVM's tools.")
    endif()

    execute_process(COMMAND "${program}" --version
        OUTPUT_VARIABLE output ERROR_VARIABLE output TIMEOUT 60)
    if(output MATCHES "(clang version|LLVM version|LLD) ([0-9]+\\.[0-9]+\\.[0-9]+)")
        set(version "${CMAKE_MATCH_2}")
        set(checked "${version}")
        if(NOT version VERSION_EQUAL MEDIAPERCH_LLVM_VERSION)
            message(FATAL_ERROR
                "The ${part} is ${program}, from LLVM ${version}, and the compilers are "
                "LLVM ${MEDIAPERCH_LLVM_VERSION}. Two versions of one toolchain is two "
                "toolchains.")
        endif()
    else()
        set(version "none")
        set(checked "reports no version; beside the compiler")
        get_filename_component(directory "${program}" DIRECTORY)
        file(REAL_PATH "${directory}" directory)
        set(expected "${MEDIAPERCH_LLVM_BIN}")
        if(CMAKE_HOST_WIN32)
            string(TOLOWER "${directory}" directory)
            string(TOLOWER "${expected}" expected)
        endif()
        if(NOT directory STREQUAL expected)
            message(FATAL_ERROR
                "The ${part} is ${program}, which reports no version and is not beside "
                "the C++ compiler in ${MEDIAPERCH_LLVM_BIN}.")
        endif()
    endif()
    message(STATUS "  ${part}: ${program} (${checked})")
    file(APPEND "${MEDIAPERCH_LLVM_TOOLCHAIN_FILE}" "${part}\t${version}\t${program}\n")
endfunction()

# The tool in `variable`, pinned: a bare name becomes the full path of the one
# beside the compilers, in the cache and in the caller's scope -- CMake keeps
# the name it was given in both -- and that path is what is checked.
function(mediaperch_pin_llvm_tool variable pattern)
    set(program "${${variable}}")
    if(NOT program)
        message(FATAL_ERROR "No program is set in ${variable}.")
    endif()
    if(NOT IS_ABSOLUTE "${program}")
        find_program(MEDIAPERCH_LLVM_TOOL_PATH NAMES "${program}"
            HINTS "${MEDIAPERCH_LLVM_BIN}" NO_DEFAULT_PATH NO_CACHE)
        if(NOT MEDIAPERCH_LLVM_TOOL_PATH)
            message(FATAL_ERROR
                "${variable} is ${program}, which is not beside the compilers in "
                "${MEDIAPERCH_LLVM_BIN}.")
        endif()
        set(program "${MEDIAPERCH_LLVM_TOOL_PATH}")
        get_property(docstring CACHE "${variable}" PROPERTY HELPSTRING)
        set(${variable} "${program}" CACHE FILEPATH "${docstring}" FORCE)
        set(${variable} "${program}" PARENT_SCOPE)
    endif()
    mediaperch_check_llvm_tool("${variable}" "${program}" "${pattern}")
endfunction()

message(STATUS "LLVM toolchain ${MEDIAPERCH_LLVM_VERSION}")
mediaperch_check_llvm_tool("C compiler" "${CMAKE_C_COMPILER}" "^clang(-[0-9]+)?(\\.exe)?$")
mediaperch_check_llvm_tool("C++ compiler" "${CMAKE_CXX_COMPILER}"
    "^clang(\\+\\+)?(-[0-9]+)?(\\.exe)?$")

# The scanner CMake runs over every C++23 source for module dependencies before
# compiling it -- the first thing to see each compile's options, which is how
# the first build here learned that `-mguard=cf` is MinGW's.
if(CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS)
    mediaperch_check_llvm_tool("dependency scanner" "${CMAKE_CXX_COMPILER_CLANG_SCAN_DEPS}"
        "^clang-scan-deps(-[0-9]+)?(\\.exe)?$")
endif()

# The LLD that Clang finds for -fuse-ld=lld, asked of Clang itself rather than
# of PATH: the one it would run is the one that matters.
if(WIN32)
    set(MEDIAPERCH_LLD_NAME lld-link)
elseif(APPLE)
    set(MEDIAPERCH_LLD_NAME ld64.lld)
else()
    set(MEDIAPERCH_LLD_NAME ld.lld)
endif()
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" "--print-prog-name=${MEDIAPERCH_LLD_NAME}"
    OUTPUT_VARIABLE MEDIAPERCH_LLD OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT IS_ABSOLUTE "${MEDIAPERCH_LLD}")
    message(FATAL_ERROR
        "Clang does not find ${MEDIAPERCH_LLD_NAME}, the LLD it runs for -fuse-ld=lld.")
endif()
set(MEDIAPERCH_LLD "${MEDIAPERCH_LLD}" CACHE INTERNAL
    "The LLD Clang runs, which cargo is told to link the Rust modules with too")
mediaperch_check_llvm_tool("linker" "${MEDIAPERCH_LLD}"
    "^(lld-link|ld64\\.lld|ld\\.lld)(\\.exe)?$")

foreach(archiver IN ITEMS
        CMAKE_AR CMAKE_RANLIB
        CMAKE_C_COMPILER_AR CMAKE_C_COMPILER_RANLIB
        CMAKE_CXX_COMPILER_AR CMAKE_CXX_COMPILER_RANLIB)
    mediaperch_pin_llvm_tool(${archiver} "^llvm-(ar|ranlib)(-[0-9]+)?(\\.exe)?$")
endforeach()

# CMake enables the resource compiler by itself on Windows, and nothing here
# has a resource script today; it is checked because a submodule that brings
# one would otherwise be compiled by whatever `rc` PATH had.
if(CMAKE_RC_COMPILER)
    mediaperch_pin_llvm_tool(CMAKE_RC_COMPILER "^llvm-rc(\\.exe)?$")
endif()

# The binary tools the presets name. Nothing in the build runs them on Windows
# today, but an external project or a later list file may, and a name in the
# cache is a promise that it is the checked one.
foreach(tool IN ITEMS CMAKE_NM CMAKE_OBJCOPY CMAKE_OBJDUMP CMAKE_STRIP CMAKE_DLLTOOL)
    if(${tool})
        mediaperch_pin_llvm_tool(${tool}
            "^llvm-(nm|objcopy|objdump|strip|dlltool)(-[0-9]+)?(\\.exe)?$")
    endif()
endforeach()

# **The C++ library, named by the headers a build actually compiles against.**
# One program says it: compiled and linked the way CMake compiles and links
# anything here, so that it is given what the build's compilations are given,
# and what it says about itself it says in a #pragma message, which comes back
# in the output. Compiling it names the library and the version of its headers;
# linking it says the library is there to link, not only to include.
function(mediaperch_check_cxx_library)
    set(check "${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/cxx-library")
    file(WRITE "${check}/main.cpp"
        "#include <version>\n"
        "#include <stdexcept>\n"
        "#include <string>\n"
        "#define MEDIAPERCH_TEXT_OF(x) #x\n"
        "#define MEDIAPERCH_TEXT(x) MEDIAPERCH_TEXT_OF(x)\n"
        "#if defined(_LIBCPP_VERSION)\n"
        "#  pragma message(\"MEDIAPERCH_CXX_LIBRARY libc++ \" MEDIAPERCH_TEXT(_LIBCPP_VERSION) \" 0\")\n"
        "#elif defined(__GLIBCXX__)\n"
        "#  pragma message(\"MEDIAPERCH_CXX_LIBRARY libstdc++ \" MEDIAPERCH_TEXT(_GLIBCXX_RELEASE) \" \" MEDIAPERCH_TEXT(__GLIBCXX__))\n"
        "#elif defined(_MSVC_STL_UPDATE)\n"
        "#  pragma message(\"MEDIAPERCH_CXX_LIBRARY MSVC-STL \" MEDIAPERCH_TEXT(_MSVC_STL_VERSION) \" \" MEDIAPERCH_TEXT(_MSVC_STL_UPDATE))\n"
        "#else\n"
        "#  pragma message(\"MEDIAPERCH_CXX_LIBRARY none 0 0\")\n"
        "#endif\n"
        "int main() { try { throw std::runtime_error(std::string(\"x\")); }\n"
        "             catch (const std::exception &e) { return e.what()[0] == 'x' ? 0 : 1; } }\n")
    try_compile(MEDIAPERCH_CXX_LIBRARY_LINKS "${check}/build" SOURCES "${check}/main.cpp"
        CMAKE_FLAGS "-DCMAKE_CXX_STANDARD=23" "-DCMAKE_LINKER_TYPE=LLD"
        OUTPUT_VARIABLE output)
    if(NOT MEDIAPERCH_CXX_LIBRARY_LINKS)
        message(FATAL_ERROR
            "A C++ program does not compile and link with this toolchain's C++ library. "
            "On Windows that library is the MSVC STL, which Clang finds in the Visual "
            "Studio installation or Build Tools on this machine:\n${output}")
    endif()
    if(NOT output MATCHES "MEDIAPERCH_CXX_LIBRARY ([A-Za-z+-]+) ([0-9]+) ([0-9]+)")
        message(FATAL_ERROR
            "A C++ program compiles and links, and the compiler did not say which C++ "
            "library its headers are:\n${output}")
    endif()
    set(library "${CMAKE_MATCH_1}")
    set(version "${CMAKE_MATCH_2}")
    set(date "${CMAKE_MATCH_3}")
    if(library STREQUAL "none")
        message(FATAL_ERROR
            "The headers on the include path are no C++ library this build knows: not "
            "the MSVC STL, not libc++, not libstdc++.")
    endif()
    if(library STREQUAL "MSVC-STL")
        set(library "MSVC STL")
    endif()
    set(named "${library} ${version}")
    set(record "${library}")
    if(NOT date EQUAL 0)
        string(APPEND named " (${date})")
        set(record "${library} ${date}")
    endif()
    message(STATUS "  C++ library: ${named}")
    file(APPEND "${MEDIAPERCH_LLVM_TOOLCHAIN_FILE}" "C++ library\t${version}\t${record}\n")
endfunction()

mediaperch_check_cxx_library()
