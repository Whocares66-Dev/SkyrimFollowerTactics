# Progression's rules (src/progression/core, fp_core): an executable of its
# own beside ft_tests, so the two suites' names and helpers never meet.
add_executable(fp_tests
    test_companion.cpp
    test_ids.cpp
    test_levelling.cpp
    test_perks.cpp
    test_serialize.cpp
    test_skills.cpp
    test_spells.cpp
    test_vanilla.cpp)
target_link_libraries(fp_tests PRIVATE fp_core Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
target_compile_features(fp_tests PRIVATE cxx_std_23)
# The vanilla trees (dev/research/extract_perk_trees.py) are read from the
# source tree, not copied: the file is the one the generator writes.
target_compile_definitions(fp_tests PRIVATE FP_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
ft_apply_warnings(fp_tests)

catch_discover_tests(fp_tests TEST_PREFIX "progression: ")
