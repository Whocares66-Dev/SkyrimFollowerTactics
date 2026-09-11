#pragma once
// What the editor asks of a rule against what the follower has: a rule
// naming a thing the follower no longer has -- the potion drunk up, the
// spell forgotten, the sword sold -- or a follower who is away, is set
// aside in the panel, its switch dead and the row dimmed, until the thing
// or the follower is back. Decided here, where it is tested; the panel
// only draws the answer.

#include "Kinds.h"
#include "Rule.h"
#include "Snapshot.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ft
{

// What a follower has, as the editor's menus offer it and as a rule is
// greyed by. The tick fills one per follower from the same scans that fill
// the menus.
struct Holdings
{
    ActorId self{0};
    // The other followers with us, whom a rule may name.
    std::vector<ActorId> peers;
    // The consumables carried -- potions, food, ingredients; a poison goes
    // on a weapon and is not asked about -- with the effects a policy
    // chooses by.
    struct Consumable
    {
        std::uint32_t form{0};
        ConsumableKind kind{ConsumableKind::Potion};
        std::vector<std::string> effects;
    };
    std::vector<Consumable> consumables;
    // The spells, powers, shouts and scrolls the cast menu offers (a spell
    // above the follower's skill is not among them).
    std::vector<std::uint32_t> castable;
    // The items carried and the spells known, as the equip menus offer them.
    std::vector<std::uint32_t> things;
};

// Does the follower have what the action names? A drink or eat policy
// names an effect, and has it while some carried thing of its kind does;
// a named consumable, an equip, a cast names a form. An action naming
// nothing -- an equip of "none", a blow -- always does.
[[nodiscard]] bool ActionHad(const Action &action, const Holdings &has);

// Is the follower the condition names with us: the subject, or the party
// member an Attacking or Attacked by asks about? The player and the
// follower themself always are. And the one the actions are aimed at?
[[nodiscard]] bool ConditionHad(const Rule &rule, const Holdings &has);
[[nodiscard]] bool TargetHad(const Rule &rule, const Holdings &has);

// Why a rule is set aside, or None. A follower away is said first: the
// whole rule waits on them, whatever else it names.
enum class Aside : std::uint8_t
{
    None,
    FollowerAway,
    NotHad
};
[[nodiscard]] Aside RuleSetAside(const Rule &rule, const Holdings &has);

} // namespace ft
