// What a list of the panel shows, and what each equip cell of a row says.
// No panel, no ImGui: the rows are built by hand.

#include <catch2/catch_test_macros.hpp>

#include "core/Rows.h"

#include <string>
#include <vector>

using namespace ft;

namespace
{

constexpr int kAll = -1;
int Of(ItemCategory category)
{
    return static_cast<int>(category);
}
int Of(MagicCategory category)
{
    return static_cast<int>(category);
}

InventoryItem Sword()
{
    InventoryItem item;
    item.form = 0x13989;
    item.name = "Steel Sword";
    item.type = "One-handed";
    item.category = ItemCategory::Weapons;
    item.weight = 9.0f;
    item.value = 45;
    item.damage = 8.0f;
    item.handItem = true;
    item.equipable = true;
    return item;
}

InventoryItem Potion()
{
    InventoryItem item;
    item.form = 0x3EADE;
    item.name = "Potion of Minor Healing";
    item.type = "Potion";
    item.effect = "Restore Health";
    item.category = ItemCategory::Potions;
    item.weight = 0.5f;
    item.value = 36;
    return item;
}

MagicEntry Firebolt()
{
    MagicEntry entry;
    entry.form = 0x12FCD;
    entry.name = "Firebolt";
    entry.type = "Spell";
    entry.cast = "Aimed";
    entry.school = "Destruction";
    entry.level = "Apprentice";
    entry.cost = "27";
    entry.category = MagicCategory::Destruction;
    entry.leftAllowed = true;
    entry.rightAllowed = true;
    return entry;
}

MagicEntry Shout()
{
    MagicEntry entry;
    entry.form = 0x13E07;
    entry.name = "Unrelenting Force";
    entry.type = "Shout";
    entry.category = MagicCategory::Shouts;
    return entry;
}

} // namespace

TEST_CASE("a list shows the columns its category calls for", "[rows]")
{
    REQUIRE(ColumnsOf(Of(ItemCategory::Weapons)).weapons);
    REQUIRE(ColumnsOf(Of(ItemCategory::Arrows)).weapons);
    REQUIRE(ColumnsOf(Of(ItemCategory::Armor)).armour);
    REQUIRE(ColumnsOf(Of(ItemCategory::Scrolls)).scrolls);
    REQUIRE(ColumnsOf(Of(ItemCategory::Scrolls)).consumables);
    REQUIRE(ColumnsOf(Of(ItemCategory::Food)).consumables);
    REQUIRE_FALSE(ColumnsOf(Of(ItemCategory::Books)).consumables);
    // The All list shows none of the per-category columns.
    const ItemColumns all = ColumnsOf(kAll);
    REQUIRE_FALSE(all.weapons);
    REQUIRE_FALSE(all.armour);
    REQUIRE_FALSE(all.consumables);
}

TEST_CASE("the filter searches the cells the list shows, numbers as they print", "[rows]")
{
    const InventoryItem sword = Sword();
    REQUIRE(ItemShown(sword, Of(ItemCategory::Weapons), "steel"));
    REQUIRE(ItemShown(sword, Of(ItemCategory::Weapons), "ONE-HANDED"));
    // The weight prints with one decimal, the value plain, the damage with
    // none: the filter matches what is on screen.
    REQUIRE(ItemShown(sword, Of(ItemCategory::Weapons), "9.0"));
    REQUIRE(ItemShown(sword, Of(ItemCategory::Weapons), "45"));
    REQUIRE(ItemShown(sword, Of(ItemCategory::Weapons), "8"));
    REQUIRE_FALSE(ItemShown(sword, Of(ItemCategory::Weapons), "dagger"));
    // An empty filter leaves every row of the category on the list.
    REQUIRE(ItemShown(sword, Of(ItemCategory::Weapons), ""));
    REQUIRE(ItemShown(sword, kAll, ""));
    // Another category's list does not hold it.
    REQUIRE_FALSE(ItemShown(sword, Of(ItemCategory::Armor), ""));

    // A consumable's second column says what it does, not what it is, so
    // the type is not searched there.
    const InventoryItem potion = Potion();
    REQUIRE(ItemShown(potion, Of(ItemCategory::Potions), "restore"));
    REQUIRE_FALSE(ItemShown(potion, Of(ItemCategory::Potions), "potion of nothing"));
    REQUIRE(ItemShown(potion, kAll, "Potion"));
    // The damage column is not on the potions' list, so a weapon's number
    // cannot be matched there.
    InventoryItem odd = Potion();
    odd.damage = 8.0f;
    REQUIRE_FALSE(ItemShown(odd, Of(ItemCategory::Potions), "8"));
}

