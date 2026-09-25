---
layout: "default"
title: "Follower training"
permalink: "/training/"
nav_order: 10
has_toc: false
---

# Follower training

With **Manage follower progression** enabled in `Settings` → `Progression`, unique followers train as they use skills. The setting is on by default. Training is available on each follower's Character, Skills, Inventory, and Magic tabs; it does not change the player's progression.

![Progression setting]({{ "/assets/img/panel/settings.png" | relative_url }}){: .screenshot loading="lazy"}

## Levels and attributes

Combat and spell use earn skill experience under the same game settings and experience curve as the player. Skill-ups earn character experience, which raises the follower's training level and grants attribute and perk points. Training adds to the level Skyrim already gives a follower; earned levels can take them up to five levels above the player, but never lower Skyrim's own level. Sneak and crafting skills do not yet gain experience from use.

The follower's [Character tab]({{ '/character/' | relative_url }}) shows their level and progress toward the next level. Click a Health, Stamina, or Magicka row to show its point controls. `-` and `+` move one point; `<<` and `>>` move as far as available. You can also take back points from the follower's original attributes, down to their race's starting values, and spend them elsewhere.

![Assign attributes]({{ "/assets/img/panel/attributes_assign.png" | relative_url }}){: .screenshot loading="lazy"}

## Skills and perks

Open a skill on the follower's [Skills tab]({{ '/skills/' | relative_url }}) to see its perk tree. The controls beside the skill level move one level or as far as available. Taking a skill level back returns its experience value to a pool that you can spend on another skill. A skill cannot go below its starting value, and a perk that still needs its current skill level must be returned before you can lower it.

Click a perk circle to learn its next rank; right-click to return the highest held rank. The page shows available perk points. **Reset perks** returns all held perks in that skill's tree for reassignment. Perks the follower started with can also be returned and restored; their original record stays intact.

[Custom Skills Framework]({{ '/skills/' | relative_url }}) trees spend the same perk points and work the same way, but have no skill level to move.

![Skill tree and perk controls]({{ "/assets/img/panel/skill_tree.png" | relative_url }}){: .screenshot loading="lazy"}

## Teach and forget spells

Give the follower a spell tome, open it on their [Inventory tab]({{ '/inventory/' | relative_url }}), and select **Learn**. Confirming teaches the spell and consumes one tome, as long as the follower does not already know it.

![Learn from a spell tome]({{ "/assets/img/panel/spell_tome_learn.png" | relative_url }}){: .screenshot loading="lazy"}

To remove a spell, open it on the follower's [Magic tab]({{ '/magic/' | relative_url }}) and select **Forget**, then confirm. A spell taught through training is removed from their learned spells. A spell they originally knew is set aside and can be restored by teaching it from a tome again.

![Forget a spell]({{ "/assets/img/panel/spell_forget.png" | relative_url }}){: .screenshot loading="lazy"}

## Turn training off or remove the mod

Turn off `Settings` → `Progression` → **Manage follower progression** to return followers to the skills, attributes, perks, and spells they would have without training. A follower who is away returns to that state when next nearby. Their training record remains in the save; turning the setting on restores it. The switch itself is [saved with your game]({{ '/getting-started/' | relative_url }}#saving-your-changes).

Removing the mod also leaves followers with their normal character values and abilities. Training changes are held by the mod rather than permanently written to follower records, so a save with the mod enabled can return to its normal follower state without a separate reset step. Reinstalling the mod and loading a save that still has its training record brings those changes back.
