#pragma once
// A follower's inventory, worded for the panel.
//
// Built on the game thread from the live actor and copied to the render thread
// with the rest of the view, so the panel can list what she carries without
// touching an RE:: type -- and without the player having to open a trade
// dialogue to find out.

#include "game/Sensors.h"

#include <cstdint>
#include <string>
#include <vector>

namespace RE
{
class Actor;
}

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
    // Something she can put on: a weapon, a piece of apparel, ammunition, a
    // torch. Only these take a click in the Worn column.
    bool equipable{false};
    // Kept on by us: the tick puts it back whenever the game takes it off.
    bool pinned{false};

    // The detail page. Numbers as sections in the style of the character
    // sheet; prose beneath them, each drawn under its own heading when it is
    // not empty. `effects` is one line per effect -- an enchantment's on a
    // weapon or armour, the item's own on a potion, scroll or ingredient --
    // with the magnitude and duration filled in as the item card does.
    std::vector<SheetSection> detail;
    std::string description;
    std::string effects;
};

// Everything she carries that the game would list, sorted by name. Nameless
// entries and armour or weapons flagged non-playable are left out, as the
// container menu leaves them out.
[[nodiscard]] std::vector<InventoryItem> ScanInventory(RE::Actor *actor);

} // namespace ft::game