TEST_CASE("a magic list is the voice's or the schools', and shows the school only on All", "[rows]")
{
    const MagicEntry firebolt = Firebolt();
    const MagicEntry shout = Shout();
    REQUIRE(VoiceEntry(shout));
    REQUIRE_FALSE(VoiceEntry(firebolt));

    // A spell is on no voice list, and a shout on no school list.
    REQUIRE(MagicShown(firebolt, kAll, false, ""));
    REQUIRE_FALSE(MagicShown(firebolt, kAll, true, ""));
    REQUIRE(MagicShown(shout, kAll, true, ""));
    REQUIRE_FALSE(MagicShown(shout, kAll, false, ""));

    // The school is searched on the All list, where it is shown.
    REQUIRE(MagicShown(firebolt, kAll, false, "destruction"));
    REQUIRE_FALSE(MagicShown(firebolt, Of(MagicCategory::Destruction), false, "destruction"));
    // The level and the cost are on both.
    REQUIRE(MagicShown(firebolt, Of(MagicCategory::Destruction), false, "apprentice"));
    REQUIRE(MagicShown(firebolt, Of(MagicCategory::Destruction), false, "27"));
    // A voice list shows neither, so neither is searched.
    MagicEntry power = Shout();
    power.category = MagicCategory::Powers;
    power.level = "Greater";
    power.cost = "0";
    REQUIRE_FALSE(MagicShown(power, kAll, true, "greater"));
    REQUIRE(MagicShown(power, kAll, true, "unrelenting"));
}

TEST_CASE("an effect row is searched by its name, its time left and its source", "[rows]")
{
    EffectRow row;
    row.name = "Fortify Health";
    row.remainingText = "42 s";
    row.source = "Amulet of Health";
    row.magnitude = 25.0f;
    REQUIRE(EffectShown(row, false, "fortify"));
    REQUIRE(EffectShown(row, false, "42 s"));
    REQUIRE(EffectShown(row, false, "amulet"));
    REQUIRE(EffectShown(row, false, "25"));
    REQUIRE(EffectShown(row, false, ""));
    REQUIRE_FALSE(EffectShown(row, false, "magicka"));
    // No magnitude: no number to match.
    row.magnitude = 0.0f;
    REQUIRE_FALSE(EffectShown(row, false, "25"));
}

TEST_CASE("an effect not acting is listed only when all effects are asked for", "[rows]")
{
    EffectRow acting;
    acting.name = "Fortify Health";
    EffectRow inactive = acting;
    inactive.active = false;
    EffectRow unapplied = acting;
    unapplied.applied = false;

    REQUIRE(EffectListed(acting, false));
    REQUIRE(EffectListed(acting, true));
    REQUIRE_FALSE(EffectListed(inactive, false));
    REQUIRE(EffectListed(inactive, true));
    REQUIRE_FALSE(EffectListed(unapplied, false));
    REQUIRE(EffectListed(unapplied, true));

    // One the game's own list hides is listed as any other.
    EffectRow hidden = acting;
    hidden.hidden = true;
    REQUIRE(EffectListed(hidden, false));

    // Asked for, it is searched as any other row; not asked for, no text
    // brings it back.
    REQUIRE(EffectShown(inactive, true, "fortify"));
    REQUIRE_FALSE(EffectShown(inactive, true, "magicka"));
    REQUIRE_FALSE(EffectShown(inactive, false, "fortify"));
    REQUIRE_FALSE(EffectShown(inactive, false, ""));
}

