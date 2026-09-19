#pragma once
// What the panel is given to draw: a sheet's rows and sections, a row of
// the Inventory tab, an entry of the Magic tab. Built on the game thread
// from the live actor and copied to the render thread, so the panel lists
// what a follower carries and knows without touching an RE:: type. Plain
// values only, so the decisions over them -- which rows a filter leaves,
// what a cell says, what a click asks -- are testable without the game or
// ImGui; what fills them is the game's (game/Inventory.h, game/Magic.h,
// game/Sensors.h).

#include "Breakdown.h"
#include "Kinds.h"
#include "Loadout.h"
#include "Snapshot.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ft
{

struct SheetRow
{
    std::string label;
    std::string value;
    unsigned icon{0};      // a Font Awesome codepoint drawn instead of the value, when set
    unsigned icon2{0};     // a second glyph after the first: the pin beside the tick
    std::string modifiers; // Skills tab only: "+35% damage, -17% cost"
    std::string note;      // plain hover text on the value; empty for none
    // The value written out as the calculation that made it, hover text
    // on the value. Drawn in place of `note` when it has lines.
    ft::Breakdown breakdown;
    // The Modifiers cell in pieces, each with its own breakdown: "+50%
    // damage" and "-50% cost" are two figures and hover apart. Drawn in
    // place of `modifiers` when not empty.
    struct ModifierPart
    {
        std::string text;
        ft::Breakdown breakdown;
    };
    std::vector<ModifierPart> modifierParts;
    // The columns an effect's row may carry after its value, each drawn
    // only where some row has it: the duration, what is left of it, and
    // the source, a link to `form` where that has a page. Hidden is the
    // `mark`.
    std::string extra;
    std::string remaining;
    std::string link;
    // An effect's description with its numbers filled in, for a table
    // with a wrapped last column of them.
    std::string description;
    // Rows revealed by expanding this one: a skill's perks. Empty means the
    // row is a plain line and cannot be opened.
    std::vector<SheetRow> detail;
    // The inventory item this row names, if any: a click on it opens the
    // item's page on the Inventory tab. 0 for a row that names nothing.
    std::uint32_t form{0};
    // Why the row is set aside -- a perk whose conditions fail for this
    // actor -- shown on the name; empty for a row that counts.
    std::string aside;
    // A glyph in the third column, where the table has one: an effect's
    // tick for Hidden. 0 for none.
    unsigned mark{0};
    // The Equipped row of a page (EquippedRow), where the pin glyph goes.
    bool equipped{false};
};

struct SheetSection
{
    std::string title;
    std::vector<SheetRow> rows;
    // The heading this section sits under when several share one -- Attack
    // over a Right Hand table and a Left Hand table. Empty means the title
    // is the heading.
    std::string group;
    // Why the whole section is set aside -- a shout's word the player has
    // not unlocked -- shown on its label, which is greyed with its rows.
    // The heading over a group is not: it covers the sections that count
    // too. Empty for a section that counts. Initialised here rather than
    // left bare so the sections brace-built from their first three fields
    // stay as they are: clang-tidy makes a field with no initializer of
    // its own a missing-field-initializer error at every one of them.
    std::string aside{};
};

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
    // stack; null for the stack. An opaque token: it is handed back with a
    // click and checked against the bag's lists before it is used, since
    // the copy may have left (dev/TESTING.md, "Identity across time").
    // Never dereferenced outside the game thread, and never here.
    const void *row{nullptr};
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

enum class MagicCategory : std::uint8_t
{
    Alteration,
    Conjuration,
    Destruction,
    Illusion,
    Restoration,
    // A spell of no school: a vampire's Drain Life, a race's ability cast
    // as a spell. The combat AI casts them and a rule may name them, so the
    // tab lists them; the level and the skill gate mean nothing for them.
    Other,
    Shouts,
    Powers,
    COUNT
};

struct MagicEntry
{
    std::uint32_t form{0};
    std::string name;
    MagicCategory category{MagicCategory::Powers};
    // The columns: the school as the game names it, the level word the
    // magic menu shows, the cost as it shows it ("13/s" for a stream).
    std::string school;
    std::string level;
    // What kind of spell: the element where the costliest effect does that
    // kind of damage (Fire, Frost, Shock, Poison), else the effect's kind by
    // its archetype -- Summon, Reanimate, Bound Weapon, Calm, Fear, Frenzy,
    // Heal, Ward, Armor, Invisibility, Paralysis and so on. Every school
    // has one; the school lists show it beside the name.
    std::string type;
    // How it is cast, delivery and casting type folded into one word: Self,
    // Touch, Spray (aimed and sustained -- Flames), Projectile (aimed and
    // fired -- Firebolt), Target, Location.
    std::string cast;
    std::string cost;
    float magnitude{0.0f}; // the costliest effect's, the column
    int levelValue{0};     // the minimum skill behind the word, for sorting
    int skill{0};          // the follower's level in the spell's school
    // The level is above the follower's skill: left to itself the combat AI
    // will not choose it (Chain Lightning, Adept, against Destruction 39).
    // The package could make them cast it regardless, but by our rule it is
    // neither cast nor pinned, and the hand cells take no click for it.
    // Never the player's: no combat AI chooses for them.
    bool aboveSkill{false};
    int castValue{0};      // the delivery behind the word, for sorting
    float costValue{0.0f}; // magicka, for sorting; 0 for powers and shouts
    // The cost written out -- the effects, the skill curve, each perk
    // entry -- as hover text on the cost cell; empty for a power or shout.
    ft::Breakdown costBreakdown;
    // In a hand -- and which -- or, for a power or shout, selected.
    bool equipped{false};
    bool equippedLeft{false};
    bool equippedRight{false};
    bool pinnedLeft{false};
    bool pinnedRight{false};
    bool pinned{false}; // a voice pin: a power or shout held in the voice slot
    // Never to be used: the combat AI scores it zero, the engine's equips of
    // it are refused, and it comes off if found on. Off, and kept off.
    bool banned{false};
    // Which hands the record allows (SpellGrip).
    bool leftAllowed{true};
    bool rightAllowed{true};
    // The same, as the pin book words it, for the equip menu's hand lists.
    ft::Grip grip{ft::Grip::None};
    // A shout the player has unlocked no word of: the follower has it in
    // their list and cannot shout a word of it, so it is listed dimmed and
    // its voice cell takes no click. Never a spell's or a power's.
    bool locked{false};
    // Kept from the combat AI in a fight because it would take a hand a
    // pin holds: it scores zero whenever the combat AI asks. Still known, and a
    // cast rule can still make them cast it. Listed dimmed.
    bool setAside{false};
    // Why, when it is: one line per pin that holds a hand it could take,
    // "Firebolt is pinned", for the row's tooltip.
    std::string asideBy;
    std::string hand; // the record's word for it: Either, Left, Right, Both; Voice for a power or shout

    // The page: numbers as sections, then the effect lines and the record's
    // description, as the Inventory tab's pages are laid out.
    std::vector<SheetSection> detail;
    // What it does: the table of effects (EffectsOf), at their own
    // magnitudes, each row's description in its last column. A shout has a
    // table per word: each word is a spell of its own, and a later word's
    // carries effects the first word's does not.
    std::vector<SheetSection> effectTables;
    std::string description;
};

struct EffectRow
{
    // The base effect and what applied it, together the row's identity:
    // the same effect can run twice from two sources.
    std::uint32_t form{0};
    std::uint32_t sourceForm{0};
    // What the source's name links to, where it has a page: the worn item
    // behind an enchantment, else the spell. The panel decides whether a
    // page exists, by its own lists.
    std::uint32_t linkForm{0};
    std::string name;
    float magnitude{0.0f};
    float duration{0.0f};      // in all; 0 for one with no duration
    float remaining{-1.0f};    // seconds left; below zero for one with no duration
    std::string remainingText; // empty for one with no duration
    std::string source;
    // False when the effect is running but changes nothing for this actor:
    // Fortify One-handed on a follower, which writes a value nothing on a
    // follower reads. Listed greyed, hovering as "Not applied".
    bool applied{true};
    // False while the effect is on the list but not acting (Spellbreaker's
    // ward off the block), by the engine's own flag and not by asking the
    // conditions again: a magic effect record's conditions are asked once,
    // when it lands, and can read false ever after (Adamant's Bastion asks
    // whether the cast was dual). Listed greyed, hovering as "Inactive".
    bool active{true};

    // The page: the effect's numbers as the first section, then what its
    // source does, effect by effect, each opening on its conditions -- the
    // perk page's shape -- and the description with the magnitude and
    // duration filled in, as the item card shows it.
    std::vector<SheetSection> detail;
    std::string description;
};

} // namespace ft
