---
layout: "default"
title: "Changelog"
permalink: "/changelog/"
nav_order: 13
has_toc: false
---

# Changelog

## 0.4.0

Major changes since `v0.3.0`:

- **More varied follower combat AI:** attack spells are chosen with weighted randomness, with recent casts less likely to repeat. Scores account for perks, enemy resistances and immunities, cast time, and remaining magicka; weapons also benefit from perk-aware scoring. `Varied AI choices` is on by default under `Settings` → `Combat AI`.
- **Self-targeting damage spells:** followers can choose area attacks such as Fire Storm when enemies are in reach, with crowds making them more attractive. `Use self-targeting damage spells` is a separate option, also on by default.
- **More tactic conditions:** check named effects, location (including homes, interiors, holds, and dungeon types), bound weapons, carried arrows or bolts, and bleeding. Burning, frostbitten, and shocked now detect damage affecting the character, rather than a cloak they wear.
- **Custom skill trees:** browse Custom Skills Framework trees on the Skills tab and spend follower training perk points in them. Perk changes update follower armour rating and carry weight immediately.
- **More reliable actions:** follower power attacks match their weapons, bashes start when the block is ready, and tactic casts take priority over the follower's own spells. Player casts wait for equip animations and allow any known spell regardless of skill. `Unequip` removes worn equipment whether pinned or not.
- **Fewer wasted casts and items:** named consumables and self-cast abilities wait while all their lasting effects are already active at equal or greater strength. For every NPC, mutually dispelling cloaks stop replacing each other, and staves remain usable while they have enough charge for a cast.
- **Weapon charging:** a `Charge` button on an enchanted weapon's page uses the weakest filled soul gem carried. `Charge needed` triggers with three or fewer uses left.
- **Easier panel controls:** drag rules by their number to reorder them; ban or unban all filtered equipment or spells at once; hover a skill level for its breakdown or a shortened condition for its full text. Lists and menus use consistent name ordering. Spell details identify the effect whose skill requirement blocks use, and the Effects tab switches between `Active` effects and `All`, including inactive effects and those that make no change.

## 0.3.0

Major changes since `v0.2.0`:

- **Follower training:** unique followers gain skill experience through use. Skill-ups add character experience on the same curve used by the player, and training levels grant attribute and perk points.
- **Reassigning points:** move Health, Magicka, and Stamina points; move skill levels using their experience value; learn, return, or reset perks from a skill's tree, including perks a follower started with.
- **Spell learning:** teach a follower a spell from a tome in their inventory, or forget a spell from its Magic page. A forgotten original spell can be restored later.
- **Reversible changes:** training keeps its own record without permanently changing the follower's original skills, attributes, perks, or spells. Turn off `Settings` → `Progression` → `Manage follower progression` to return to their normal state; turn it back on to restore training. Removing the mod also returns followers to their normal state.
- **Localized panel:** interface text now follows the game's language, with English and Simplified Chinese catalogs and English fallback for untranslated text.

## 0.2.0

Major changes since `v0.1.0`:

- **Player tactics:** automate consumables, spells, powers, shouts, and equipment with the same rule editor used for followers, while keeping control of aiming and movement.
- **Idle tactics:** separate out-of-combat rules for the player and followers, with independent enable switches. Maintain buffs, heal after fights, cure diseases, or change equipment.
- **Navigation:** keyboard control of tabs, category chips, and filters; long spell and scroll menus grouped by magic school.
- **Equipment and consumables:** idle equipment pins survive combat, automatic player casts restore dual-wielded weapons, and `Weakest` consumable rules preserve stronger items while the weaker effect is active.
- **Performance:** roughly **20× faster tactics evaluation** by computing spell costs only for spells referenced by rules.
- **Versioned documentation:** browse guides for individual releases, with an in-site changelog and collapsible table of contents.
