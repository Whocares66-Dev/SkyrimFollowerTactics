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

if(FT_CLANG_TIDY)
    message(STATUS "clang-tidy: ${FT_CLANG_TIDY}")

    # Scope: ft_core and the tests only. That is the RE::-free half of the
    # project -- plain C++23 with no CommonLibSSE and no MSVC extensions -- so
    # clang-tidy parses it exactly. Pointing it at src/game/ later would drag in
    # RE/Skyrim.h and bury real findings under thousands of third-party
    # diagnostics. The architectural line that makes the core testable is the
    # same line that makes it analysable.
    # The test file is deliberately NOT analysed. Catch2's REQUIRE_FALSE expands
    # into an enum-flag cast that the static analyzer flags inside Catch2's own
    # headers -- a third-party false positive that buries anything real.
    file(GLOB FT_TIDY_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/core/*.cpp")

    # compile_commands.json records MSVC-only switches that clang ignores;
    # without this every run leads with noise about them.
    add_custom_target(tidy
        COMMAND "${FT_CLANG_TIDY}"
                -p "${CMAKE_BINARY_DIR}"
                --extra-arg=-Wno-unused-command-line-argument
                ${FT_TIDY_SOURCES}
        COMMENT "Running clang-tidy over src/core"
        VERBATIM)
else()
    message(STATUS "clang-tidy: NOT FOUND -- 'tidy' target unavailable")
endif()

unset(_llvm_hints)
