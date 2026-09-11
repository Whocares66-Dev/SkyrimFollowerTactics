// The hit table: what has hit an actor lately, and by whom. No Skyrim; the
// game side feeds it from the engine's events.

#include <catch2/catch_test_macros.hpp>

#include "core/HitTable.h"

using namespace ft;

TEST_CASE("a hit counts within the window, and the latest attacker is the one named", "[hits]")
{
    HitTable table;
    constexpr ActorId kLydia = 0xA2C94;
    constexpr ActorId kMage = 0x101;
    constexpr ActorId kArcher = 0x102;

    // Nothing noted: nothing.
    REQUIRE(table.Lately(kLydia, 100.0, 3.0).kinds == 0);
    REQUIRE(table.Lately(kLydia, 100.0, 3.0).attacker == 0);

    table.Note(kLydia, DamageKind::Frost, kMage, 99.0);
    table.Note(kLydia, DamageKind::Ranged, kArcher, 100.0);

    // Both inside the window; the archer hit last.
    Attacked now = table.Lately(kLydia, 101.0, 3.0);
    REQUIRE(now.kinds == (Bit(DamageKind::Frost) | Bit(DamageKind::Ranged)));
    REQUIRE(now.attacker == kArcher);

    // The frost has aged out, the arrow has not.
    now = table.Lately(kLydia, 102.5, 3.0);
    REQUIRE(now.kinds == Bit(DamageKind::Ranged));
    REQUIRE(now.attacker == kArcher);

    // Both gone.
    now = table.Lately(kLydia, 104.0, 3.0);
    REQUIRE(now.kinds == 0);
    REQUIRE(now.attacker == 0);

    // A later hit of the same kind replaces the earlier one's time and
    // attacker; someone else's hits are theirs.
    table.Note(kLydia, DamageKind::Frost, kArcher, 105.0);
    table.Note(kMage, DamageKind::Melee, kLydia, 105.0);
    now = table.Lately(kLydia, 105.5, 3.0);
    REQUIRE(now.kinds == Bit(DamageKind::Frost));
    REQUIRE(now.attacker == kArcher);
    REQUIRE(table.Lately(kMage, 105.5, 3.0).kinds == Bit(DamageKind::Melee));
}
