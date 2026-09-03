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
class MagicItem;
} // namespace RE

namespace ft::game
{

// SkyUI's tabs, less Favorites: a follower has none.
enum class ItemCategory : std::uint8_t
{
    Weapons,
    Apparel,
    Potions,
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
    // A thing held in a hand -- weapon, shield, torch -- as opposed to worn;
    // and one that only one particular hand takes: a shield or a torch on
    // the left, a mod's right-hand armour on the right.
    bool handItem{false};
    bool leftOnly{false};
    bool rightOnly{false};
    // For a weapon, shield or torch: which hand holds it.
    bool equippedLeft{false};
    bool equippedRight{false};
    bool pinnedLeft{false};
    bool pinnedRight{false};
    // Something she can put on: a weapon, a piece of apparel, ammunition, a
    // torch. Only these take a click in the Worn column.
    bool equipable{false};
    // Kept on by us: the tick puts it back whenever the game takes it off.
    bool pinned{false};
    // Kept from the combat AI while a pinned spell holds a hand this would
    // take: pruned from the AI's list of options in a fight. Still hers.
    bool setAside{false};

    // The detail page. Numbers as sections in the style of the character
    // sheet; prose beneath them, each drawn under its own heading when it is
    // not empty. `effects` is one line per effect -- an enchantment's on a
    // weapon or armour, the item's own on a potion, scroll or ingredient --
    // with the magnitude and duration filled in as the item card does.
    std::vector<SheetSection> detail;
    std::string description;
    std::string effects;
};

// One line per effect of a spell, potion or enchantment: its description
// with the magnitude and duration filled in, as the item card shows it.
[[nodiscard]] std::string EffectLines(const RE::MagicItem *magic);

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
