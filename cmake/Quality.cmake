# The format and lint targets, over every language of ours in the repository:
#
#   cmake --build --preset core --target format         rewrite files in place
#   cmake --build --preset core --target format-check    fail if anything is unformatted
#   cmake --build --preset core --target tidy            static analysis
#
#   C++         clang-format, clang-tidy
#   Python      ruff format, ruff check
#   PowerShell  PSScriptAnalyzer's Invoke-Formatter and Invoke-ScriptAnalyzer
#   CMake       gersemi, a formatter only
#
# CMake has no linter here. cmake-lint (cmakelang) was tried on 2026-09-22:
# every one of its 745 findings was layout or naming, and it has had no
# release since 2021. The check that finds CMake bugs is CMake's own
# --warn-uninitialized, clean over our files in both presets that day; it
# fires in vcpkg's and CommonLibSSE's files too, so it is not on by default.
#
# Each formatter runs its stock style, and each linter the checks that find
# bugs and costs, nothing stylistic: .clang-format, .clang-tidy, ruff.toml and
# .gersemirc say which; PSScriptAnalyzer runs its defaults.
#
# clang-format, clang-tidy and the coverage tools are LLVM's own release,
# 22 or later (`winget install LLVM.LLVM`, into Program Files\LLVM, or where
# LLVM_ROOT says), and only that: not the older LLVM Visual Studio bundles as
# its "C++ Clang tools" component, which is searched nowhere. clang-tidy 22
# walks none of a system header's declarations -- the STL's, and vcpkg's
# libraries' under /external:I -- which took a src/game file from 45.9 s
# under the bundled 18 to 15.0 s (2026-09-25). CommonLibSSE is not a system
# header to clang, which ignores /external:anglebrackets, and making it one
# changed nothing: 16.8 s and the same 389 warnings either way.
# tools/build.ps1 puts the same bin first on PATH, for core-cov's clang-cl.
# ruff and gersemi are `pip install --user`, and PSScriptAnalyzer
# `Install-Module PSScriptAnalyzer -Scope CurrentUser`. A tool that is
# missing is said at configure time, and its language skipped.

set(_llvm_hints "$ENV{LLVM_ROOT}/bin" "$ENV{ProgramFiles}/LLVM/bin")

find_program(
    FT_CLANG_FORMAT
    NAMES clang-format
    HINTS ${_llvm_hints}
    NO_DEFAULT_PATH
)
find_program(
    FT_CLANG_TIDY
    NAMES clang-tidy
    HINTS ${_llvm_hints}
    NO_DEFAULT_PATH
)
# For the `coverage` target (tests/CMakeLists.txt); same LLVM, same place.
find_program(
    FT_LLVM_PROFDATA
    NAMES llvm-profdata
    HINTS ${_llvm_hints}
    NO_DEFAULT_PATH
)
find_program(FT_LLVM_COV NAMES llvm-cov HINTS ${_llvm_hints} NO_DEFAULT_PATH)

# ruff by `python -m`: pip's --user install puts ruff.exe in a Scripts folder
# that is not on PATH, so find_program would miss it.
set(FT_RUFF "")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m ruff --version
        RESULT_VARIABLE _ruff_result
        OUTPUT_VARIABLE _ruff_version
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_ruff_result EQUAL 0)
        set(FT_RUFF "${Python3_EXECUTABLE}" -m ruff)
        message(STATUS "ruff: ${_ruff_version} (${Python3_EXECUTABLE})")
    endif()
endif()
if(NOT FT_RUFF)
    message(
        STATUS
        "ruff: NOT FOUND -- Python is not formatted or linted; pip install --user ruff"
    )
endif()

# gersemi the same way. It walks into build/ and extern/ if given a folder, so
# it is given git's list instead. Read here, at configure time, and not stale:
# a new CMake file does nothing until a CMakeLists.txt includes it, and that
# edit reconfigures.
set(FT_GERSEMI "")
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" -m gersemi --version
        RESULT_VARIABLE _gersemi_result
        OUTPUT_VARIABLE _gersemi_version
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_gersemi_result EQUAL 0)
        set(FT_GERSEMI "${Python3_EXECUTABLE}" -m gersemi)
        message(STATUS "gersemi: ${_gersemi_version}")
    endif()
