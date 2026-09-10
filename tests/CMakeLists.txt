# Catch2 comes from vcpkg when the vcpkg toolchain is in play (the `debug` /
# `release` presets, which enable the manifest's `tests` feature). The `core`
# preset deliberately runs with no vcpkg at all -- that is the fast, game-free
# feedback loop -- so fall back to fetching Catch2 directly.

find_package(Catch2 3 CONFIG QUIET)

if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found via find_package; fetching it.")
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        v3.7.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
endif()

add_executable(ft_tests test_blows.cpp test_evaluator.cpp test_loadout.cpp test_logevent.cpp
                       test_profile.cpp)
# nlohmann_json is found or fetched by the top-level CMakeLists, beside ft_core;
# the profile tests parse the serialiser's output back to check its shape.
target_link_libraries(ft_tests PRIVATE ft_core Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
target_compile_features(ft_tests PRIVATE cxx_std_23)
ft_apply_warnings(ft_tests)

include(Catch)
catch_discover_tests(ft_tests)
