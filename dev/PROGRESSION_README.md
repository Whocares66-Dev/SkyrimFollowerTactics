# Follower Progression

**Follower Progression** lets Skyrim companions level the way you do. A spell cast, a blow landed, a hit taken raises the skill used, by your own levelling's rules read from the game; skill-ups make levels, and each level brings an attribute point and a perk point for you to assign. A skill's levels can be moved to another, or a skill reset, for free. Spell tomes you carry can be taught to them. Nothing a follower was recruited with is taken away, and every change can be traced to its cause and undone.

Tactics decides *when* a follower does something; Progression decides *what they are able to do*.

**Folded into Follower Tactics on 2026-09-21** (branch `wip-progression`): the code is `src/progression/` (`core/` tested by `tests/progression/`, `game/` in the plugin), its pages are under *Follower Tactics / Progression* in the panel, its records are in Tactics' co-save block, and Tactics' Settings page has the switch, *Enable leveling for followers*. The stand-alone repository (`C:\project\SkyrimFollowerProgression`) keeps the history before that.

**Status: proof of concept.** The rules are built and tested; the SKSE plugin builds and has not yet been run in the game. See [dev/POC.md](POC.md).

## Documents

- [dev/PROGRESSION.md](PROGRESSION.md) — the design: pillars, learning by doing, levels, points and reassigning, perks, spells, reconsidering, the interface, Tactics.
- [dev/POC.md](POC.md) — what was built, what is verified, and the checklist for the first session in play.
- [dev/ENGINE_PERKS.md](ENGINE_PERKS.md), [dev/ENGINE_SPELLS.md](ENGINE_SPELLS.md), [dev/ENGINE_SKILLS.md](ENGINE_SKILLS.md) — how the engine answers an NPC's perks and spells and levels the player, read from the executables; the views put in front of it so no record is edited, and where a companion's skill use is caught.
- [dev/BRAINSTORM.md](BRAINSTORM.md), [dev/DESIGN.md](DESIGN.md), [dev/PRIOR_ART.md](PRIOR_ART.md) — the research and engineering plan the design builds on.

## Building

Tactics' build (CLAUDE.md): `fp_core` and `fp_tests` are built and run with Tactics' own, and the plugin carries Progression.

## Requirements in game

As Tactics': SKSE64, Address Library for SKSE Plugins, and SKSE Menu Framework for the panel (F1). Skyrim SE and AE; on VR the perk, spell and learning hooks are not installed.

Levelling can be turned off at any time: Tactics' Settings → *Enable leveling for followers*. Off takes everything of Progression's off each companion as they come near and keeps the record of it; on puts it back. To remove the mod from a playthrough, turn it off, wait until the page no longer lists anyone as still levelled, and save.

## License

[GPL-3.0](LICENSE).