endif()
if(FT_GERSEMI)
    execute_process(
        COMMAND
            git ls-files --cached --others --exclude-standard -- "*.cmake"
            "*CMakeLists.txt"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _git_result
        OUTPUT_VARIABLE FT_CMAKE_FILES
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _git_result EQUAL 0 OR NOT FT_CMAKE_FILES)
        message(
            STATUS
            "gersemi: no CMake files from git -- CMake is not formatted"
        )
        set(FT_GERSEMI "")
    else()
        string(REPLACE "\n" ";" FT_CMAKE_FILES "${FT_CMAKE_FILES}")
    endif()
else()
    message(
        STATUS
        "gersemi: NOT FOUND -- CMake is not formatted; pip install --user gersemi"
    )
endif()
unset(_gersemi_result)
unset(_gersemi_version)
unset(_git_result)

set(FT_PSCHECK "")
find_program(FT_PWSH NAMES pwsh)
if(FT_PWSH)
    execute_process(
        COMMAND
            "${FT_PWSH}" -NoProfile -NonInteractive -Command
            "if (-not (Get-Module -ListAvailable PSScriptAnalyzer)) { exit 1 }"
        RESULT_VARIABLE _pssa_result
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(_pssa_result EQUAL 0)
        set(FT_PSCHECK
            "${FT_PWSH}"
            -NoProfile
            -NonInteractive
            -File
            "${CMAKE_SOURCE_DIR}/tools/check-powershell.ps1"
        )
        message(STATUS "PSScriptAnalyzer: found (${FT_PWSH})")
    endif()
endif()
if(NOT FT_PSCHECK)
    message(
        STATUS
        "PSScriptAnalyzer: NOT FOUND -- PowerShell is not formatted or linted; "
        "Install-Module PSScriptAnalyzer -Scope CurrentUser, under pwsh 7"
    )
endif()
unset(_ruff_result)
unset(_ruff_version)
unset(_pssa_result)

# Our own sources only. Never glob the build tree: it holds fetched third-party
# code (Catch2) and vcpkg headers, and reformatting those would be both wrong
# and enormous.
file(
    GLOB_RECURSE FT_SOURCES
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h"
    "${CMAKE_SOURCE_DIR}/tests/*.cpp"
    "${CMAKE_SOURCE_DIR}/tests/*.h"
)

set(FT_FORMAT_COMMANDS "")
set(FT_FORMAT_CHECK_COMMANDS "")
if(FT_CLANG_FORMAT)
    message(STATUS "clang-format: ${FT_CLANG_FORMAT}")
    list(
        APPEND FT_FORMAT_COMMANDS
        COMMAND
        "${FT_CLANG_FORMAT}"
        -i
        --style=file
        ${FT_SOURCES}
    )
    # -Werror turns "would reformat" into a non-zero exit, which is what makes
    # this usable as a gate rather than a report.
    list(
        APPEND FT_FORMAT_CHECK_COMMANDS
        COMMAND
        "${FT_CLANG_FORMAT}"
        --dry-run
        -Werror
        --style=file
        ${FT_SOURCES}
    )
else()
    message(STATUS "clang-format: NOT FOUND -- C++ is not formatted")
endif()
# ruff and the PowerShell script find their own files: what git would track,
# so a script in a new folder is not missed.
if(FT_RUFF)
    list(APPEND FT_FORMAT_COMMANDS COMMAND ${FT_RUFF} format --quiet)
    list(APPEND FT_FORMAT_CHECK_COMMANDS COMMAND ${FT_RUFF} format --check)
endif()
if(FT_GERSEMI)
    list(
        APPEND FT_FORMAT_COMMANDS
        COMMAND
        ${FT_GERSEMI}
        --in-place
        --quiet
        ${FT_CMAKE_FILES}
    )
    list(
        APPEND FT_FORMAT_CHECK_COMMANDS
        COMMAND
        ${FT_GERSEMI}
        --check
        ${FT_CMAKE_FILES}
    )
endif()
if(FT_PSCHECK)
    list(APPEND FT_FORMAT_COMMANDS COMMAND ${FT_PSCHECK} -Mode Format)
    list(
        APPEND FT_FORMAT_CHECK_COMMANDS
        COMMAND
        ${FT_PSCHECK}
        -Mode
        FormatCheck
    )
endif()
if(FT_FORMAT_COMMANDS)
    add_custom_target(
        format
        ${FT_FORMAT_COMMANDS}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Formatting C++, Python, PowerShell and CMake"
        VERBATIM
    )
    add_custom_target(
        format-check
        ${FT_FORMAT_CHECK_COMMANDS}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Checking the formatting of C++, Python, PowerShell and CMake"
        VERBATIM
    )
endif()

if(FT_CLANG_TIDY)
    message(STATUS "clang-tidy: ${FT_CLANG_TIDY}")

    # Scope: everything WE wrote -- src/core and the tests, plus src/game and the
    # files at the top of src (plugin.cpp, the entry point) where they are built.
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
    # Each preset lints what its OWN database covers -- src/game and the top of
    # src are in the plugin's alone. Pointing -p at another preset's is what
    # went stale; CLAUDE.md, "The linter's blind spot", has that story.
    file(
        GLOB FT_TIDY_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/core/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/progression/core/*.cpp"
    )
    set(FT_TIDY_SCOPE "src/core and src/progression/core")
    if(FT_BUILD_PLUGIN)
        file(
            GLOB FT_TIDY_GAME
            CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/src/game/*.cpp"
            "${CMAKE_SOURCE_DIR}/src/game/ui/*.cpp"
            "${CMAKE_SOURCE_DIR}/src/fix/*.cpp"
            "${CMAKE_SOURCE_DIR}/src/progression/game/*.cpp"
            "${CMAKE_SOURCE_DIR}/src/*.cpp"
        )
        list(APPEND FT_TIDY_SOURCES ${FT_TIDY_GAME})
        set(FT_TIDY_SCOPE
            "src/core, src/game, src/game/ui, src/fix, src/progression and src/*.cpp"
        )
    endif()
    # The tests are in every preset's database that builds them. tests/.clang-tidy
    # says what is relaxed there, and why.
    if(FT_BUILD_TESTS)
        file(
            GLOB_RECURSE FT_TIDY_TESTS
            CONFIGURE_DEPENDS
            "${CMAKE_SOURCE_DIR}/tests/*.cpp"
        )
        list(APPEND FT_TIDY_SOURCES ${FT_TIDY_TESTS})
        string(APPEND FT_TIDY_SCOPE ", tests")
    endif()

    # Our own headers: any of them changing can change a finding in any file
    # that includes it, and clang-tidy emits no depfile to tell us which. Coarse
    # on purpose -- touching PCH.h re-checks everything, which is right, and it
    # happens about never.
    file(
        GLOB_RECURSE FT_TIDY_HEADERS
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/tests/*.h"
    )

    # One custom command per file, not one command over all of them.
    #
    # The cost is per file. Measured 2026-09-25 under clang-tidy 22 on
    # src/game/Traits.cpp:
    #
    #     parse only                               9.7 s
    #     + the AST-matcher checks                11.1 s
    #     + clang-analyzer-*                      16.8 s
    #
    # Most of it is parsing CommonLibSSE and the STL, which clang cannot take
    # from MSVC's .pch. (Under 18 the checks walked every system header too:
    # 8.1, 42.9 and 71.6 s on a file in 2026-09-09.) A file far slower than
    # that is the analyzer, and the file's compile command run through
    # clang-cl with `--analyze -Xclang -analyzer-display-progress` prints its
    # time per function: in UI.cpp on 2026-09-25, 106 of its 149 s went to
    # the 64 instances of one trampoline, each analysed into the whole page
    # it draws.
    #
    # So spread it, and then stop repeating it. A command per file gets two
    # things from Ninja that one command cannot: the files run in parallel
    # across cores, and a stamp per file means an unchanged file is not checked
    # again. Measured after this change: a full pass 99 s, nothing changed 3.6 s,
    # one core file touched 6.6 s -- and that last number is the one that
    # matters, because it is the after-every-edit loop. This replaces LLVM's
    # run-clang-tidy driver, which parallelised but re-checked everything every
    # run and needed Python beside the binary.
    #
    # The stamps deliberately do NOT depend on the compile database: it is
    # rewritten on every configure, and depending on it would mean a full
    # re-check after every build. Delete build/<preset>/tidy to force one.

    # Reports from any header under this tree's src/ or tests/, by its absolute
    # path: a list of folder names left src/progression out until 2026-09-22,
    # and a bare "src" would also match a vendored library's src/. clang-tidy
    # prints mixed separators (src\core/I18n.h), hence either slash.
    set(FT_TIDY_HEADER_FILTER "${CMAKE_SOURCE_DIR}")
    foreach(
        _c
        .
        +
        *
        ?
        ^
        $
        "("
        ")"
        "|"
        "{"
        "}"
        "["
    )
        string(
            REPLACE "${_c}"
            "\\${_c}"
            FT_TIDY_HEADER_FILTER
            "${FT_TIDY_HEADER_FILTER}"
        )
    endforeach()
    string(APPEND FT_TIDY_HEADER_FILTER "/(src|tests)/")
    string(REPLACE "/" "[/\\]" FT_TIDY_HEADER_FILTER "${FT_TIDY_HEADER_FILTER}")
    unset(_c)

    set(FT_TIDY_STAMPS "")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tidy")
    foreach(_src IN LISTS FT_TIDY_SOURCES)
        file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_src}")
        string(REPLACE "/" "_" _stampname "${_rel}")
        set(_stamp "${CMAKE_BINARY_DIR}/tidy/${_stampname}.stamp")
        # The compile database is MSVC's, and clang takes /Zc:preprocessor
        # (which CommonLibSSE propagates) and /external:anglebrackets without
        # using them; under WarningsAsErrors that note would fail every file.
        add_custom_command(
            OUTPUT "${_stamp}"
            COMMAND
                "${FT_CLANG_TIDY}" -p "${CMAKE_BINARY_DIR}"
                "--header-filter=${FT_TIDY_HEADER_FILTER}"
                --extra-arg-before=/Y-
                --extra-arg=-Wno-unused-command-line-argument "${_src}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
            DEPENDS
                "${_src}"
                ${FT_TIDY_HEADERS}
                "${CMAKE_SOURCE_DIR}/.clang-tidy"
                "${CMAKE_SOURCE_DIR}/tests/.clang-tidy"
            COMMENT "clang-tidy ${_rel}"
            VERBATIM
        )
        list(APPEND FT_TIDY_STAMPS "${_stamp}")
    endforeach()
    unset(_src)
    unset(_rel)
    unset(_stampname)
    unset(_stamp)

    # The scope is named once, at configure time, because the target itself now
    # prints a line per file: "clang-tidy over src/core" with nothing following
    # it means the glob found nothing, not that the code is clean.
    message(STATUS "clang-tidy scope: ${FT_TIDY_SCOPE}")
else()
    set(FT_TIDY_STAMPS "")
    message(STATUS "clang-tidy: NOT FOUND -- C++ is not linted")
endif()

# ruff check runs every time: it takes well under a second. PSScriptAnalyzer
# takes seconds to load, so its script keeps a stamp and returns at once while
# no script has changed.
set(FT_LINT_COMMANDS "")
if(FT_RUFF)
    list(APPEND FT_LINT_COMMANDS COMMAND ${FT_RUFF} check --quiet)
endif()
if(FT_PSCHECK)
    list(
        APPEND FT_LINT_COMMANDS
        COMMAND
        ${FT_PSCHECK}
        -Mode
        Lint
        -Stamp
        "${CMAKE_BINARY_DIR}/tidy/powershell.stamp"
    )
endif()
if(FT_TIDY_STAMPS OR FT_LINT_COMMANDS)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tidy")
    add_custom_target(
        tidy
        ${FT_LINT_COMMANDS}
        DEPENDS ${FT_TIDY_STAMPS}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VERBATIM
    )
endif()

unset(_llvm_hints)
