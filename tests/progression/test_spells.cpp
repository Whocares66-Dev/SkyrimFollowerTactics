#include "progression/core/Companion.h"
#include "progression/core/Spells.h"

#include <catch2/catch_test_macros.hpp>

namespace
{

using fp::Companion;
using fp::SpellForgotten;
using fp::TomeRead;

const fp::SpellFacts kFlames{{"Skyrim.esm", 0x012FCD}, "Flames"};
const fp::SpellFacts kSparks{{"Skyrim.esm", 0x02B96B}, "Sparks"};

Companion Marcurio()
{
    return fp::Enroll({"Skyrim.esm", 0x0B9986}, "Marcurio");
}

} // namespace

TEST_CASE("Learn asks only that they do not know the spell, as the player's reading does", "[spells]")
{
    const auto can = fp::LearnButton("Flames", false);
    CHECK(can.can);
    CHECK(can.hover == "Click to learn Flames");
    const auto known = fp::LearnButton("Flames", true);
    CHECK_FALSE(known.can);
    CHECK(known.hover == "Already knows Flames");

    const auto forget = fp::ForgetButton("Sparks");
    CHECK(forget.can);
    CHECK(forget.hover == "Click to forget Sparks");
}

TEST_CASE("a tome of a spell they do not know teaches it; of one they know, nothing", "[spells]")
{
    Companion c = Marcurio();
    CHECK(fp::ReadTome(c, kFlames, false) == TomeRead::Taught);
    CHECK(fp::Taught(c, kFlames.spell));
    REQUIRE(c.spells.size() == 1);
    CHECK(c.spells[0].name == "Flames");

    // Known now, through the view: a second tome does nothing.
    CHECK(fp::ReadTome(c, kFlames, true) == TomeRead::Known);
    CHECK(c.spells.size() == 1);

    // One of their own, known: nothing either, and nothing recorded.
    CHECK(fp::ReadTome(c, kSparks, true) == TomeRead::Known);
    CHECK_FALSE(fp::Taught(c, kSparks.spell));
    CHECK(c.spellsSetAside.empty());

    // A spell with no form to name is nothing to learn.
    CHECK(fp::ReadTome(c, {}, false) == TomeRead::Known);
    CHECK(c.spells.size() == 1);
}

TEST_CASE("forgetting takes back a spell taught here, and sets aside any other", "[spells]")
{
    Companion c = Marcurio();
    fp::ReadTome(c, kFlames, false);

    // Taught here: out of the ledger, and not set aside -- there is nothing
    // of theirs to hide.
    CHECK(fp::ForgetSpell(c, kFlames, true) == SpellForgotten::Forgotten);
    CHECK_FALSE(fp::Taught(c, kFlames.spell));
    CHECK_FALSE(fp::IsSpellSetAside(c, kFlames.spell));

    // Theirs -- their record's, their race's, a quest's: set aside, so the
    // engine is told they do not know it.
    CHECK(fp::ForgetSpell(c, kSparks, true) == SpellForgotten::SetAside);
    CHECK(fp::IsSpellSetAside(c, kSparks.spell));
    REQUIRE(c.spellsSetAside.size() == 1);
    CHECK(c.spellsSetAside[0].name == "Sparks");

    // Not known now, through the view: forgetting again does nothing.
    CHECK(fp::ForgetSpell(c, kSparks, false) == SpellForgotten::NotKnown);
    CHECK(c.spellsSetAside.size() == 1);
    CHECK(fp::ForgetSpell(c, kFlames, false) == SpellForgotten::NotKnown);
    CHECK(c.spells.empty());
}

TEST_CASE("a spell forgotten comes back with a tome of it, whoever's it was", "[spells]")
{
    Companion c = Marcurio();

    // Their own: set aside, then the tome takes it up again -- restored,
    // not taught, so the ledger is as it was.
    fp::ForgetSpell(c, kSparks, true);
    CHECK(fp::ReadTome(c, kSparks, false) == TomeRead::Restored);
    CHECK_FALSE(fp::IsSpellSetAside(c, kSparks.spell));
    CHECK_FALSE(fp::Taught(c, kSparks.spell));
    CHECK(c.spells.empty());
    CHECK(c.spellsSetAside.empty());

    // Taught here: forgotten, then taught again.
    fp::ReadTome(c, kFlames, false);
    fp::ForgetSpell(c, kFlames, true);
    CHECK(fp::ReadTome(c, kFlames, false) == TomeRead::Taught);
    CHECK(fp::Taught(c, kFlames.spell));
    CHECK(c.spells.size() == 1);
}
