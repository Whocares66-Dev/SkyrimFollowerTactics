# clang-format / clang-tidy targets.
#
# These ship inside Visual Studio (VC\Tools\Llvm\x64\bin) as the "C++ Clang tools
# for Windows" component, so they are not an extra dependency on an MSVC box --
# VS uses clang-format for "Format Document" and clang-tidy for Code Analysis.
#
#   cmake --build --preset core --target format         rewrite files in place
#   cmake --build --preset core --target format-check    fail if anything is unformatted
#   cmake --build --preset core --target tidy            static analysis
#
# Both tools run on stock configuration: .clang-format is a single BasedOnStyle
# line, and there is deliberately no .clang-tidy, so clang-tidy uses its built-in
# default checks.

set(_llvm_hints
    "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin"
    "$ENV{VCToolsInstallDir}/../Llvm/x64/bin"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/x64/bin"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Enterprise/VC/Tools/Llvm/x64/bin"
    "$ENV{ProgramFiles}/LLVM/bin")

find_program(FT_CLANG_FORMAT NAMES clang-format HINTS ${_llvm_hints})
find_program(FT_CLANG_TIDY   NAMES clang-tidy   HINTS ${_llvm_hints})

# Our own sources only. Never glob the build tree: it holds fetched third-party
# code (Catch2) and vcpkg headers, and reformatting those would be both wrong
# and enormous.
file(GLOB_RECURSE FT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/tests/*.h")

if(FT_CLANG_FORMAT)
    message(STATUS "clang-format: ${FT_CLANG_FORMAT}")

    add_custom_target(format
        COMMAND "${FT_CLANG_FORMAT}" -i --style=file ${FT_SOURCES}
        COMMENT "Formatting sources with clang-format"
        VERBATIM)

    # -Werror turns "would reformat" into a non-zero exit, which is what makes
    # this usable as a gate rather than a report.
    add_custom_target(format-check
        COMMAND "${FT_CLANG_FORMAT}" --dry-run -Werror --style=file ${FT_SOURCES}
        COMMENT "Checking formatting with clang-format"
        VERBATIM)
else()
    message(STATUS "clang-format: NOT FOUND -- 'format' targets unavailable")
endif()

# src/game only appears in the PLUGIN's compile database, which the core-only
# presets never generate. Point tidy at build/debug when it exists so `tidy`
# works from any preset; the core files are in both.
set(FT_TIDY_BUILD_DIR "${CMAKE_BINARY_DIR}")
if(EXISTS "${CMAKE_SOURCE_DIR}/build/debug/compile_commands.json")
    set(FT_TIDY_BUILD_DIR "${CMAKE_SOURCE_DIR}/build/debug")
endif()

if(FT_CLANG_TIDY)
    message(STATUS "clang-tidy: ${FT_CLANG_TIDY}")

    # Scope: everything WE wrote -- src/core and src/game both.
    #
    # This used to be src/core only, on the grounds that src/game pulls in
    # RE/Skyrim.h and would bury real findings under third-party noise. That was
    # right when src/game was empty; it is not now, and it left the imperative
    # half -- the half that can actually crash the game -- entirely unchecked.
    #
    # Two flags make it work:
    #  * --header-filter restricts reports to our own headers, so CommonLibSSE's
    #    thousands of lines stay quiet while our .h files are still checked.
    #  * /Y- disables the precompiled header. MSVC's .pch is not a format clang
    #    can read, and without this clang-tidy fails outright with "not a valid
    #    precompiled PCH file" -- which looks exactly like a clean run, because
    #    it reports zero findings.
    file(GLOB FT_TIDY_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/core/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/game/*.cpp")

    add_custom_target(tidy
        COMMAND "${FT_CLANG_TIDY}"
                -p "${FT_TIDY_BUILD_DIR}"
                --header-filter=src.\(core\|game\)
                --extra-arg-before=/Y-
                --extra-arg=-Wno-unused-command-line-argument
                ${FT_TIDY_SOURCES}
        COMMENT "Running clang-tidy over src/core and src/game"
        VERBATIM)
else()
    message(STATUS "clang-tidy: NOT FOUND -- 'tidy' target unavailable")
endif()

unset(_llvm_hints)
