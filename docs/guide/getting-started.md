---
layout: "default"
title: "Getting started"
permalink: "/getting-started/"
nav_order: 1
has_toc: false
---

# Getting started

## Install

One DLL runs on **Special Edition 1.5.97** and every **Anniversary Edition** build, 1.6.317 through the current 1.7.104; the Address Library resolves each runtime. VR is not supported. Development and in-game verification use 1.5.97, 1.6.1170 and 1.7.104.

The following dependencies are required; install versions matching your Skyrim runtime:

- [SKSE64](https://www.nexusmods.com/skyrimspecialedition/mods/30379)
- [Address Library for SKSE Plugins](https://www.nexusmods.com/skyrimspecialedition/mods/32444)
- [SKSE Menu Framework](https://www.nexusmods.com/skyrimspecialedition/mods/120352)

Install normally via your preferred mod manager (e.g. [MO2](https://www.nexusmods.com/skyrimspecialedition/mods/6194)).

## Compatibility

Compatible with vanilla followers (including Serana, who is highly custom), as well as most follower management systems, such as [Simple Follower Framework](https://www.nexusmods.com/skyrimspecialedition/mods/174017).

Some heavily scripted followers, like Megara from [Katana - Journey in the Shadows](https://www.nexusmods.com/skyrimspecialedition/mods/69622), may actively fight against some features, such as weapon [pinning]({{ '/equip-states/' | relative_url }}#pinned). Other features, drinking potions via tactics, work fine.

## Uninstall

Leave combat, save, and uninstall normally.

## Interface

To bring up the interface, open the SKSE Menu (default key <kbd>F1</kbd>) and select `Follower Tactics`.

![Interface]({{ "/assets/img/panel/settings.png" | relative_url }}){: .screenshot loading="lazy"}

The player and followers start with no [tactics]({{ '/tactics/' | relative_url }}). Add combat rules on the `Tactics` tab and out-of-combat rules on `Idle Tactics`. Nothing changes until you add a rule.

## Saving your changes

Both tactics lists, their enable / disable switches, pins, bans, and Settings choices are saved when you save the game, including quicksaves and autosaves. There is no separate save button for the mod.

Loading an earlier save restores the choices saved with it. Closing the panel does not save changes, and quitting without saving loses them. These choices belong to each game save; they are not shared across characters or new games.

## Reporting problems

The log is `Documents\My Games\Skyrim Special Edition\SKSE\FollowerTactics.log`. `FollowerTactics.events.jsonl` beside it records what tactics did, one event per line. Earlier sessions are kept in `SKSE\FollowerTactics\`.

For more detail, create `Data\SKSE\Plugins\FollowerTactics.ini`:

```ini
[Log]
level = debug
```
