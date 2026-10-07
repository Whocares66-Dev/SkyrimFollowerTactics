#include "core/Places.h"

#include "Build.h"
#include "core/Evaluator.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace ft;

TEST_CASE("physical enclosure follows reviewed worldspaces before lighting flags", "[places]")
{
    REQUIRE(PlaceContextOf(false, false, true, true) == PlaceContext::NoCell);
    REQUIRE(PlaceContextOf(true, false, false) == PlaceContext::Enclosed);
    REQUIRE(PlaceContextOf(false, true, false) == PlaceContext::OpenAir);
    REQUIRE(PlaceContextOf(false, true, true) == PlaceContext::Enclosed);
    // Blackreach and Darkwater are enclosed; the lit Volkihar courtyard is not.
    const auto context = [](std::string_view plugin, std::uint32_t form, bool roofed) {
        const auto found = std::ranges::find_if(
            kWorldEnclosures, [=](const auto &world) { return world.plugin == plugin && world.localForm == form; });
        REQUIRE(found != std::end(kWorldEnclosures));
        return PlaceContextOf(false, true, roofed, found->enclosed);
    };
    REQUIRE(context("Skyrim.esm", 0x01EE62, false) == PlaceContext::Enclosed);
    REQUIRE(context("Skyrim.esm", 0x02C965, false) == PlaceContext::Enclosed);
    REQUIRE(context("Dawnguard.esm", 0x007202, true) == PlaceContext::OpenAir);
    REQUIRE(context("Dawnguard.esm", 0x001408, true) == PlaceContext::OpenAir);
}

TEST_CASE("Embershard's shared location separates its entrance from its interior", "[places]")
{
    using K = LocationKind;
    // houseCARL: one LCTN for both, Dungeon + BanditCamp + Cave.
    const std::uint64_t embershard = Bit(K::Dungeon) | Bit(K::BanditCamp) | Bit(K::Cave);
    REQUIRE(SitePlaces(embershard, PlaceContext::Enclosed) == (Bit(K::Dungeon) | Bit(K::BanditHideout) | Bit(K::Cave)));
    REQUIRE(SitePlaces(embershard, PlaceContext::OpenAir) == (Bit(K::Camp) | Bit(K::BanditCamp)));
    REQUIRE(SitePlaces(embershard, PlaceContext::NoCell) == 0);

    // Run the actual idle rule and its negation across the doorway.
    Snapshot snapshot = ft::test::Healthy();
    snapshot.inCombat = false;
    Rule rule;
    rule.predicate = PredicateKind::Location;
    rule.locationKind = K::Dungeon;
    rule.FirstAction().kind = ActionKind::DrinkStrongest;
    rule.FirstAction().effect = "Restore Magicka";
    RuleSet rules;
    rules.moment = Moment::Idle;
    rules.rules.push_back(rule);
    const auto holds = [&](PlaceContext context, K kind, bool negated = false) {
        snapshot.places = SitePlaces(embershard, context);
        rules.rules[0].locationKind = kind;
        rules.rules[0].negated = negated;
        EvalContext eval;
        return Evaluate(rules, snapshot, eval).Fired();
    };
    REQUIRE(holds(PlaceContext::Enclosed, K::Dungeon));
    REQUIRE_FALSE(holds(PlaceContext::OpenAir, K::Dungeon));
    REQUIRE(holds(PlaceContext::OpenAir, K::Dungeon, true));
    REQUIRE_FALSE(holds(PlaceContext::Enclosed, K::Camp));
    REQUIRE(holds(PlaceContext::OpenAir, K::BanditCamp));
    REQUIRE(holds(PlaceContext::Enclosed, K::BanditHideout));
}

TEST_CASE("outdoor sites are camps even without Bethesda's dungeon keyword", "[places]")
{
    using K = LocationKind;
    REQUIRE(SitePlaces(Bit(K::BanditCamp), PlaceContext::OpenAir) == (Bit(K::Camp) | Bit(K::BanditCamp)));
    REQUIRE(SitePlaces(Bit(K::ForswornCamp), PlaceContext::OpenAir) == (Bit(K::Camp) | Bit(K::ForswornCamp)));
    REQUIRE(SitePlaces(Bit(K::Dungeon) | Bit(K::GiantCamp), PlaceContext::OpenAir) ==
            (Bit(K::Camp) | Bit(K::GiantCamp)));
    REQUIRE(SitePlaces(Bit(K::SprigganGrove), PlaceContext::OpenAir) == (Bit(K::Camp) | Bit(K::SprigganGrove)));
    // Guardian Stones have no site keywords, nor does the road between sites.
    REQUIRE(SitePlaces(0, PlaceContext::OpenAir) == 0);
    // Ordinary buildings retain their existing meaning.
    const auto house = Bit(K::Building) | Bit(K::House) | Bit(K::Home);
    REQUIRE(SitePlaces(house, PlaceContext::Enclosed) == house);
    const auto inn = Bit(K::Building) | Bit(K::Inn);
    REQUIRE(SitePlaces(inn, PlaceContext::Enclosed) == inn);
    // A cave built from exterior cells still uses its enclosed context.
    const auto blackreach = Bit(K::Dungeon) | Bit(K::Cave) | Bit(K::FalmerHive);
    REQUIRE(SitePlaces(blackreach, PlaceContext::Enclosed) == blackreach);
}

