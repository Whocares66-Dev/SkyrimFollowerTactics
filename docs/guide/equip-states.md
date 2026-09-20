---
layout: "default"
title: "Equip states"
permalink: "/equip-states/"
nav_order: 10
has_toc: false
---

# Equip states

Equippable items and abilities can be in one of several states:

- Unequipped
- Equipped
- [Pinned](#pinned) (always equip)
- [Banned](#banned) (never equip)
- [Unavailable](#unavailable) (cannot equip)

Pins and bans apply to followers. The player's equipment uses ordinary equip / unequip controls.

## Pinned

**Pinning** an object forces the follower to equip it, and prevents them from unequipping it.

![Weapon pinning]({{ "/assets/img/panel/weapon_pinning.png" | relative_url }}){: .screenshot loading="lazy"}

Pinned objects can be overridden by [tactics]({{ '/tactics/' | relative_url }}#actions), but are restored after combat ends.
{: .note }

Pinning is motivated by the fact that something equipped normally can be overridden by the game. For example, if a follower is equipped with an Iron Sword, but is given a Steel Sword, they will automatically equip the Steel Sword, as it's evaluated to be better.

How the game decides a piece of equipment is "better" is a source of much frustration. Often, it's simply which item has better base stats. A pair of boots with better enchantment but worse armor, for example, won't be worn. It gets worse with **outfits**, the default equipment followers come with, which don't show up in normal inventory and cannot be (normally) removed. In the base game, a follower won't replace an outfit piece with a superior version of the same equipment.

### Shadowed

A pinned object **shadows** other objects that occupy the same slot.

![Armor shadowing]({{ "/assets/img/panel/armor_shadowing.png" | relative_url }}){: .screenshot loading="lazy"}

A shadowed object is grayed out. On hover over its name, a shadowed object will show which object is pinned. Shadowed objects can still be equipped directly, which unpins the pinned object. There is no need to unpin first.

The AI won't use shadowed objects, but [tactics]({{ '/tactics/' | relative_url }}#actions) can still equip them.

## Banned

**Banning** an object prevents it from being equipped.

![Weapon banning]({{ "/assets/img/panel/weapon_banning.png" | relative_url }}){: .screenshot loading="lazy"}

Banned objects can be equipped by [tactics]({{ '/tactics/' | relative_url }}#actions), but are unequipped after combat ends.
{: .note }

The AI won't use banned objects. This is useful for preventing a follower from using a particular [weapon]({{ '/inventory/' | relative_url }}#weapons), casting a certain [spell]({{ '/magic/' | relative_url }}), etc.

## Unavailable

An object the follower cannot equip is **unavailable**.

Unavailable objects cannot be equipped by [tactics]({{ '/tactics/' | relative_url }}#actions).
{: .note }

Many NPC spells are restricted to a particular hand. Below, `Raise Zombie` can only be equipped in the right hand, while `Revenant` can only be equipped in the left.

![Spell unavailable hand]({{ "/assets/img/panel/spell_unavailable_hand.png" | relative_url }}){: .screenshot loading="lazy"}

Some spells require a certain [skill level]({{ '/skills/' | relative_url }}) to use. Below, `Blood Javelin` requires Destruction of 75, while the follower only has 51. Therefore, both hands are unavailable.

![Spell unavailable level]({{ "/assets/img/panel/spell_unavailable_level.png" | relative_url }}){: .screenshot loading="lazy"}
