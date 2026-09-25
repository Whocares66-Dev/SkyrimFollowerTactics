---
layout: "default"
title: "Combat AI"
permalink: "/combat-ai/"
nav_order: 12
has_toc: false
math: true
---

# Combat AI

Followers fight with Skyrim's own combat AI, and [tactics]({{ '/tactics/' | relative_url }}) override it only when a rule fires. Two switches under `Settings` → `Combat AI` change how the AI picks a follower's weapon and attack spell. Both are on by default, apply only to followers, and are [saved with the game]({{ '/getting-started/' | relative_url }}#saving-your-changes).

![Combat AI settings]({{ "/assets/img/panel/settings_combat_ai.png" | relative_url }}){: .screenshot loading="lazy"}

## Varied AI choices

In the base game, the AI rescores its weapons and spells once a second and always takes the highest score, so a follower casts the same spell over and over. Its attack-spell score does not account for perks or magicka cost.

With `Varied AI choices` on, an attack spell is scored by what it does per second of the follower's time, counting magicka as time:

$$
s_i = \frac{d_i}{t_i + p\,c_i},
\qquad
p = \left(1 - \frac{m}{M}\right)^{2} \min\!\left(\frac{1}{\rho},\, 5\right)
$$

- $$d_i$$ is the effect value against the current enemy over one casting cycle: the game's own score, raised by perks such as Augmented Flames, and $$0$$ if the enemy is immune.
- $$t_i$$ is the cycle time and $$c_i$$ its magicka cost. For a released spell, time is charge plus the AI’s hold before release (at least 0.1 s); for a concentration spell, value, time, and cost cover the same scoring interval.
- $$p$$ is what a point of magicka is worth in seconds: $$0$$ with magicka $$m$$ at its maximum $$M$$, rising to the time one point takes to regenerate ($$\rho$$ points a second, at most 5 s) as it runs out. With no regeneration, use 5 s per point; with no maximum magicka, use $$p=0$$. The fraction $$m/M$$ is clamped between 0 and 1.

Full magicka favours effect per second; low magicka increasingly favours efficiency. Staves cost no magicka. Scrolls are saved for low magicka: their score is multiplied by $$(1 - m/M)^2$$.

The score is then varied:

$$
s_i' = s_i \, r_i \, \operatorname{clamp}\!\left(e^{g_i-\gamma},\,0.1,\,10\right)
$$

- $$r_i$$ penalizes recent use. With $$a_1, \dots, a_8$$ the follower's last eight attack casts, newest first,

  $$
  r_i = \prod_{k \,:\, a_k = i} \left(1 - 2^{-k}\right)
  $$

  so the last cast halves a spell's score, the one before takes a quarter off, and so on.
- $$g_i$$ is a random draw from the Gumbel distribution, and $$\gamma \approx 0.577$$ centres its logarithm on 0.

The AI still takes the highest adjusted score. For a fresh draw among eligible attack-spell entries, the approximate chance is

$$
P(i) \approx \frac{s_i \, r_i}{\sum_j s_j \, r_j}
$$

An entry with twice the score after the recency penalty has roughly twice the chance. This would be exact without the 0.1–10 cap; actual casts also depend on range, available hands, and the game’s casting checks.

Each hand’s spell entry keeps its draw until that spell is cast and the minimum hold time has passed, the enemy changes, the entry becomes unusable, or the fight ends. This avoids rerolling every second. A cast from either hand counts toward the shared recency penalty.

Weapons are scored by their damage against the current enemy after perks such as Armsman and Overdraw, with no random factor. They still compete with attack spells, so a spellsword sometimes casts where they would have swung.

Heals, wards, cloaks, and other buffs keep the game's own choice. The [combat style]({{ '/combat-style/' | relative_url }}) still weighs magic against weapons.

### Example

A follower knows Fireball, Firebolt, and Ice Spike.

#### Before

Fireball scores highest, so they cast it over and over while it remains usable.

#### After

They mix in Firebolt and Ice Spike instead of repeating Fireball. Recent casts are less likely to repeat, fire spells are favoured against frost-resistant enemies, and the cheaper Firebolt becomes more attractive as magicka runs low.

## Self-targeting damage spells

In the base game, NPCs never cast spells that damage enemies around the caster, such as Fire Storm. With `Use self-targeting damage spells` on, followers score them by the damage they would do to every enemy in reach. They cast them when enemies are close, and more readily in a crowd.

Followers do not check for allies nearby. [Ban]({{ '/equip-states/' | relative_url }}#banned) the spell to stop a follower casting it.

## Always on

- While a rule waits to cast a spell, the follower's own spells stand down, so the rule is not held up.
- For every NPC, a spell that would dispel an effect already running on the caster waits until that effect ends. Two cloaks are no longer cast in turn.
- For every NPC, a staff stays in use while it has the charge for a cast. The base game dropped it earlier.
