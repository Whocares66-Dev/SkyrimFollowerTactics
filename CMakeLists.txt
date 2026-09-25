cmake_minimum_required(VERSION 3.21)

project(FollowerTactics VERSION 0.3.0 LANGUAGES CXX)

option(
    FT_BUILD_PLUGIN
    "Build the SKSE plugin (needs CommonLibSSE-NG, Windows only)"
    ${WIN32}
)
option(FT_BUILD_TESTS "Build the core unit tests (no game required)" ON)

option(FT_SANITIZE "Build with AddressSanitizer + UBSan where available" OFF)
option(FT_ANALYZE "MSVC static analysis (/analyze) on our own targets" OFF)
option(
    FT_COVERAGE
    "Instrument for test coverage (clang-cl only; core-cov preset)"
    OFF
)

# ---------------------------------------------------------------------------
# Shared compile hygiene. The core engine is the only part of this project that
# can be verified without launching Skyrim, so it is worth over-tooling: max
# warnings, and a sanitized build variant for the test loop. The conformance
# flags are set here, per target, and nowhere else: the presets carry only
# /EHsc, and CommonLibSSE propagates /Zc:preprocessor to the plugin itself.
# ---------------------------------------------------------------------------
function(ft_apply_warnings target)
    if(MSVC)
        # MSVC is also true for clang-cl, which takes /W4 and /permissive-
        # but has nothing to do for /Zc:preprocessor: its preprocessor
        # conforms already, and it warns about the unused flag.
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /Zc:preprocessor)
            # Third-party headers are what <angle brackets> include: the
            # STL, CommonLibSSE, the menu framework (whose vendored header
            # has W4 findings of its own). Their warnings are not ours to
            # fix, so they are not shown; /analyze below skips them too.
            target_compile_options(
                ${target}
                PRIVATE /external:anglebrackets /external:W0
            )
        else()
            # clang's /W4 includes -Wextra, which warns when an aggregate
            # init leaves trailing members out. Those are value-initialised,
            # by the standard; the tests do it on purpose, everywhere.
            target_compile_options(
                ${target}
                PRIVATE -Wno-missing-field-initializers
            )
        endif()
        # Microsoft's static analyser, a different engine from clang-tidy's:
        # null dereferences, uninitialised members, buffer bounds through
        # SAL, the C6xxx warnings. Slower to compile, so it is an option,
        # toggled by tools\build.ps1 -Analyze; the flag is cached, so a
        # toggle recompiles our targets both ways. External headers are
        # analysed only for what our code passes into them. clang-cl
        # accepts MSVC's flags but not this one.
        if(FT_ANALYZE AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(
                ${target}
                PRIVATE /analyze /analyze:external-
            )
        endif()
    else()
        target_compile_options(
            ${target}
            PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
        )
    endif()
endfunction()

# AddressSanitizer has to cover EVERY translation unit in the tree, third-party
# code included, so this is deliberately global rather than per-target. MSVC's
# STL switches on container annotations under ASan; linking an annotated object
# against an unannotated static library fails with a wall of
#   LNK2038: mismatch detected for 'annotate_string'
# which says nothing about sanitizers. Applying the flags before any target or
# FetchContent subproject is declared is what keeps Catch2 consistent with us.
if(FT_SANITIZE)
    if(MSVC)
        add_compile_options(/fsanitize=address /Oy-)
        add_link_options(/INCREMENTAL:NO)
        # ASan is incompatible with /RTC1, which CMake puts in the Debug flags.
        string(
            REPLACE "/RTC1"
            ""
            CMAKE_CXX_FLAGS_DEBUG
            "${CMAKE_CXX_FLAGS_DEBUG}"
        )
    else()
        add_compile_options(
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        )
        add_link_options(-fsanitize=address,undefined)
    endif()
    message(
        STATUS
        "FT_SANITIZE: AddressSanitizer enabled for the whole build tree"
    )
endif()

# Test coverage, clang's source-based kind: every region of ft_core and
# fp_core gets a counter, ft_tests and fp_tests run once each, and llvm-cov
# turns the counts into a report
# (the `coverage` target, tests/CMakeLists.txt). Needs clang-cl, which the
# core-cov preset selects; cl.exe has no equivalent. The instrumented
# objects ask the linker for clang_rt.profile themselves (a /DEFAULTLIB
# directive), but link.exe does not know clang's resource directory, so it
# is put on the library path here.
if(FT_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(
            FATAL_ERROR
            "FT_COVERAGE needs clang-cl (use the core-cov preset); "
            "compiler is ${CMAKE_CXX_COMPILER_ID}"
        )
    endif()
    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" /clang:-print-resource-dir
        OUTPUT_VARIABLE _clang_resource_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    link_directories("${_clang_resource_dir}/lib/windows")
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    message(
        STATUS
        "FT_COVERAGE: instrumenting; profile runtime from ${_clang_resource_dir}/lib/windows"
    )
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include(Quality)

# ---------------------------------------------------------------------------
# ft_core -- the rule engine.
#
# This target must NEVER link CommonLibSSE and must NEVER see an RE:: type.
# That constraint is what makes it testable: there is no headless test harness
# for Skyrim, so the only route to automated tests is logic that does not need
# the game. If you ever find yourself wanting to #include "RE/Skyrim.h" here,
# the code you are writing belongs in src/game/ instead.
# ---------------------------------------------------------------------------
add_library(
    ft_core
    STATIC
    src/core/Archive.cpp
    src/core/AttackScore.cpp
    src/core/BagView.cpp
    src/core/Bash.cpp
    src/core/Strike.cpp
    src/core/Blows.cpp
    src/core/Breakdown.cpp
    src/core/Clock.cpp
    src/core/CoSave.cpp
    src/core/Coordinator.cpp
    src/core/CustomSkills.cpp
    src/core/Editor.cpp
    src/core/Effects.cpp
    src/core/Evaluator.cpp
    src/core/EventRing.cpp
    src/core/GraphEvents.cpp
    src/core/Files.cpp
    src/core/HitKinds.cpp
    src/core/HitTable.cpp
    src/core/I18n.cpp
    src/core/Lease.cpp
    src/core/Loadout.cpp
    src/core/LogEvent.cpp
    src/core/LogSettings.cpp
    src/core/Marks.cpp
    src/core/MenuSlots.cpp
    src/core/Names.cpp
    src/core/OpenRows.cpp
    src/core/PlayerCast.cpp
    src/core/PluginFile.cpp
    src/core/Routes.cpp
    src/core/Party.cpp
    src/core/PerkTree.cpp
    src/core/Profile.cpp
    src/core/Reach.cpp
    src/core/Rows.cpp
    src/core/Rule.cpp
    src/core/Sessions.cpp
    src/core/Spells.cpp
    src/core/Table.cpp
    src/core/Tick.cpp
    src/core/Variety.cpp
    src/core/Vocabulary.cpp
    src/core/Watchdog.cpp
)
target_include_directories(ft_core PUBLIC src)
target_compile_features(ft_core PUBLIC cxx_std_23)
ft_apply_warnings(ft_core)

# nlohmann/json, for the profile files (src/core/Profile.cpp). The plugin
# presets get it from vcpkg (it is in the manifest); the core presets run
# with no vcpkg at all, so fall back to fetching it -- the same arrangement
# tests/CMakeLists.txt makes for Catch2. Header-only, so PRIVATE: nothing
# that links ft_core needs to see it.
find_package(nlohmann_json 3 CONFIG QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via find_package; fetching it.")
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        URL
            https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH
            SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()
target_link_libraries(ft_core PRIVATE nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# fp_core -- Progression's rules (src/progression/core): the player's
# levelling, perks, spells, the ledger and its co-save records, for
# companions. The same rule as ft_core: no RE:: type, no CommonLibSSE; the
# plugin links it and tests/progression tests it. dev/PROGRESSION.md.
# ---------------------------------------------------------------------------
add_library(
    fp_core
    STATIC
    src/progression/core/Companion.cpp
    src/progression/core/Ids.cpp
    src/progression/core/Levelling.cpp
    src/progression/core/Perks.cpp
    src/progression/core/Serialize.cpp
    src/progression/core/Skills.cpp
    src/progression/core/Spells.cpp
)
target_include_directories(fp_core PUBLIC src)
target_compile_features(fp_core PUBLIC cxx_std_23)
ft_apply_warnings(fp_core)
target_link_libraries(fp_core PRIVATE ft_core nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# The SKSE plugin.
# ---------------------------------------------------------------------------
if(FT_BUILD_PLUGIN)
    # The build copies the .dll nowhere. It used to land in a mod folder named
    # by SKYRIM_MODS_FOLDER, which was one machine's layout in the build of a
    # mod that ships as a zip: the plugin is installed from
    # dist\follower-tactics-<version>-test.zip like any other mod (see
    # tools\package.ps1). Nothing here needs to know where anyone's Skyrim is,
    # and a build can run with the game up, since no file of the game's is
    # held open.

    # CommonLibSSE-NG, alandtse's live fork, as a submodule built with us
    # (extern/commonlibsse-ng, branch ng). Its own dependencies -- spdlog,
    # rapidcsv, directxtk and what they pull -- come through our vcpkg
    # manifest, since a manifest build reads only the top-level one. Its
    # tests are not ours to build. dev/COMMONLIB.md has the history.
    set(BUILD_TESTS OFF CACHE BOOL "CommonLibSSE's own tests" FORCE)
    # AE only, for now (2026-09-17). The plugin calls engine functions by
    # Address Library ID, and an ID is only stable within one line: the same
    # number names another function in the Special Edition's database, which
    # is a separately numbered space. Every address of our own is an
    # (SE, AE) pair in src/game/Addresses.h, each half read on its own
    # build (dev/VERSIONS.md), so the one DLL runs on both lines. VR is a
    # third library again, one nobody here can read or run, so it stays off.
    set(ENABLE_SKYRIM_SE
        ON
        CACHE BOOL
        "Skyrim SE support in CommonLibSSE"
        FORCE
    )
    set(ENABLE_SKYRIM_VR
        OFF
        CACHE BOOL
        "Skyrim VR support in CommonLibSSE"
        FORCE
    )
    set(ENABLE_SKYRIM_AE
        ON
        CACHE BOOL
        "Skyrim AE support in CommonLibSSE"
        FORCE
    )
    add_subdirectory(extern/commonlibsse-ng EXCLUDE_FROM_ALL)
    # add_commonlibsse_plugin: the library's CMakeLists includes it only on
    # its prebuilt path (7.5.1), so the source build includes it here.
    include(extern/commonlibsse-ng/cmake/CommonLibSSE.cmake)

    # src/game/ -- every RE:: call lives here. Thin, imperative, verified by
    # playing. Nothing in here may be #included from src/core/.
    add_commonlibsse_plugin(
        ${PROJECT_NAME}
        SOURCES
            src/plugin.cpp
            src/fix/DispelHold.cpp
            src/fix/StaffCharge.cpp
            src/game/AiScore.cpp
            src/game/Log.cpp
            src/game/Sensors.cpp
            src/game/Bag.cpp
            src/game/CharacterSheet.cpp
            src/game/EffectRows.cpp
            src/game/Effects.cpp
            src/game/PerkSheet.cpp
            src/game/Spells.cpp
            src/game/Traits.cpp
            src/game/Values.cpp
            src/game/Sheet.cpp
            src/game/Inventory.cpp
            src/game/Magic.cpp
            src/game/Pins.cpp
            src/game/Places.cpp
            src/game/Hits.cpp
            src/game/I18n.cpp
            src/game/Actions.cpp
            src/game/Blows.cpp
            src/game/Graph.cpp
            src/game/PlayerCast.cpp
            src/game/Tactics.cpp
            src/game/Toggles.cpp
            src/game/Util.cpp
            src/game/Packages.cpp
            src/game/Forms.cpp
            src/game/Profiles.cpp
            src/game/Settings.cpp
            src/game/CustomSkillsFramework.cpp
            # The in-game panel (game/ui/UI.h)
            src/game/ui/Widgets.cpp
            src/game/ui/Panel.cpp
            src/game/ui/Conditions.cpp
            src/game/ui/Actions.cpp
            src/game/ui/RuleTable.cpp
            src/game/ui/Sections.cpp
            src/game/ui/Inventory.cpp
            src/game/ui/Magic.cpp
            src/game/ui/Effects.cpp
            src/game/ui/Summons.cpp
            src/game/ui/Character.cpp
            src/game/ui/Skills.cpp
            src/game/ui/Pages.cpp
            src/game/ui/Menu.cpp
            # Progression (dev/PROGRESSION.md)
            src/progression/game/Actors.cpp
            src/progression/game/Events.cpp
            src/progression/game/Forms.cpp
            src/progression/game/Learning.cpp
            src/progression/game/Persistence.cpp
            src/progression/game/PerkTrees.cpp
            src/progression/game/PerkView.cpp
            src/progression/game/Rules.cpp
            src/progression/game/Service.cpp
            src/progression/game/SpellView.cpp
            src/progression/game/Tomes.cpp
            src/progression/game/ValueView.cpp
    )
    target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

    # These are Skyrim/Windows-API concerns and belong to THIS target only.
    # They used to live in the preset's global CMAKE_CXX_FLAGS, where
    # -DUNICODE leaked into third-party code built in the same tree: it
    # flipped Catch2's entry point from main() to wmain(), and the test
    # executable then failed to link with an unresolved external 'main'
    # that pointed nowhere near the actual cause.
    target_compile_definitions(
        ${PROJECT_NAME}
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
            # Every structured log line carries the build that wrote it
            # (dev/LOGGING.md's envelope), so a .jsonl attached to a bug report
            # says which version produced it without anyone having to ask.
            FT_VERSION="${PROJECT_VERSION}"
    )
    ft_apply_warnings(${PROJECT_NAME})
    # A release build keeps its .pdb -- in build\release, not in the package
    # (27 MB against a 1 MB DLL) -- so a crash log's "FollowerTactics.dll
    # +5C8BD" reads as a line here rather than as a disassembly (the Nordic
    # Souls crash of 2026-09-08 took one). The linker folds and strips as
    # usual.
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(
        ${PROJECT_NAME}
        PRIVATE $<$<CONFIG:Release>:/DEBUG:FULL /OPT:REF /OPT:ICF>
    )
    # The SKSE Menu Framework SDK is a single vendored header. It declares the
    # whole ImGui API under namespace ImGuiMCP and forwards every call into
    # SKSEMenuFramework.dll via GetProcAddress, so there is nothing to link --
    # no vcpkg imgui, no extra library. The mod page's "import both imgui and
    # this mod" reads as if imgui were a build dependency; it is not.
    # Not committed -- see extern/SKSEMenuFramework/README.md. Fail here with
    # something actionable rather than 200 lines of "ImGuiMCP is undeclared".
    if(
        NOT EXISTS
            "${CMAKE_SOURCE_DIR}/extern/SKSEMenuFramework/SKSEMenuFramework.h"
    )
        message(
            FATAL_ERROR
            "Missing extern/SKSEMenuFramework/SKSEMenuFramework.h.\n"
            "Download \"SKSEMenuFramework Header\" from the Files tab of\n"
            "  https://www.nexusmods.com/skyrimspecialedition/mods/120352\n"
            "and extract it there. See extern/SKSEMenuFramework/README.md."
        )
    endif()

    target_include_directories(${PROJECT_NAME} PRIVATE extern/SKSEMenuFramework)

    target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
    target_link_libraries(
        ${PROJECT_NAME}
        PRIVATE ft_core fp_core nlohmann_json::nlohmann_json
    )

    # Microsoft Detours, for the one function-ENTRY hook (Pins.cpp, on
    # ActorEquipManager::EquipObject). CommonLibSSE's trampoline
    # write_branch/write_call overwrite an existing jump or call instruction
    # and preserve nothing, so they cannot detour a function's first bytes;
    # Detours relocates the displaced prologue and hands back a callable
    # original.
    find_path(DETOURS_INCLUDE_DIRS "detours/detours.h" REQUIRED)
    find_library(DETOURS_LIBRARY detours REQUIRED)
    target_include_directories(${PROJECT_NAME} PRIVATE ${DETOURS_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${DETOURS_LIBRARY})
endif()

# ---------------------------------------------------------------------------
# Tests. Run these constantly; they are the only fast feedback loop in this
# project. Everything else costs a Skyrim restart.
# ---------------------------------------------------------------------------
if(FT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
