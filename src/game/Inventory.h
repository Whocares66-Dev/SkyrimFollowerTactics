#pragma once
// A follower's inventory, worded for the panel.
//
// Built on the game thread from the live actor and copied to the render thread
// with the rest of the view, so the panel can list what they carry without
// touching an RE:: type -- and without the player having to open a trade
// dialogue to find out.

#include "core/Loadout.h"
#include "core/Views.h"
#include "game/Sensors.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
class Actor;
struct Effect;
class MagicItem;
class TESDescription;
} // namespace RE

namespace ft::game
{

// The tab's name for a category.
[[nodiscard]] const char *DisplayName(ItemCategory category);

// An effect's magnitude and duration as this actor casts it. The record's
// number, put through the perk entry points the engine applies when the
// effect is made (ModSpellMagnitude, ModSpellDuration): real perks such as
// Augmented Flames and Necromage, and the hidden per-actor perks that turn
// the Fortify <School> actor values into the same entry points -- the
// PowerModifier for potions, the Modifier for enchantments (dev/RESEARCH.md
// 6). What it does NOT include is dual casting, a flag of the cast itself.
[[nodiscard]] float ActualMagnitude(RE::Actor *caster, RE::MagicItem *spell, const RE::Effect *effect);
[[nodiscard]] float ActualDuration(RE::Actor *caster, RE::MagicItem *spell, const RE::Effect *effect);

// A spell's description with <mag>, <dur> and <area> filled for THIS caster,
// from the costliest effect, as the engine fills them -- for the player. The
// engine's own substitution is the player's wherever it runs (the magic
// menu is the player's), so a follower's page asks for the text with no
// parent, which leaves the tokens in, and fills them with the same numbers
// the effect lines above it carry. Falls back to the engine's text when the
// tokens have already been filled.
[[nodiscard]] std::string DescriptionFor(RE::Actor *caster, RE::MagicItem *spell, RE::TESDescription &description);

// The hands a piece of armour takes, read from its equip slot and that
// slot's parents rather than from a list of known shields: the game's
// Shield slot is a child of LeftHand, and a mod's hand-held armour names a
// hand slot itself or one beneath it. Body armour takes none.
[[nodiscard]] ft::Grip ArmorGrip(const RE::TESObjectARMO *armor);

// The hands a spell takes, by its equip slot. Most spells take either and a
// master spell both at once; one locked to a hand is an NPC-only variant
// carrying the player's spell's display name (Marcurio's Lightning Bolt), so
// the name never tells it from the one that fits either.
[[nodiscard]] ft::Grip SpellGrip(const RE::SpellItem *spell);

// Everything they carry that the game would list, sorted by name. Nameless
// entries and armour or weapons flagged non-playable are left out, as the
// container menu leaves them out.
[[nodiscard]] std::vector<InventoryItem> ScanInventory(RE::Actor *actor);

} // namespace ft::game
