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

    # Our own headers: any of them changing can change a finding in any file
    # that includes it, and clang-tidy has no depfile to tell us which. Coarse
    # on purpose -- touching PCH.h re-checks everything, which is right, and it
    # happens about never.
    file(GLOB FT_TIDY_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/src/core/*.h"
        "${CMAKE_SOURCE_DIR}/src/game/*.h")

    # One command per file, not one command over all of them. Measured on this
    # machine (2026-09-09, 20 logical cores), one src/game translation unit:
    #
    #     parse only, no checks                    8.1 s
    #     + the AST-matcher checks                42.9 s
    #     + clang-analyzer-*                      71.6 s
    #
    # 8 s of that is parsing CommonLibSSE without a PCH; the other 63 s is the
    # checks walking its inlined header bodies. There is no flag that stops
    # that -- the checks run over the translation unit's AST, and a header-only
    # library IS most of that AST. --header-filter only drops the findings
    # afterwards: about 82,000 per src/game file, which is where the "Suppressed
    # 1373265 warnings" of a whole serial run came from. Marking the include
    # dirs /external:I was tried and is not the answer either: 82,707 findings
    # instead of 82,709, and 68.5 s instead of 71.6. So the cost is inherent per
    # file, and the way out is to stop paying it 21 times in a row on a 20-core
    # machine. Measured after this change: 99 s for a full pass, against a
    # serial run that did not finish inside ten minutes.
    #
    # A file per command gives us two things from Ninja for free: the files run
    # in parallel, and a stamp per file means an unchanged file is not checked
    # again. The everyday cost after editing two files is two files.
    #
    # Note the stamps do NOT depend on the compile database: it is rewritten on
    # every configure of the debug preset, and depending on it would mean a full
    # re-check after every plugin build. Delete build/<preset>/tidy to force one.
    set(FT_TIDY_STAMPS "")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tidy")
    foreach(_src IN LISTS FT_TIDY_SOURCES)
        file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_src}")
        string(REPLACE "/" "_" _stampname "${_rel}")
        set(_stamp "${CMAKE_BINARY_DIR}/tidy/${_stampname}.stamp")
        add_custom_command(
            OUTPUT "${_stamp}"
            COMMAND "${FT_CLANG_TIDY}"
                    -p "${FT_TIDY_BUILD_DIR}"
                    --header-filter=src.\(core\|game\)
                    --extra-arg-before=/Y-
                    --extra-arg=-Wno-unused-command-line-argument
                    "${_src}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
            DEPENDS "${_src}" ${FT_TIDY_HEADERS}
            COMMENT "clang-tidy ${_rel}"
            VERBATIM)
        list(APPEND FT_TIDY_STAMPS "${_stamp}")
    endforeach()
    unset(_src)
    unset(_rel)
    unset(_stampname)
    unset(_stamp)

    add_custom_target(tidy DEPENDS ${FT_TIDY_STAMPS})
else()
    message(STATUS "clang-tidy: NOT FOUND -- 'tidy' target unavailable")
endif()

unset(_llvm_hints)
