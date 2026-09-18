# Follower Tactics

Follower Tactics is a Skyrim mod that allows combat tactics to be customized for followers. It is inspired by similar systems in [Dragon Age: Origins](https://dragonage.fandom.com/wiki/Tactics_(Origins)) and [Pillars of Eternity II](https://www.gamepressure.com/pillars-of-eternity-2/partys-ai/z1ae65).

<main.png>

## Features

- Highly customizable combat tactics
- Inspection of follower inventory, magic, skills, and more
- Follower equipment management

## Non-features

This mod is focused on helping you understand followers and set appropriate tactics. It is not a:

- Follower management framework
  - See options like [Simple Followers Framework](https://www.nexusmods.com/skyrimspecialedition/mods/174017)
- Follower customization system
  - Changing leveling schemes, teaching followers new spells, etc.
- Combat AI overhaul
  - Underlying AI behavior is preserved, only occasionally overridden by tactics
- Gameplay rebalancer
  - Some followers are over or underpowered. This mod does not address this.

# Getting started

## Install

The following dependencies are required:

- [SKSE64](https://www.nexusmods.com/skyrimspecialedition/mods/30379)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)

Install normally via your preferred mod manager (e.g. [MO2](https://www.nexusmods.com/skyrimspecialedition/mods/6194)).

## Uninstall
site
Leave combat, save, and uninstall normally.

## Interface

To bring up the interface, open the SKSE Menu (default key <kbd>F1</kbd>) and select `Follower Tactics`.

<interface.png>

# Tactics

Tactics are condition-action rules, evaluated every 0.5s (a "tick") for all followers for which they are enabled.

<tactics.png>

The list is evaluated top-down until a **condition** is met. The corresponding **actions** for the matching condition are then executed.

{note}
Tactics are **only applied in combat** (with the exception of `Combat end`; see below).

### Conditions

To set a condition, first select a **target** by clicking on the condition cell:

<condition_target.png>

- `Ally` is the player or another follower.
- `Enemy` is any NPC hostile to the player.
- `Corpse` is a dead body in the area

Then select a condition:

<condition_combat.png>

#### Conditions explained

Most conditions are self-explanatory. A few are worth highlighting.

`Combat start` is checked the first tick after combat begins. `Combat start` rules are checked first, before other rules. If there are multiple `Combat start` rules, their actions are concatenated in rule order.

`Combat end` is checked the first tick after combat ends. Unlike other rules, these are evaluated outside of combat. If there are multiple `Combat end` rules, their actions are concatenated in rule order.

`Hit type` is the kind of attack the target uses (e.g. Melee or Magic, Fire or Frost).

`Hit by` is the kind of attack the target is being hit by. It has the same options as `Hit type`.

`Armor` is shown as percent of damage reduction, instead of absolute values.

### Actions

Actions, like conditions, first require a target:

<action_target.png>

Then an action:

<action_enemy_attack.png>

A few things to keep in mind:

**Actions are contextual**. A follower without potions in their inventory will not see a `Potion` action. A spell that targets `Self` will not show up under `Cast` for `Enemy`.

**Actions respect gameplay rules**. A follower without the [Destruction Dual Casting](https://skyrim.fandom.com/wiki/Destruction_Dual_Casting) perk cannot dual-cast Destruction spells.

**Equip actions override equipment settings during combat**. If a follower has an Iron Dagger pinned to their right hand, but an action says to equip a Steel Sword, the action takes precedence for the duration of combat. Pins and bans are restored after combat.

**Equip actions effectively pin during combat**. An action that equips e.g. a Steel Sword in the right hand prevents the AI from equipping something else there. Only another rule can change the equipment.

#### Multiple Actions

More than one action is allowed for a rule. Actions are taken in order, one per tick. A rule with 4 actions will occupy the next 4 ticks, unless an action is skipped because it cannot be undertaken, in which case the next action is tried in the same tick.

## Enabling / Disabling

### All followers

Tactics can be enabled or disabled for all followers in the `Settings` menu.

<settings.png>

### Per follower

Tactics can be toggled on or off for each follower.

<tactics_char_disabled.png>

### Per tactic

A rule can be also be individually toggled on / off. Disabled tactics are not considered during evaluation.

<tactic_disabled.png>

### Unavailable

A rule whose condition or action is invalid is unavailable. For example, a condition set for a follower who has been dismissed, or an action to drink a potion that's no longer in the inventory.

<tactic_unavailable.png>

If there are multiple actions for a rule, the rule is unavailable if any action is invalid.

# Character

The `Character` tab contains information about the follower, such as their health / stamina / magicka, level, and currently equipped weapons.

<character.png>

Hovering over certain labels reveals details.

<character_magic_hover.png>

# Inventory

The `Inventory` tab contains information about what the follower is carrying, such as weapons and potions.

<inventory_all.png>

## Weapons

The `Weapons` sub-tab lists weapons.

<inventory_weapons.png>

Clicking on a weapon name shows its details.

<weapon_details.png>

`Left` and `Right` columns show equip state in the left and right hands. Click a cell to cycle between states.

### Dual wielding

Whether a follower can dual-wield is determined by their combat style. If a follower cannot dual-wield, putting a weapon into their left hand will remove the one in their right hand, and vice versa.

{:note:}
Dual-wielding only applies to weapons. Having a weapon in one hand and a spell in the other does not qualify as dual-wielding.

## Arrows

The `Arrows` sub-tab lists arrows. Arrows are separated from weapons for convenience, as they are not equipped in hands.

<inventory_arrows.png>

Clicking on an arrow name shows its details.

<arrow_details.png>

## Armor

The `Armor` sub-tab lists armors, clothing, jewelry, etc.:

<inventory_armor.png>

Clicking on an armor name shows its details.

<armor_details.png>

## Potions

The `Potions` sub-tab lists potions:

<inventory_potions.png>

Clicking on a potion name shows its details.

<potion_details.png>

## Poisons

The `Poisons` sub-tab lists poisons:

<inventory_poisons.png>

Clicking on a poison name shows its details.

<poison_details.png>

## Food

The `Food` sub-tab lists foods:

<inventory_food.png>

Clicking on a food name shows its details.

<food_details.png>

## Ingredients

The `Ingredients` sub-tab lists ingredients:

<inventory_ingredients.png>

Clicking on an ingredient name shows its details.

<ingredient_details.png>

## Scrolls

The `Scrolls` sub-tab lists scrolls:

<inventory_scrolls.png>

Clicking on a scroll name shows its details.

<scroll_details.png>

# Magic

The `Magic` tab contains information about a follower's spells, shouts, and powers.

<magic_all.png>

Clicking on a spell name shows its details.

<spell_details.png>

# Summons

The `Summons` tab shows summoned creatures or raised corpses.

<summons.png>

# Effects

The `Effects` tab shows active effects.

<effects.png>

Clicking on an effect name shows its details.

<effect_details.png>

# Skills

The `Skills` tab shows skills and perks.

<skills.png>

Perks for which conditions are not met are shown as inactive.

<perk_inactive.png>

Clicking on a perk shows its details.

<perk_details.png>

# Combat Style

The `Combat Style` tab shows the follower's combat style.

<combat_style.png>

Combat style determines how the AI picks attacks. In the example, `Magic` is set to 10 / 10, while Ranged is set to 0.55 / 10. The AI will therefore heavily prefer magic over ranged attacks.

# Equip states

Equippable items and abilities can be in one of several states:

- Unequipped
- Equipped
- Pinned (always equip)
- Banned (never equip)
- Unavailable (cannot equip)

## Pinned

**Pinning** an object forces the follower to equip it, and prevents them from unequipping it.

<weapon_pinning.png>

{:note:}
Pinned objects can be overridden by tactics, but are restored after combat ends.

Pinning is motivated by the fact that something equipped normally can be overridden by the game. For example, if a follower is equipped with an Iron Sword, but is given a Steel Sword, they will automatically equip the Steel Sword, as it's evaluated to be better.

How the game decides a piece of equipment is "better" is a source of much frustration. Often, it's simply which item has better base stats. A pair of boots with better enchantment but worse armor, for example, won't be worn. It gets worse with **outfits**, the default equipment followers come with, which don't show up in normal inventory and cannot be (normally) removed. A follower won't replace an outfit piece with a superior version of the same equipment.

### Shadowed

A pinned object **shadows** other objects that occupy the same slot.

<armor_shadowing.png>

A shadowed object is grayed out. On hover over its name, a shadowed object will show which object is pinned. Shadowed objects can still be equipped directly, which unpins the pinned object. There is no need to unpin first.

The AI won't use shadowed objects, but tactics can still equip them.

## Banned

**Banning** an object prevents it from being equipped.

<weapon_banning.png>

{:note:}
Banned objects can be equipped by tactics, but are unequipped after combat ends.

The AI won't use banned objects. This is useful for preventing a follower from using a particular weapon, casting a certain spell, etc.

## Unavailable

An object the follower cannot equip is **unavailable**.

{:note:}
Unavailable objects cannot be equipped by tactics.

Many NPC spells are restricted to a particular hand. Below, `Raise Zombie` can only be equipped in the right hand, while `Revenant` can only be equipped in the left.

<spell_unavailable_hand.png>

Some spells require a certain skill level to use. Below, `Blood Javelin` require Destruction of 75, while the follower only has 51. Therefore, both hands are unavailable.

<spell_unavailable_level.png>
