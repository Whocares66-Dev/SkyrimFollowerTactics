# Dungeon and Camp classifications

Issue [#25](https://github.com/Whocares66-Dev/SkyrimFollowerTactics/issues/25): an idle Candlelight rule on Any dungeon fired outdoors near dungeon entrances and the Guardian Stones. A location's `LocTypeDungeon` tag does not tell us whether the actor is inside. Embershard's interior and entrance share one location, and giant camps also carry that tag.

Dungeon means an enclosed dungeon, cave, mine, fort or ruin. Camp means the outdoor part of such a site, including outdoor dens, roosts and groves. Normal houses and inns remain buildings; a road or standing stone is neither a dungeon nor a camp. Every specific occupant choice also sets its group's Any. Cave requires enclosure; Fort and Ruin remain structural choices that can match indoors or outdoors.

Interior cells are enclosed. For exterior cells, a reviewed worldspace entry takes precedence; otherwise No Sky or interior lighting supplies the enclosure fallback for mod-added caves. This physical context is separate from the Interior/Exterior condition, weather and brightness, which retain their existing reads. An actor without a cell matches neither Dungeon nor Camp.

Location keywords supply fallback evidence for unreviewed and mod-added sites. `core/Places.h` owns the reviewed corrections and indoor/outdoor occupant pairs; `game/Places.cpp` resolves portable plugin-local IDs once and reads the actor's cell, worldspace and location chain. No plugin records are edited. Corrections apply to the load-order winner of the defining record, even when a later plugin overrides it. They add missing classifications and remove misleading tags before parent inheritance. Underground sites stop settlement inheritance from their parent town.

## Reviewed locations

Read with houseCARL 1.9.0 on 2026-10-06, MO2 instance `C:\modding\MO2`, profile Default (80 active masters/Creation Club plugins). The three main masters contain 757 locations; this is a targeted audit of 16 locations and four worldspaces, not a complete catalog. [Raw records](research/locations-gh25.json) retain the actual record names, keyword IDs, resolved keyword identities and winning source. Guardian Stones has no record name; the human label below is not a name read from the record.

| Defining plugin | Local location ID | Name / label | Classification | Reason / correction |
|---|---|---|---|---|
| Skyrim.esm | `0B712C` | Embershard | Dungeon / Bandit hideout / Cave inside; Camp / Bandit camp outside | One shared location; enclosure decides. |
| Skyrim.esm | `10FE43` | Guardian Stones (label) | Neither Dungeon nor Camp | No site tags; explicit exclusion also removes erroneous added site tags. |
| Skyrim.esm | `018EEB` | Bleakwind Basin | Camp / Giant camp | Its Dungeon keyword does not make the open-air camp enclosed. |
| Skyrim.esm | `0C2EF8` | Snowpoint Beacon | Camp / Bandit camp | Occupant tag suffices despite the missing Dungeon keyword. |
| Skyrim.esm | `0B2395` | Dragon Bridge Overlook | Camp / Forsworn camp | Same missing-head case. |
| Skyrim.esm | `04787B` | Roadside Ruins | Camp / Spriggan grove | Open-air spriggan site, not an enclosed dungeon. |
| Skyrim.esm | `03B871` | Riften Ratway | Dungeon | Add missing dungeon classification and stop the parent city. The active CC override adds homestead tags rather than Dungeon. |
| Skyrim.esm | `02BCEB` | The Midden | Dungeon | Underground complex; replace the guild dwelling classification and stop Winterhold inheritance. |
| Skyrim.esm | `0E2502` | Markarth Ruins | Dungeon / Dwarven ruin | Add missing dungeon and ruin classifications; stop Markarth inheritance. |
| Skyrim.esm | `022639` | Esbern's Vault | Dungeon | Underground Ratway vault; add missing dungeon classification and stop Riften inheritance. |
| Skyrim.esm | `018C91` | Cidhna Mine | Dungeon | Underground mine; the Jail keyword does not make it an ordinary building. Stop Markarth inheritance. |
| Skyrim.esm | `01914D` | Broken Fang Cave | Dungeon / Vampire lair / Cave | Remove the misleading military-fort tag. |
| Skyrim.esm | `016F85` | Bloodlet Throne | Dungeon / Vampire lair / Fort inside; Camp / Vampire camp / Fort outside | Retain its fort classification; no record correction. |
| Skyrim.esm | `0192B9` | Nightcaller Temple | Dungeon / Warlock lair / Temple | Temple complex; replace military-fort classification with Temple/Building. |
| Skyrim.esm | `018EE8` | Blackreach | Dungeon / Falmer hive / Cave | Enclosed worldspace even though its cells are exteriors. |
| Dawnguard.esm | `004C20` | Castle Volkihar | Existing location kinds | The reviewed worldspace context distinguishes the courtyard; this location has no dungeon/occupant tags. |

## Reviewed worldspaces

| Defining plugin | Local worldspace ID | Name | Physical context | Record evidence |
|---|---|---|---|---|
| Skyrim.esm | `01EE62` | Blackreach | Enclosed | No Sky; underground cavern. |
| Skyrim.esm | `02C965` | Darkwater Pass | Enclosed | Interior lighting; cave worldspace. |
| Dawnguard.esm | `007202` | Volkihar Courtyard | Open air | Has interior lighting despite being an outdoor courtyard. |
| Dawnguard.esm | `001408` | Soul Cairn | Open air | Exterior landscape, no interior lighting or No Sky. |

## Existing profiles

Wire names are retained. Bandit camp, Forsworn camp, Giant camp, Riekling camp and Spriggan grove move to Camp and now match outdoors only. For their interior counterparts, choose Bandit hideout, Forsworn hideout, Giant den, Riekling den or Spriggan den under Dungeon. Other old dungeon occupant choices now require enclosure; Camp provides their outdoor equivalents. Any dungeon no longer matches outdoors. No save-schema migration is needed.

## Verification

Headless regression tests cover the shared Embershard location, the actual idle rule and negation, missing Dungeon tags, ordinary buildings, reviewed worldspace enclosure, corrected underground locations and every occupant pair. In play, verify Candlelight stays off at the Guardian Stones and Embershard entrance, turns on inside Embershard and Blackreach, and that Camp > Giant camp matches Bleakwind Basin. Also inspect the Ratway/Midden classifications and the translated menu. These engine reads have not yet been verified in play.