TEST_CASE("an item's cells: which hand can take it, what is in it, and what the book says", "[rows]")
{
    InventoryItem sword = Sword();
    // A hand item takes either hand and not the worn column.
    REQUIRE(LeftCell(sword).allowed);
    REQUIRE(RightCell(sword).allowed);
    REQUIRE_FALSE(WornCell(sword).allowed);
    // A shield takes the left hand alone.
    InventoryItem shield = Sword();
    shield.name = "Iron Shield";
    shield.leftOnly = true;
    REQUIRE(LeftCell(shield).allowed);
    REQUIRE_FALSE(RightCell(shield).allowed);
    // A cuirass takes no hand, and the worn column instead.
    InventoryItem cuirass = Sword();
    cuirass.handItem = false;
    REQUIRE_FALSE(LeftCell(cuirass).allowed);
    REQUIRE(WornCell(cuirass).allowed);

    // The state each cell carries.
    sword.equippedRight = true;
    sword.pinnedRight = true;
    REQUIRE(RightCell(sword).on);
    REQUIRE(RightCell(sword).pinned);
    REQUIRE_FALSE(LeftCell(sword).on);
    sword.banned = true;
    REQUIRE(LeftCell(sword).banned);
    REQUIRE(RightCell(sword).banned);
    // Set aside by a pin: every cell of the row is dim.
    sword.setAside = true;
    REQUIRE(Dimmed(sword));
    REQUIRE(LeftCell(sword).disabled);
    REQUIRE(WornCell(sword).disabled);
}

TEST_CASE("a spell above their skill takes no hand; a shout with no word unlocked takes no voice", "[rows]")
{
    MagicEntry firebolt = Firebolt();
    REQUIRE(LeftCell(firebolt).allowed);
    REQUIRE(RightCell(firebolt).allowed);
    firebolt.aboveSkill = true;
    REQUIRE_FALSE(LeftCell(firebolt).allowed);
    REQUIRE_FALSE(RightCell(firebolt).allowed);
    REQUIRE(Dimmed(firebolt));

    // A one-hand spell variant takes that hand alone.
    MagicEntry leftOnly = Firebolt();
    leftOnly.rightAllowed = false;
    REQUIRE(LeftCell(leftOnly).allowed);
    REQUIRE_FALSE(RightCell(leftOnly).allowed);

    // A shout is offered in the voice, and its hand cells take it too --
    // they are the voice's, whatever the hands allow.
    MagicEntry shout = Shout();
    REQUIRE(VoiceCell(shout).allowed);
    REQUIRE(LeftCell(shout).allowed);
    // With no word unlocked nothing the panel does can make them shout it.
    shout.locked = true;
    REQUIRE_FALSE(VoiceCell(shout).allowed);
    REQUIRE(Dimmed(shout));
    // Readied in the voice, and pinned there.
    MagicEntry readied = Shout();
    readied.equipped = true;
    readied.pinned = true;
    REQUIRE(VoiceCell(readied).on);
    REQUIRE(VoiceCell(readied).pinned);
}

