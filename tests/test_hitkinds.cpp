// What a hit counts as, from what the event says.

#include <catch2/catch_test_macros.hpp>

#include "core/HitKinds.h"

using namespace ft;

TEST_CASE("a spell is magic and its elements; a poison is poison besides; a blow is melee or ranged", "[hits]")
{
    HitSeen fireAndFrost;
    fireAndFrost.magic = true;
    fireAndFrost.detrimental = {Resist::Fire, Resist::Frost};
    REQUIRE(KindsOfHit(fireAndFrost) ==
            std::vector<DamageKind>{DamageKind::Magic, DamageKind::Fire, DamageKind::Magic, DamageKind::Frost});

    // A beneficial spell landing: nothing detrimental, nothing noted.
    HitSeen heal;
    heal.magic = true;
    REQUIRE(KindsOfHit(heal).empty());

    // A poison with no harmful effect of its own is still a poison.
    HitSeen poison;
    poison.magic = true;
    poison.poison = true;
    REQUIRE(KindsOfHit(poison) == std::vector<DamageKind>{DamageKind::Poison});
    poison.detrimental = {Resist::Poison};
    REQUIRE(KindsOfHit(poison) == std::vector<DamageKind>{DamageKind::Magic, DamageKind::Poison, DamageKind::Poison});

    HitSeen arrow;
    arrow.projectile = true;
    REQUIRE(KindsOfHit(arrow) == std::vector<DamageKind>{DamageKind::Ranged});
    REQUIRE(KindsOfHit(HitSeen{}) == std::vector<DamageKind>{DamageKind::Melee});

    REQUIRE(KindOfResist(Resist::Shock) == DamageKind::Shock);
    REQUIRE(KindOfResist(Resist::Other) == DamageKind::Magic);
}
