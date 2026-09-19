// The followers' menu entries: who gets which slot, and when.

#include <catch2/catch_test_macros.hpp>

#include "core/MenuSlots.h"

#include <vector>

using namespace ft;

namespace
{
constexpr ActorId kLydia = 0xA2C94;
constexpr ActorId kJenassa = 0x1348A;
constexpr ActorId kSerana = 0x2B74;
} // namespace

TEST_CASE("newcomers wait for the party to settle, then take the first free slots in name order", "[menuslots]")
{
    MenuSlots slots(3);
    const std::vector<MenuSlot> two{{kLydia, "Lydia"}, {kJenassa, "Jenassa"}};
    // Seen at 10: held.
    REQUIRE(slots.Arrivals(two, 10.0, 2.0).empty());
    REQUIRE(slots.Arrivals(two, 11.0, 2.0).empty());
    // Serana appears at 11.5: the settle starts over.
    const std::vector<MenuSlot> three{{kLydia, "Lydia"}, {kJenassa, "Jenassa"}, {kSerana, "Serana"}};
    REQUIRE(slots.Arrivals(three, 11.5, 2.0).empty());
    REQUIRE(slots.Arrivals(three, 13.0, 2.0).empty());
    // Settled: one batch, alphabetical, into slots 0, 1, 2.
    const auto placed = slots.Arrivals(three, 13.5, 2.0);
    REQUIRE(placed.size() == 3);
    REQUIRE(placed[0].who.name == "Jenassa");
    REQUIRE(placed[0].slot == 0);
    REQUIRE(placed[1].who.name == "Lydia");
    REQUIRE(placed[1].slot == 1);
    REQUIRE(placed[2].who.name == "Serana");
    REQUIRE(placed[2].slot == 2);
    REQUIRE(slots.OwnerOf(0) == kJenassa);
    REQUIRE(slots.NameOf(1) == "Lydia");
    REQUIRE(slots.OwnerOf(3) == 0);
    // Nothing new: nothing.
    REQUIRE(slots.Arrivals(three, 20.0, 2.0).empty());
}

TEST_CASE("a dismissed follower's slot is freed only when the entry could be deleted", "[menuslots]")
{
    MenuSlots slots(2);
    const std::vector<MenuSlot> both{{kLydia, "Lydia"}, {kJenassa, "Jenassa"}};
    REQUIRE(slots.Arrivals(both, 0.0, 0.0).size() == 2);
    // Lydia dismissed.
    const std::vector<ActorId> present{kJenassa};
    const auto gone = slots.Gone(present);
    REQUIRE(gone == std::vector<std::size_t>{1});
    // The framework refused: the slot stays Lydia's, and Lydia coming
    // back is no newcomer.
    REQUIRE(slots.OwnerOf(1) == kLydia);
    REQUIRE(slots.Arrivals(both, 5.0, 0.0).empty());
    // Freed: a newcomer takes it.
    slots.Free(1);
    REQUIRE(slots.OwnerOf(1) == 0);
    const std::vector<MenuSlot> withSerana{{kJenassa, "Jenassa"}, {kSerana, "Serana"}};
    const auto placed = slots.Arrivals(withSerana, 6.0, 0.0);
    REQUIRE(placed.size() == 1);
    REQUIRE(placed[0].slot == 1);
    REQUIRE(placed[0].who.id == kSerana);
}

TEST_CASE("with every slot taken a newcomer is returned with no slot", "[menuslots]")
{
    MenuSlots slots(1);
    const std::vector<MenuSlot> one{{kLydia, "Lydia"}};
    REQUIRE(slots.Arrivals(one, 0.0, 0.0)[0].slot == 0);
    const std::vector<MenuSlot> two{{kLydia, "Lydia"}, {kJenassa, "Jenassa"}};
    const auto placed = slots.Arrivals(two, 1.0, 0.0);
    REQUIRE(placed.size() == 1);
    REQUIRE(placed[0].slot == slots.Count());
    REQUIRE(placed[0].who.id == kJenassa);
    REQUIRE(slots.OwnerOf(0) == kLydia);
}
