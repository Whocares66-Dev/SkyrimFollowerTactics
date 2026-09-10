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
# clang-format runs on stock configuration: .clang-format is a single
# BasedOnStyle line. clang-tidy reads .clang-tidy at the repo root, which adds
# the correctness and performance check groups and nothing stylistic.

set(_llvm_hints
    "$ENV{VCINSTALLDIR}/Tools/Llvm/x64/bin"
    "$ENV{VCToolsInstallDir}/../Llvm/x64/bin"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Community/VC/Tools/Llvm/x64/bin"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Professional/VC/Tools/Llvm/x64/bin"
    "$ENV{ProgramFiles}/Microsoft Visual Studio/2022/Enterprise/VC/Tools/Llvm/x64/bin"
    "$ENV{ProgramFiles}/LLVM/bin")

find_program(FT_CLANG_FORMAT  NAMES clang-format  HINTS ${_llvm_hints})
find_program(FT_CLANG_TIDY    NAMES clang-tidy    HINTS ${_llvm_hints})
# For the `coverage` target (tests/CMakeLists.txt); same LLVM, same place.
find_program(FT_LLVM_PROFDATA NAMES llvm-profdata HINTS ${_llvm_hints})
find_program(FT_LLVM_COV      NAMES llvm-cov      HINTS ${_llvm_hints})
# LLVM's parallel driver for clang-tidy. It spreads the same work across cores
# rather than reducing it -- every src/game TU still parses all of CommonLibSSE,
# which no filter avoids -- and that alone is most of the linter's wall time.
find_program(FT_RUN_CLANG_TIDY NAMES run-clang-tidy run-clang-tidy.py HINTS ${_llvm_hints})
find_package(Python3 QUIET COMPONENTS Interpreter)

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

    # Scope: everything WE wrote -- src/core, plus src/game where it is built.
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
    #
    # Each preset lints what its OWN database covers -- src/game is in the
    # plugin's alone. Pointing -p at another preset's is what went stale;
    # CLAUDE.md, "The linter's blind spot", has that story.
    file(GLOB FT_TIDY_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/core/*.cpp")
    set(FT_TIDY_SCOPE "src/core")
    set(FT_TIDY_PATTERN "src.core.")
    if(FT_BUILD_PLUGIN)
        file(GLOB FT_TIDY_GAME CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/game/*.cpp")
        list(APPEND FT_TIDY_SOURCES ${FT_TIDY_GAME})
        set(FT_TIDY_SCOPE "src/core and src/game")
        set(FT_TIDY_PATTERN "src.(core|game).")
    endif()

    if(FT_RUN_CLANG_TIDY AND Python3_Interpreter_FOUND)
        # The driver takes a REGEX matched against the database, not a file
        # list, so a pattern matching nothing lints nothing and still exits 0.
        # If `tidy` comes back instant and empty, count the files it named.
        set(FT_TIDY_COMMAND
            "${Python3_EXECUTABLE}" "${FT_RUN_CLANG_TIDY}"
            -p "${CMAKE_BINARY_DIR}"
            -clang-tidy-binary "${FT_CLANG_TIDY}"
            -header-filter "src.(core|game)"
            -extra-arg-before=/Y-
            -extra-arg=-Wno-unused-command-line-argument
            -quiet
            "${FT_TIDY_PATTERN}")
    else()
        set(FT_TIDY_COMMAND
            "${FT_CLANG_TIDY}"
            -p "${CMAKE_BINARY_DIR}"
            --header-filter=src.\(core\|game\)
            --extra-arg-before=/Y-
            --extra-arg=-Wno-unused-command-line-argument
            ${FT_TIDY_SOURCES})
    endif()

    add_custom_target(tidy
        COMMAND ${FT_TIDY_COMMAND}
        COMMENT "Running clang-tidy over ${FT_TIDY_SCOPE}"
        VERBATIM)
else()
    message(STATUS "clang-tidy: NOT FOUND -- 'tidy' target unavailable")
endif()

unset(_llvm_hints)