TEST_CASE("reviewed underground locations stop their parent town and correct wrong structural tags", "[places]")
{
    using K = LocationKind;
    const auto correction = [](std::uint32_t form) -> const LocationCorrection & {
        const auto found = std::ranges::find_if(kLocationCorrections, [=](const auto &entry) {
            return entry.localForm == form && entry.plugin == "Skyrim.esm";
        });
        REQUIRE(found != std::end(kLocationCorrections));
        return *found;
    };
    // The Midden is tagged as a guild dwelling; its parent is Winterhold.
    const ChainLink midden[] = {CorrectPlace({Bit(K::Building) | Bit(K::Guild)}, correction(0x02BCEB)),
                                {Bit(K::Town) | Bit(K::Settlement)}};
    REQUIRE(SitePlaces(PlacesUp(midden), PlaceContext::Enclosed) == Bit(K::Dungeon));
    // The Ratway has no dungeon keyword (CC even adds homestead tags).
    const ChainLink ratway[] = {CorrectPlace({}, correction(0x03B871)), {Bit(K::City) | Bit(K::Settlement)}};
    REQUIRE(SitePlaces(PlacesUp(ratway), PlaceContext::Enclosed) == Bit(K::Dungeon));
    const ChainLink vault[] = {CorrectPlace({}, correction(0x022639)), {Bit(K::City) | Bit(K::Settlement)}};
    REQUIRE(SitePlaces(PlacesUp(vault), PlaceContext::Enclosed) == Bit(K::Dungeon));
    const ChainLink cidhna[] = {CorrectPlace({Bit(K::Building)}, correction(0x018C91)),
                                {Bit(K::City) | Bit(K::Settlement)}};
    REQUIRE(SitePlaces(PlacesUp(cidhna), PlaceContext::Enclosed) == Bit(K::Dungeon));
    const ChainLink ruins[] = {CorrectPlace({}, correction(0x0E2502)), {Bit(K::City) | Bit(K::Settlement)}};
    REQUIRE(SitePlaces(PlacesUp(ruins), PlaceContext::Enclosed) ==
            (Bit(K::Dungeon) | Bit(K::Ruin) | Bit(K::DwarvenRuin)));
    const auto brokenFang = CorrectPlace({Bit(K::Dungeon) | Bit(K::VampireLair) | Bit(K::Fort)}, correction(0x01914D));
    REQUIRE(SitePlaces(brokenFang.kinds, PlaceContext::Enclosed) ==
            (Bit(K::Dungeon) | Bit(K::VampireLair) | Bit(K::Cave)));
    // Bloodlet Throne's fort tag is valid; do not lump it in with Broken Fang.
    const auto bloodlet = Bit(K::Dungeon) | Bit(K::VampireLair) | Bit(K::Fort);
    REQUIRE(SitePlaces(bloodlet, PlaceContext::Enclosed) == bloodlet);
    const auto temple = CorrectPlace({Bit(K::Dungeon) | Bit(K::WarlockLair) | Bit(K::Fort)}, correction(0x0192B9));
    REQUIRE(SitePlaces(temple.kinds, PlaceContext::Enclosed) ==
            (Bit(K::Dungeon) | Bit(K::WarlockLair) | Bit(K::Building) | Bit(K::Temple)));
    // A bad added tag cannot turn the Guardian Stones into a dungeon/camp.
    const auto stones = CorrectPlace({Bit(K::Dungeon) | Bit(K::Cave)}, correction(0x10FE43));
    REQUIRE(SitePlaces(stones.kinds, PlaceContext::OpenAir) == 0);
}

TEST_CASE("every site occupant has exactly one indoor and one outdoor classification", "[places]")
{
    using K = LocationKind;
    std::uint64_t seen = Bit(K::Dungeon) | Bit(K::Camp);
    for (const auto &pair : kSiteKinds)
    {
        const auto bits = Bit(pair.dungeon) | Bit(pair.camp);
        REQUIRE((seen & bits) == 0);
        seen |= bits;
        REQUIRE(GroupOf(pair.dungeon) == LocationGroup::Dungeon);
        REQUIRE(GroupOf(pair.camp) == LocationGroup::Camp);
        REQUIRE(SitePlaces(bits, PlaceContext::Enclosed) == (Bit(K::Dungeon) | Bit(pair.dungeon)));
        REQUIRE(SitePlaces(bits, PlaceContext::OpenAir) == (Bit(K::Camp) | Bit(pair.camp)));
        REQUIRE(SitePlaces(bits, PlaceContext::NoCell) == 0);
    }
    for (unsigned i = 0; i < static_cast<unsigned>(K::COUNT); ++i)
    {
        const auto kind = static_cast<K>(i);
        if (GroupOf(kind) == LocationGroup::Dungeon || GroupOf(kind) == LocationGroup::Camp)
            REQUIRE((seen & Bit(kind)) != 0);
    }
}
