#pragma once
// A follower's inventory, worded for the panel.
//
// Built on the game thread from the live actor and copied to the render thread
// with the rest of the view, so the panel can list what she carries without
// touching an RE:: type -- and without the player having to open a trade
// dialogue to find out.

#include "core/Loadout.h"
#include "game/Sensors.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class Effect;
class MagicItem;
class TESDescription;
} // namespace RE

namespace ft::game
{

// SkyUI's tabs, less Favorites: a follower has none; plus Arrows, which
// SkyUI folds into Weapons. Ammunition has no equip slot at all -- neither
// hand, not worn -- so its own list is the one with a plain Equipped
// column, and Weapons keeps just Left and Right.
enum class ItemCategory : std::uint8_t
{
    Weapons,
    Arrows,
    Armor,
    Potions,
    Poisons,
    Food,
    Ingredients,
    Scrolls,
    Books,
    Keys,
    Misc,
    COUNT
};

[[nodiscard]] const char *DisplayName(ItemCategory category);

struct InventoryItem
{
    std::uint32_t form{0};
    // As the game would show it: a renamed or player-enchanted piece keeps its
    // given name rather than reverting to the record's.
    std::string name;
    // The Type column: "Sword", "Light Boots", "Poison".
    std::string type;
    ItemCategory category{ItemCategory::Misc};
    int count{0};
    float weight{0.0f}; // of one
    int value{0};       // of one, enchantment included
    float damage{0.0f}; // a weapon's damage in her hands, or ammunition's; 0 for the rest
    float armor{0.0f};  // a piece of armour's rating on her; 0 for the rest
    bool worn{false};
    bool enchanted{false};
    // A Daedric artifact (the DaedricArtifact keyword, or the vendor one a
    // few Creation Club pieces carry instead): named in gold.
    bool artifact{false};
    // A weapon with a poison on it: the poison's effects, one per line,
    // for the detail page's Poison section; empty for none. The row shows
    // a poison glyph after the name.
    std::string poisonEffects;
    // A thing held in a hand -- weapon, shield, torch -- as opposed to worn;
    // and one that only one particular hand takes: a shield or a torch on
    // the left, a mod's right-hand armour on the right.
    bool handItem{false};
    bool leftOnly{false};
    bool rightOnly{false};
    // Which hands its record lets it take, for the equip menu's hand lists.
    ft::Grip grip{ft::Grip::None};
    // For a weapon, shield or torch: which hand holds it.
    bool equippedLeft{false};
    bool equippedRight{false};
    bool pinnedLeft{false};
    bool pinnedRight{false};
    // Something she can put on: a weapon, a piece of armour, ammunition, a
    // torch. Only these take a click in the Worn column.
    bool equipable{false};
    // Kept on by us: the tick puts it back whenever the game takes it off.
    bool pinned{false};
    // Never to be used: the combat AI scores it zero, the engine's equips of
    // it are refused, and it comes off if found on. Off, and kept off.
    bool banned{false};
    // Kept from the combat AI while a pinned spell holds a hand this would
    // take: it scores zero whenever the combat AI asks. Still carried.
    bool setAside{false};
    // Why, when it is: one line per pin that holds a hand it could take.
    std::string asideBy;

    // The detail page. Numbers as sections in the style of the character
    // sheet; prose beneath them, each drawn under its own heading when it is
    // not empty. `effects` is one line per effect -- an enchantment's on a
    // weapon or armour, the item's own on a potion, scroll or ingredient --
    // with the magnitude and duration filled in as the item card does.
    std::vector<SheetSection> detail;
    std::string description;
    std::string effects;
    // For a potion, poison, food or ingredient: what it does, the effects'
    // names in one line -- "Restore Health", "Fortify Health, Restore
    // Magicka" -- the list's column in place of a Type that would only
    // repeat the heading. An ingredient's is the one eating it gives, the
    // first.
    std::string effect;
};

// One line per effect of a spell, potion or enchantment: its description
// with the magnitude and duration filled in, as the item card shows it.
[[nodiscard]] std::string EffectLines(const RE::MagicItem *magic);
// The same for a spell the actor casts: <mag> and <dur> are what THEY would
// get, perks and Fortify effects applied, not the record's base numbers.
[[nodiscard]] std::string EffectLines(RE::Actor *caster, RE::MagicItem *spell);

// An effect's magnitude and duration as this actor casts it. The record's
// number, put through the perk entry points the engine applies when the
// effect is made (ModSpellMagnitude, ModSpellDuration): real perks such as
// Augmented Flames and Necromage, and the hidden per-actor perks that turn
// the Fortify <School> actor values into the same entry points -- the
// PowerModifier for potions, the Modifier for enchantments (docs/RESEARCH.md
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

// Everything she carries that the game would list, sorted by name. Nameless
// entries and armour or weapons flagged non-playable are left out, as the
// container menu leaves them out.
[[nodiscard]] std::vector<InventoryItem> ScanInventory(RE::Actor *actor);

} // namespace ft::game
