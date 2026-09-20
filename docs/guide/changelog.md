---
layout: "default"
title: "Changelog"
permalink: "/changelog/"
nav_order: 11
has_toc: false
---

# Changelog

## 0.2.0

Major changes since `v0.1.0`:

- **Player tactics:** automate consumables, spells, powers, shouts, and equipment with the same rule editor used for followers, while keeping control of aiming and movement.
- **Idle tactics:** separate out-of-combat rules for the player and followers, with independent enable switches. Maintain buffs, heal after fights, cure diseases, or change equipment.
- **Navigation:** keyboard control of tabs, category chips, and filters; long spell and scroll menus grouped by magic school.
- **Equipment and consumables:** idle equipment pins survive combat, automatic player casts restore dual-wielded weapons, and `Weakest` consumable rules preserve stronger items while the weaker effect is active.
- **Performance:** roughly **20Ã— faster tactics evaluation** by computing spell costs only for spells referenced by rules.
- **Versioned documentation:** browse guides for individual releases, with an in-site changelog and collapsible table of contents.
