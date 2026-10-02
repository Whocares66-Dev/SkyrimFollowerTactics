---
layout: "default"
title: "Tactics"
permalink: "/tactics/"
nav_order: 10
has_toc: false
---

# Tactics

Tactics are condition-action rules, evaluated every 0.5s (a "tick") for the player and followers for which they are enabled.

![Tactics]({{ "/assets/img/panel/tactics.png" | relative_url }}){: .screenshot loading="lazy"}

The list is evaluated top-down. When a [**condition**](#conditions) is met, the corresponding [**actions**](#actions) are tried. If none can be executed, evaluation moves to the next rule in the same tick. See [Unavailable](#unavailable).

The **Tactics** tab runs in combat, including `Combat start` and `Combat end` rules. **Idle Tactics** runs out of combat. Each character has both lists; add a rule to both if you want it in both.

## Player tactics

The player's page has the same rule editor. Use it to drink potions, cast spells, use powers and shouts, or change equipment automatically.

The player aims their own spells and blows. `Attack` is not offered; `Power Attack`, `Bash`, and `Power Bash` are. Equip actions last until you or another rule replace the item; they do not pin it.

Automatic casts use your hands and normal casting animations. Tactics pause during dialogue, while mounted, swimming, using furniture, knocked down, in a kill move or beast form, or when fighting controls are disabled.

## Idle Tactics

Use **Idle Tactics** to maintain buffs, heal after a fight, cure diseases, or change equipment out of combat.

![Idle tactics]({{ "/assets/img/panel/idle_tactics.png" | relative_url }}){: .screenshot loading="lazy"}

For example, `IF Self: Any THEN Self: Cast Oakflesh` recasts the spell when its effect expires. Use `Self: Diseased` to trigger a cure, or a health percentage condition to heal.

Combat-only choices are omitted: enemy conditions and targets, `Combat start`, `Combat end`, `Hit by`, `Fleeing`, `Attacker`, attacks, and bashes.

`Combat end` actions finish before idle rules begin. When combat starts, any remaining actions in the idle sequence are dropped.

## Conditions

To set a condition, first select a **target** by clicking on the condition cell:

![Condition target]({{ "/assets/img/panel/condition_target.png" | relative_url }}){: .screenshot loading="lazy"}

- `Self` is the character whose tactics you are editing.
- `Player` is the player.
- `Follower` is a specific follower, chosen by name.
- `Ally` is another party member: the player or a follower. In the player's rules, it means a follower.
- `Enemy` is any NPC hostile to the player.
- `Corpse` is a dead body in the area.

Then select a condition:

![Condition combat]({{ "/assets/img/panel/condition_combat.png" | relative_url }}){: .screenshot loading="lazy"}

### Conditions explained

Most conditions are self-explanatory. A few are worth highlighting.

`Combat start` is checked the first tick after combat begins. `Combat start` rules are checked first, before other rules. If there are multiple `Combat start` rules, their actions are concatenated in rule order.

`Combat end` is checked the first tick after combat ends, before idle rules. If there are multiple `Combat end` rules, their actions are concatenated in rule order.

`Attacks with` is the kind of attack the target uses (e.g. Melee or Magic, Fire or Frost).

`Hit by` is the kind of attack the target is being hit by.

`Armor` is shown as percent of damage reduction, rather than absolute values.

`Effect` is met while the named effect is active on the target, whatever applied it. The list offers lasting effects available from the whole party's potions, food, spells, scrolls, powers, and shouts, including toggle powers. Negate it to act while the effect is absent.

`Rallied`, `Calmed`, `Feared`, and `Frenzied` detect active illusion influences on the target. For example, use a negated `Calmed` condition to avoid trying to calm an already calmed enemy.

`Time` checks the part of the day: `Morning`, `Afternoon`, `Evening`, or `Night`. It is offered in idle tactics. The day is divided by the sun as the game's climate times it: morning from sunrise to noon, afternoon until sunset begins, evening through sunset, and night after it. In the unmodified game that is 5:30, 12:00, 16:00, and 20:30; weather mods may move sunrise and sunset. Hover over a part of the day to see its hours in your game.

`Location` checks where the target is: home, interior or exterior, a hold, or a type of building, dungeon, ruin, or settlement. It is offered in idle tactics; for example, use `Self: Location → Home` to change equipment at home. Place types follow the game's location tags, including parent locations; a dungeon or mine filed under a town is not in the settlement.

`Weather` checks the weather over the target: `Pleasant`, `Cloudy`, `Rain`, `Snow`, or `Ash`. It is offered in idle tactics, and only outdoors: indoors none of them is met, whatever the weather outside. `Pleasant` includes partly cloudy days; `Cloudy` is fog and overcast; `Rain`, `Snow`, and `Ash` are met while precipitation is falling. `Ash` means Solstheim's ash storm (`DLC02VolcanicAshStorm01`), which counts as Ash instead of Snow. Solstheim's other hazy ash weathers remain Cloudy.

`Enemy` → `Rank` → `Boss`, just after `Type`, selects powerful enemies such as dragons, dragon priests, Harkon, Miraak, and enemies the game marks as a location's boss. An action aimed at `Enemy` targets the matching boss. This classification is automatic and has no settings.

`Weapon` → `Bound` → `Active` means a conjured weapon is in either hand; `None` means neither hand holds one.

`Arrows` → `None` / `Available` checks whether the character carries arrows or bolts, whether equipped or not. It is offered for `Self`.

`Bleeding` means an active bleeding damage effect; `Bleeding out` means the character is down. `Burning`, `Frostbitten`, and `Shocked` detect damage affecting the target, not a cloak they wear.

`Weapon` → `Charge needed` is met when an enchanted weapon in hand has three or fewer uses left.

### Negation

A condition can be negated by checking the `NOT` column.

![Condition not]({{ "/assets/img/panel/condition_not.png" | relative_url }}){: .screenshot loading="lazy"}

Negation applies to each target the condition looks at. `Enemy` `NOT` `Type: Undead` matches an enemy that is not undead, and an action aimed at `Enemy` goes at that one. When several match, the nearest is picked. To express "no enemy is undead", order the rules instead: the rule for the undead first, a catch-all below it.

`Any`, `Combat start`, `Combat end`, the group extremes (e.g. `Health lowest`), and `Corpse: None` cannot be negated.

## Actions

Actions, like [conditions](#conditions), first require a target:

![Action target]({{ "/assets/img/panel/action_target.png" | relative_url }}){: .screenshot loading="lazy"}

`Ally`, `Enemy`, and `Corpse` are the one the condition matched. `Enemy` under a condition not about an enemy is whoever the character is fighting, or else the nearest enemy. `Attacker` is whoever last hit the condition's target.

Then an action:

![Action enemy attack]({{ "/assets/img/panel/action_enemy_attack.png" | relative_url }}){: .screenshot loading="lazy"}

**Actions are contextual**. A follower without [potions]({{ '/inventory/' | relative_url }}#potions) in their inventory will not see a `Potion` action. A [spell]({{ '/magic/' | relative_url }}) that targets `Self` will not show up under `Cast` for `Enemy`.

Long menus are grouped: spells and scrolls by magic school, weapons by type, with shields among them, and armor by the part of the body its `Unequip` names. Anything that fits no group is under `Other`. Short lists, lists that would all fall under one group, powers, shouts, and arrows remain flat lists.

**Actions respect gameplay rules**. A follower who doesn't meet the skill requirement to cast a spell cannot equip it. Per the base game, this limitation does not apply to the player.

**Equip actions [pin]({{ '/equip-states/' | relative_url }}#pinned) while their rule's condition holds**. An action that equips e.g. a Steel Sword in the right hand prevents the AI from equipping something else there. When the condition no longer holds, the pin is released: the sword stays equipped until the AI, another rule, or one of your own pins replaces it. `Unequip` likewise keeps the hand or slot empty while its condition holds.

**Rules override [equipment settings]({{ '/equip-states/' | relative_url }})**. Combat rules take precedence over idle rules, and idle rules over your own [pins]({{ '/equip-states/' | relative_url }}#pinned) and [bans]({{ '/equip-states/' | relative_url }}#banned). Each comes back when the rule over it no longer applies.

**`Combat start` equips last the whole fight; `Combat end` equips stay**, like pins you make yourself.

Player equip actions are ordinary equips and never create pins.

## Actions explained

Most actions are self-explanatory. A few are worth highlighting.

The `Any buff` action for `Potion` picks a random potion that adds or improves a buff, e.g. Fortify Conjuration. An alchemy effect already active at equal or greater magnitude does not count as an improvement.

A named potion or food is skipped while its effects would add nothing, including when its lasting effects are already covered by alchemy of equal or greater strength. Items whose effects cannot apply to the character are skipped too.

Spells, scrolls, powers, and shouts check whether their effects can apply to the target. A lasting cast waits while that spell is already active there; dual casting does not replace its own single cast, and a scroll and a spell are checked separately. Area effects consider only targets within each effect's reach. Reanimation checks whether the particular corpse can be raised, including its level and the caster's perks.

For potions, food, and ingredients, `Strongest` and `Weakest` choose by effect. `Weakest` keeps stronger items in reserve: if the weakest item's effect is already covered, it waits instead of consuming a stronger one. `Strongest` can replace a weaker active effect.

![Action any buff]({{ "/assets/img/panel/action_any_buff.png" | relative_url }}){: .screenshot loading="lazy"}

The `Charge` action for enchanted weapons has two policies. `Strongest` picks the largest gem that doesn't overfill the weapon, or the smallest carried if every gem would. `Weakest` picks the smallest gem carried, even if it overfills.

![Action charge soul gem]({{ "/assets/img/panel/action_charge_soul_gem.png" | relative_url }}){: .screenshot loading="lazy"}

`Unequip` takes off what is currently worn in the chosen hand, or for armor the chosen part of the body, whether pinned or not. The follower's AI can equip something there again afterward. For weapons and spells, `Unequip` offers `All`, `Right` and `Left`, `All` being both hands. Armor's parts are `All`, `Head`, `Body`, `Hands`, `Feet`, `Amulet`, `Ring`, `Cloak` and `Backpack`; `Head` covers helmets, hoods, masks and circlets alike. `Cloak` is slot 46 and `Backpack` slot 47, the slots the Anniversary Edition's cloaks and backpacks use, and most cloak mods use 46 too. `All` leaves a shield on: it is unequipped from the left hand under `Weapon`. Armor on a slot a mod adds comes off only with `All`.

### Dual casting and power bashing

Under `Settings` → `Requirements`, two switches control whether followers need perks for these actions:

- `Require Dual Casting perks`: requires the spell school's Dual Casting perk for `Dual Cast`.
- `Require Power Bash perk`: requires the Block tree's Power Bash perk for `Power Bash`.

Both switches are **off by default**, so followers can use these actions without the corresponding perks. Turning a requirement on hides the action for followers who lack the perk and makes existing actions that require it [unavailable](#unavailable).

A follower is never offered a spell above their skill in its school: their own AI would never cast it, so a rule or a pin on it would leave them doing nothing.

![Requirements settings]({{ "/assets/img/panel/settings_requirements.png" | relative_url }}){: .screenshot loading="lazy"}

These switches apply to all followers and are [saved with the game]({{ '/getting-started/' | relative_url }}#saving-your-changes).

The player always needs the corresponding perks for `Dual Cast` and `Power Bash`, regardless of these switches, and can cast any spell they know at any skill.

### Multiple Actions

More than one action is allowed for a rule.

![Action multiple]({{ "/assets/img/panel/action_multiple.png" | relative_url }}){: .screenshot loading="lazy"}

Actions are taken in order, one per tick. A rule with 4 actions will occupy the next 4 ticks, unless an action is skipped because it cannot be executed, in which case the next action is tried in the same tick.

### Cooldowns

An action that fires goes on cooldown for its target: about 3s for potions, food, poisons, and soul gems; 2s for casts, shouts, and attacks; 1s for equips. The same action on the same target is blocked meanwhile, whichever rule asks. Cooldowns reset when combat starts.

## Enabling / Disabling

### Whole party

`Enable tactics for party` in `Settings` controls both lists for the player and all followers.

![Settings]({{ "/assets/img/panel/settings.png" | relative_url }}){: .screenshot loading="lazy"}

### Per character

The `Enabled` switch controls the current list for that character. Combat and idle tactics can be enabled independently.

![Tactics char disabled]({{ "/assets/img/panel/tactics_char_disabled.png" | relative_url }}){: .screenshot loading="lazy"}

Clicking the `On` header toggles all rules in the current list.

![Tactics disable all]({{ "/assets/img/panel/tactics_disable_all.png" | relative_url }}){: .screenshot loading="lazy"}

This is useful for testing a particular rule, for example.

### Per tactic

A rule can be also be individually toggled on / off. Disabled rules are not considered during evaluation.

![Tactic disabled]({{ "/assets/img/panel/tactic_disabled.png" | relative_url }}){: .screenshot loading="lazy"}

### Unavailable

A rule whose [condition](#conditions) or [action](#actions) is invalid is unavailable. For example, a condition set for a follower who has been dismissed, or an action to drink a [potion]({{ '/inventory/' | relative_url }}#potions) that's no longer in the inventory.

![Tactic unavailable]({{ "/assets/img/panel/tactic_unavailable.png" | relative_url }}){: .screenshot loading="lazy"}

If there are multiple actions for a rule, the rule is unavailable if **all** actions are invalid. If only some actions are invalid, the invalid ones are skipped.

An action that cannot be done at the moment is also greyed, and its hover says why: not enough magicka for the spell, a shout still recovering, a greater power already used today, an action used too recently. This is read when the page is built, so it shows the state at the moment the panel was opened. An action that is simply already in effect, a buff still running or nothing to unequip, is not greyed: that is what the rule waits on.

Temporarily unavailable rules remain editable. You can change their conditions and actions without waiting for magicka or cooldowns to recover. Powers have their own availability checks and do not wait for shout recovery.

## Reordering rules

Rules run from top to bottom, so put higher-priority rules first. Drag a rule by its number and drop it at the insertion line, or use the arrows in the **Order** column to move it one place.

![Dragging a tactic to a new position]({{ "/assets/img/panel/tactic_drag_and_drop.png" | relative_url }}){: .screenshot loading="lazy"}
