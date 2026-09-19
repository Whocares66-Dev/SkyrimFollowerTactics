#pragma once
// A follower's inventory, worded for the panel.
//
// Built on the game thread from the live actor and copied to the render thread
// with the rest of the view, so the panel can list what they carry without
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
struct Effect;
class MagicItem;
class TESDescription;
} // namespace RE

namespace ft::game
{

// The categories the Inventory tab sorts a bag into. A follower has no
// favourites, so there is no such list, and arrows are a category of their
// own rather than part of Weapons: ammunition has no equip slot at all --
// neither hand, not worn -- so its own list is the one with a plain
// Equipped column, and Weapons keeps just Left and Right.
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
    // Which of the form's copies this row is. The bag keeps one entry per
    // form; the copies that stand apart from the rest -- enchanted at an
    // enchanter, renamed, tempered, poisoned, charged, holding a soul -- are
    // each a row of their own, as the game's menu shows them, numbered from
    // 1 in the entry's order. 0 is the plain stack. So the form repeats
    // across rows, and a row is named by Key(); a pin, a ban and a rule's
    // equip name the row's variant (below), or the form for every row.
    std::uint32_t stack{0};
    // The row's variant: what a pin, a ban or a rule on this row names.
    // Coarser than the row: the poisoned dagger and the clean stack are one
    // variant.
    ft::ItemVariant variant;
    // The list this row is, for a row that stands apart from the plain
    // stack; null for the stack. Not to be read outside the game thread:
    // it is a token handed back with a click, and checked against the
    // bag's lists before it is used, since the copy may have left.
    RE::ExtraDataList *row{nullptr};
    [[nodiscard]] std::uint64_t Key() const
    {
        return (static_cast<std::uint64_t>(stack) << 32) | form;
    }
    // As the game would show it: a renamed or player-enchanted piece keeps its
    // given name rather than reverting to the record's.
    std::string name;
    // The Type column: "Sword", "Light Boots", "Poison".
    std::string type;
    ItemCategory category{ItemCategory::Misc};
    int count{0};
    float weight{0.0f}; // of one
    int value{0};       // of one, enchantment included
    float damage{0.0f}; // a weapon's damage in their hands, or ammunition's; 0 for the rest
    float armor{0.0f};  // a piece of armour's rating on them; 0 for the rest
    bool worn{false};
    bool enchanted{false};
    // A Daedric artifact (the DaedricArtifact keyword, or the vendor one a
    // few Creation Club pieces carry instead): named in gold, with a crown.
    bool artifact{false};
    // A copy on the row is stolen: a red hand after the name. The row's mark
    // only, never a part of the variant: a rule's "equip Iron Dagger (+7)"
    // takes a stolen one (dev/UNIQUE.md).
    bool stolen{false};
    // A poison on the weapon: one row, its name and the hits left, under
    // headings of its own, and its effects in the enchantment's table
    // shape. No rows for a clean weapon. The list shows a poison glyph
    // after the name.
    SheetSection poison;
    SheetSection poisonEffects;
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
    // Something they can put on: a weapon, a piece of armour, ammunition, a
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
    // The enchantment as one row, its name and its charge, under headings
    // of its own; no rows for an item without one.
    SheetSection enchantment;
    // What the enchantment, the potion, the scroll does: the table of
    // effects (EffectsOf), each row's description as the item card shows
    // it in the table's last column.
    SheetSection effectsTable;
    // For a potion, poison, food or ingredient: what it is for, its first
    // effect's name -- "Restore Health" -- the list's column in place of a
    // Type that would only repeat the heading. The rest are on its page.
    std::string effect;
    // For a scroll, as a spell's list has them: how it is cast (Self,
    // Projectile, Target ...) and the costliest effect's magnitude as the
    // follower would cast it.
    std::string cast;
    float magnitude{0.0f};
};

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
