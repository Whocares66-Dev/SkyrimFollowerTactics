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

TEST_CASE("hex is upper case, padded to the width asked", "[ids]")
{
    CHECK(fp::Hex(0x0A2C8E, 6) == "0A2C8E");
    CHECK(fp::Hex(0, 6) == "000000");
    CHECK(fp::Hex(0x50524F46, 8) == "50524F46");
    CHECK(fp::Hex(0x1234567, 6) == "1234567"); // wider than asked: every digit kept
    CHECK(fp::ToString({"Skyrim.esm", 0x0A2C8E}) == "Skyrim.esm|0A2C8E");
}
