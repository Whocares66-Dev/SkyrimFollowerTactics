#pragma once
// The Skills tab: the skills, each skill's perk tree as the perk menu
// draws it, and a page per perk.

#include "core/BagView.h"
#include "core/Blows.h"
#include "core/Breakdown.h"
#include "core/Effects.h"
#include "core/Rule.h"
#include "core/Snapshot.h"
#include "core/Views.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
class BGSAttackData;
struct Effect;
class InventoryEntryData;
class MagicItem;
class SpellItem;
class TESObjectARMO;
class TESObjectWEAP;
} // namespace RE

namespace ft::game
{

// The Skills tab: the eighteen skills grouped as the game groups them, with
// any fortify or potion modifier folded into the same line.
[[nodiscard]] std::vector<SheetSection> BuildSkillSheet(RE::Actor *actor);

// One perk's page: what the record says. The Perk section (id, rank,
// skill) and an Entries section, one row per thing the perk does -- an
// entry point with its function, an ability it grants, a quest it
// advances -- with the description beneath as prose.
struct PerkPage
{
    std::uint32_t form{0};
    [[nodiscard]] std::uint32_t Key() const noexcept
    {
        return form;
    }
    std::string name;
    std::string description;
    // "Perk Details", the facts; then "Effects", one row per entry -- what it does,
    // a tick in the mark while it is active -- opening on the conditions
    // that gate it, as the engine reads them for this actor: the call, the
    // comparison, a tick when met. Not the record's own conditions: those
    // are the skill tree's, for the player.
    std::vector<SheetSection> sections;
};

// A page for every perk the follower holds, in a skill's tree or loose, and
// for every tree perk they do not, by its first rank (the skill page's names
// open any of them).
[[nodiscard]] std::vector<PerkPage> BuildPerkPages(RE::Actor *actor);

// Every skill's perk tree as the perk menu draws it, and what the actor
// holds of each node, as the engine answers HasPerk -- so a perk another
// plugin gives or hides through the engine reads as the game reads it. The
// Skills tab's skill page; the row's `tree` names one by its key.
[[nodiscard]] std::vector<ft::PerkTreeView> BuildPerkTrees(RE::Actor *actor);

// A perk's name, trimmed, because the records are not: Skyrim.esm's first
// rank of Magic Resistance is named " Magic Resistance", leading space and
// all, and on screen that reads as a row set in for no reason. Empty for
// a perk with no name.
[[nodiscard]] std::string PerkName(const RE::BGSPerk *perk);
} // namespace ft::game
