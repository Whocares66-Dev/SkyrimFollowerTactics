---
layout: "default"
title: "About"
permalink: "/"
nav_order: 0
has_toc: false
---

# About

**Follower Tactics** is a Skyrim mod for player and follower tactics, with follower equipment management. It is inspired by similar systems in [Dragon Age: Origins](https://dragonage.fandom.com/wiki/Tactics_(Origins)) and [Pillars of Eternity II](https://www.gamepressure.com/pillars-of-eternity-2/partys-ai/z1ae65).

![Main]({{ "/assets/img/panel/main.png" | relative_url }}){: .screenshot loading="lazy"}

## Features

- [Tactics]({{ '/tactics/' | relative_url }}) for the player and followers, with separate combat and idle rules
- Inspection of player and follower [inventory]({{ '/inventory/' | relative_url }}), [magic]({{ '/magic/' | relative_url }}), [skills]({{ '/skills/' | relative_url }}), and more
- Follower [equipment management]({{ '/equip-states/' | relative_url }})
- SKSE plugin, no ESP, easy uninstall

## Non-features

This mod is focused on helping you understand followers and set appropriate tactics. It is not a:

- Follower management framework
  - See options like [Simple Followers Framework](https://www.nexusmods.com/skyrimspecialedition/mods/174017)
- Follower customization system
  - Changing leveling schemes, teaching followers new spells, etc.
- Combat AI overhaul
  - Underlying AI behavior is preserved, only occasionally overridden by tactics
- Gameplay rebalancer
  - Some followers are over or underpowered. This is not addressed.
