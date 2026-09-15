// The rules of a swing and a bash, from what the hands hold: vanilla's,
// which the game side maps to the race's attack events.
#include "core/Blows.h"

#include <catch2/catch_test_macros.hpp>

using namespace ft;

TEST_CASE("a power attack is the right hand's blade, else the left's, else the fists", "[blows]")
{
    REQUIRE(SwingWith({Held::OneHander, Held::Nothing}) == Swing::Right);
    REQUIRE(SwingWith({Held::TwoHander, Held::Nothing}) == Swing::Right);
    REQUIRE(SwingWith({Held::OneHander, Held::Shield}) == Swing::Right);
    REQUIRE(SwingWith({Held::OneHander, Held::Spell}) == Swing::Right);
    REQUIRE(SwingWith({Held::OneHander, Held::OneHander}) == Swing::Both);
    // The right hand swinging nothing, the left's blade swings, whatever the
    // right holds.
    REQUIRE(SwingWith({Held::Nothing, Held::OneHander}) == Swing::Left);
    REQUIRE(SwingWith({Held::Spell, Held::OneHander}) == Swing::Left);
    REQUIRE(SwingWith({Held::Staff, Held::OneHander}) == Swing::Left);
    REQUIRE(SwingWith({Held::Nothing, Held::Nothing}) == Swing::Fists);
    // A bow, a staff, a spell or a shield swings nothing, and a hand holding
    // one is not a fist.
    REQUIRE(SwingWith({Held::Bow, Held::Nothing}) == Swing::None);
    REQUIRE(SwingWith({Held::Staff, Held::Nothing}) == Swing::None);
    REQUIRE(SwingWith({Held::Spell, Held::Shield}) == Swing::None);
    REQUIRE(SwingWith({Held::Spell, Held::Spell}) == Swing::None);
    REQUIRE(SwingWith({Held::Nothing, Held::Shield}) == Swing::None);
    REQUIRE(SwingWith({Held::Nothing, Held::Staff}) == Swing::None);
}

TEST_CASE("a bash is what blocks: a shield or torch, or the right hand's weapon with the left empty", "[blows]")
{
    REQUIRE(BashesWith({Held::Spell, Held::Shield}));
    REQUIRE(BashesWith({Held::OneHander, Held::Shield}));
    REQUIRE(BashesWith({Held::OneHander, Held::Torch}));
    REQUIRE(BashesWith({Held::OneHander, Held::Nothing}));
    REQUIRE(BashesWith({Held::TwoHander, Held::Nothing}));
    REQUIRE(BashesWith({Held::Bow, Held::Nothing}));
    REQUIRE(BashesWith({Held::Staff, Held::Nothing}));
    // A weapon alone in the left hand cannot block, nor two hands each
    // holding something, nor the fists, nor a spell hand.
    REQUIRE_FALSE(BashesWith({Held::Nothing, Held::Staff}));
    REQUIRE_FALSE(BashesWith({Held::Nothing, Held::OneHander}));
    REQUIRE_FALSE(BashesWith({Held::OneHander, Held::OneHander}));
    REQUIRE_FALSE(BashesWith({Held::OneHander, Held::Staff}));
    REQUIRE_FALSE(BashesWith({Held::OneHander, Held::Spell}));
    REQUIRE_FALSE(BashesWith({Held::Staff, Held::Staff}));
    REQUIRE_FALSE(BashesWith({Held::Nothing, Held::Nothing}));
    REQUIRE_FALSE(BashesWith({Held::Spell, Held::Nothing}));
    REQUIRE_FALSE(BashesWith({Held::Spell, Held::Spell}));
}
