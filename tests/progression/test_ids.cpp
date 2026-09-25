#include "progression/core/Ids.h"

#include <catch2/catch_test_macros.hpp>

using fp::FormKey;

TEST_CASE("a form key reads back as it was written", "[ids]")
{
    const FormKey lydia{"Skyrim.esm", 0x0A2C8E};
    CHECK(fp::ToString(lydia) == "Skyrim.esm|0A2C8E");
    CHECK(fp::ParseFormKey("Skyrim.esm|0A2C8E") == lydia);
}

TEST_CASE("a plugin name with spaces and bars before the last one survives", "[ids]")
{
    const FormKey odd{"My Mod | Extra.esp", 0x800};
    CHECK(fp::ParseFormKey(fp::ToString(odd)) == odd);
}

TEST_CASE("anything that is not a form key is refused", "[ids]")
{
    CHECK_FALSE(fp::ParseFormKey(""));
    CHECK_FALSE(fp::ParseFormKey("Skyrim.esm"));
    CHECK_FALSE(fp::ParseFormKey("|0A2C8E"));
    CHECK_FALSE(fp::ParseFormKey("Skyrim.esm|"));
    CHECK_FALSE(fp::ParseFormKey("Skyrim.esm|XYZ"));
    CHECK_FALSE(fp::ParseFormKey("Skyrim.esm|1000000")); // past a local id's 24 bits
}

TEST_CASE("a form key's id is six upper-case hex digits", "[ids]")
{
    CHECK(fp::ToString({"Skyrim.esm", 0x0A2C8E}) == "Skyrim.esm|0A2C8E");
    CHECK(fp::ToString({"Skyrim.esm", 0}) == "Skyrim.esm|000000");
}
