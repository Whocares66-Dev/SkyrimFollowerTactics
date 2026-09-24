# Catch2 comes from vcpkg when the vcpkg toolchain is in play (the `debug` /
# `release` presets, which enable the manifest's `tests` feature). The `core`
# preset deliberately runs with no vcpkg at all -- that is the fast, game-free
# feedback loop -- so fall back to fetching Catch2 directly.

find_package(Catch2 3 CONFIG QUIET)

if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found via find_package; fetching it.")
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

add_executable(
    ft_tests
    test_archive.cpp
    test_attackscore.cpp
    test_bag.cpp
    test_bash.cpp
    test_blows.cpp
    test_breakdown.cpp
    test_clock.cpp
    test_conditions.cpp
    test_coordinator.cpp
    test_cosave.cpp
    test_customskills.cpp
    test_editor.cpp
    test_effects.cpp
    test_evaluator.cpp
    test_eventring.cpp
    test_files.cpp
    test_hitkinds.cpp
    test_hits.cpp
    test_i18n.cpp
    test_idle.cpp
    test_landing.cpp
    test_lease.cpp
    test_loadout.cpp
    test_logevent.cpp
    test_logsettings.cpp
    test_marks.cpp
    test_menuslots.cpp
    test_openrows.cpp
    test_party.cpp
    test_perktree.cpp
    test_playercast.cpp
    test_pluginfile.cpp
    test_profile.cpp
    test_reach.cpp
    test_rows.cpp
    test_sessions.cpp
    test_spells.cpp
    test_table.cpp
    test_tick.cpp
    test_variety.cpp
    test_vocabulary.cpp
    test_watchdog.cpp
)
# nlohmann_json is found or fetched by the top-level CMakeLists, beside ft_core;
# the profile tests parse the serialiser's output back to check its shape.
target_link_libraries(
    ft_tests
    PRIVATE ft_core Catch2::Catch2WithMain nlohmann_json::nlohmann_json
)
target_compile_features(ft_tests PRIVATE cxx_std_23)
ft_apply_warnings(ft_tests)

include(Catch)
catch_discover_tests(ft_tests)

# Progression's rules, in an executable of their own (tests/progression).
add_subdirectory(progression)

# Not a Catch2 test: a grep over the sources, so a gendered pronoun for a
# follower fails the run rather than waiting for a review to find it.
add_test(
    NAME pronouns
    COMMAND
        "${CMAKE_COMMAND}" -DROOT=${CMAKE_SOURCE_DIR} -P
        "${CMAKE_SOURCE_DIR}/tests/pronouns.cmake"
)

# Coverage report, for the core-cov preset:
#
#   .\tools\build.ps1 -Preset core-cov -Coverage
#   cmake --build --preset core-cov --target coverage
#
# Runs ft_tests and fp_tests once each as a single process (ctest would run
# each case in its own process and leave a hundred .profraw files), merges
# the counts, and prints a per-file summary for src/core and
# src/progression/core. The line-by-line view is written as HTML beside it,
# in build/core-cov/coverage/. Nothing outside those two is reported: the
# tests, Catch2 and the JSON library are means, not the thing measured.
if(FT_COVERAGE AND FT_LLVM_PROFDATA AND FT_LLVM_COV)
    set(_cov_dir "${CMAKE_BINARY_DIR}/coverage")
    set(_cov_sources
        "${CMAKE_SOURCE_DIR}/src/core"
        "${CMAKE_SOURCE_DIR}/src/progression/core"
    )
    add_custom_target(
        coverage
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_cov_dir}"
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "LLVM_PROFILE_FILE=${_cov_dir}/ft_tests.profraw"
            "$<TARGET_FILE:ft_tests>"
        COMMAND
            "${CMAKE_COMMAND}" -E env
            "LLVM_PROFILE_FILE=${_cov_dir}/fp_tests.profraw"
            "$<TARGET_FILE:fp_tests>"
        COMMAND
            "${FT_LLVM_PROFDATA}" merge -sparse "${_cov_dir}/ft_tests.profraw"
            "${_cov_dir}/fp_tests.profraw" -o "${_cov_dir}/tests.profdata"
        COMMAND
            "${FT_LLVM_COV}" show "$<TARGET_FILE:ft_tests>"
            "-object=$<TARGET_FILE:fp_tests>"
            "-instr-profile=${_cov_dir}/tests.profdata" -format=html
            "-output-dir=${_cov_dir}/html" ${_cov_sources}
        COMMAND
            "${FT_LLVM_COV}" report "$<TARGET_FILE:ft_tests>"
            "-object=$<TARGET_FILE:fp_tests>"
            "-instr-profile=${_cov_dir}/tests.profdata" ${_cov_sources}
        DEPENDS ft_tests fp_tests
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        COMMENT
            "Coverage of src/core and src/progression/core by their tests; HTML in ${_cov_dir}/html"
        VERBATIM
        USES_TERMINAL
    )
elseif(FT_COVERAGE)
    message(
        WARNING
        "FT_COVERAGE is ON but llvm-profdata/llvm-cov were not found; no 'coverage' target"
    )
endif()
