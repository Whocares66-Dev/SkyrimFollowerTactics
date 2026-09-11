# Every follower is "they". A test holds the line mechanically (CLAUDE.md,
# "no gendered NPC pronouns"): a feminine pronoun anywhere in src/ or tests/
# fails the run, with the file and the line. (The masculine ones are left
# to review: the few there are name Marcurio and Mercer, particular men.)
# Run by ctest as `pronouns`; ROOT is the repository.
file(GLOB_RECURSE files "${ROOT}/src/*.cpp" "${ROOT}/src/*.h" "${ROOT}/tests/*.cpp" "${ROOT}/tests/*.h")
set(bad "")
foreach(f IN LISTS files)
    file(STRINGS "${f}" lines REGEX "(^|[^A-Za-z])([Hh]er|[Ss]he|[Hh]ers|[Hh]erself)([^A-Za-z]|$)")
    foreach(l IN LISTS lines)
        file(RELATIVE_PATH rel "${ROOT}" "${f}")
        list(APPEND bad "${rel}: ${l}")
    endforeach()
endforeach()
if(bad)
    list(JOIN bad "\n  " text)
    message(FATAL_ERROR "gendered pronouns; a follower is \"they\":\n  ${text}")
endif()
message(STATUS "no gendered pronouns in src/ or tests/")
