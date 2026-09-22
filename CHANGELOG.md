# Changelog

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