TEST_CASE("the inventory columns order by what each shows", "[rows]")
{
    InventoryItem light = Sword();
    light.name = "Dagger";
    light.weight = 2.0f;
    light.value = 10;
    light.damage = 4.0f;
    InventoryItem heavy = Sword();
    heavy.name = "Warhammer";
    REQUIRE(CompareItems(light, heavy, Column::Weight) == -1);
    REQUIRE(CompareItems(light, heavy, Column::Value) == -1);
    REQUIRE(CompareItems(light, heavy, Column::Damage) == -1);
    REQUIRE(CompareItems(heavy, light, Column::Damage) == 1);
    REQUIRE(CompareItems(light, light, Column::Weight) == 0);
    // The name is the default, and what an unshown column falls back to.
    REQUIRE(CompareItems(light, heavy, Column::Name) < 0);
    REQUIRE(CompareItems(light, heavy, Column::Remaining) < 0);
    // The Type column shows a consumable's effect instead of its type.
    InventoryItem potion = Potion();
    InventoryItem poison = Potion();
    poison.effect = "Damage Health";
    poison.type = "Zzz";
    REQUIRE(CompareItems(potion, poison, Column::Type) > 0);
    InventoryItem plainA = Sword();
    plainA.type = "Axe";
    InventoryItem plainB = Sword();
    plainB.type = "Bow";
    REQUIRE(CompareItems(plainA, plainB, Column::Type) < 0);
    // An equip column orders by the cell's rank: pinned first, banned late.
    InventoryItem pinned = Sword();
    pinned.pinnedRight = true;
    InventoryItem banned = Sword();
    banned.banned = true;
    REQUIRE(CompareItems(pinned, banned, Column::Right) == -1);
    REQUIRE(CompareItems(pinned, Sword(), Column::Left) == 0);
}

TEST_CASE("the magic columns order by their values, the cast by its delivery first", "[rows]")
{
    MagicEntry novice = Firebolt();
    novice.level = "Novice";
    novice.levelValue = 0;
    novice.costValue = 10.0f;
    MagicEntry expert = Firebolt();
    expert.levelValue = 75;
    expert.costValue = 200.0f;
    REQUIRE(CompareMagic(novice, expert, Column::Level) == -1);
    REQUIRE(CompareMagic(novice, expert, Column::Cost) == -1);
    REQUIRE(CompareMagic(novice, novice, Column::Level) == 0);

    // Two of one delivery are told apart by the word; two deliveries by
    // the delivery, whatever the words say.
    MagicEntry selfA = Firebolt();
    selfA.castValue = 0;
    selfA.cast = "Self";
    MagicEntry selfB = Firebolt();
    selfB.castValue = 0;
    selfB.cast = "Concentration";
    MagicEntry aimed = Firebolt();
    aimed.castValue = 2;
    aimed.cast = "Aimed";
    REQUIRE(CompareMagic(selfA, selfB, Column::Cast) > 0);
    REQUIRE(CompareMagic(aimed, selfA, Column::Cast) == 1);

    // The voice column orders by the voice cell, the hands by theirs.
    MagicEntry readied = Shout();
    readied.equipped = true;
    REQUIRE(CompareMagic(readied, Shout(), Column::Equipped) == -1);
    MagicEntry pinnedLeft = Firebolt();
    pinnedLeft.pinnedLeft = true;
    REQUIRE(CompareMagic(pinnedLeft, Firebolt(), Column::Left) == -1);
    REQUIRE(CompareMagic(pinnedLeft, Firebolt(), Column::Right) == 0);
}

TEST_CASE("an effect with no duration sorts after every one that runs out", "[rows]")
{
    EffectRow short_;
    short_.name = "Fortify Health";
    short_.remaining = 5.0f;
    short_.magnitude = 10.0f;
    short_.source = "Amulet";
    EffectRow long_ = short_;
    long_.name = "Waterbreathing";
    long_.remaining = 300.0f;
    EffectRow endless = short_;
    endless.name = "Blessing";
    endless.remaining = -1.0f;
    endless.source = "Shrine";
    REQUIRE(CompareEffects(short_, long_, Column::Remaining) == -1);
    REQUIRE(CompareEffects(long_, endless, Column::Remaining) == -1);
    REQUIRE(CompareEffects(endless, short_, Column::Remaining) == 1);
    REQUIRE(CompareEffects(endless, endless, Column::Remaining) == 0);
    REQUIRE(CompareEffects(short_, endless, Column::Source) < 0);
    long_.magnitude = 50.0f;
    REQUIRE(CompareEffects(short_, long_, Column::Magnitude) == -1);
    REQUIRE(CompareEffects(short_, long_, Column::Name) < 0);
}
