# Actions: heal, buff, debuff, shouts, transformations, dual casting, attack, powers and food

What the engine lets a follower do on request, from research done
2026-09-04 (sources at the end). The existing actions are potions, a cast
through a UseMagic package, and the equip pins. This is about what the
next ones can be built on, and what they cannot.

## 1. Heal, buff and debuff on someone else

**Vanilla combat AI never casts a beneficial spell on anyone but itself.**
The caster classes in its list are offensive, restore-self, ward, armour,
summon, bound weapon, cloak, invisibility, light, disarm, paralyse,
reanimate, stagger, target-effect and scripted; `Restore` is self only.
Follower-healer mods (Be With Healer, MSFF, the 3DNPC healers) all do it
the way we already cast: a package that overrides combat with the spell
and a target input.

So **heal / buff an ally is our CastSpell with a target**, and the pieces
are in place: the package's Target input takes a single reference, the
lease repoints it per cast, and the step already carries a target actor.
What is missing is (a) Execute honouring the step's target for a targeted
spell rather than always the follower's combat target, and (b) a way to
choose the target in the editor. Both are small. Aimed delivery (Healing
Hands, a stream) at a moving ally is unverified; Heal Other and Grand
Healing are fire-and-forget or area and safer to start with. Healing Hands
on a follower can aggro them on some records; Heal Other does not.

**Debuffs do not exist as castable spells in vanilla.** Weakness to
Fire/Frost/Shock/Magic/Poison are alchemy effects, poisons, abilities and
one shout (Marked for Death); no spell record references them. Apocalypse,
Odin and Mysticism add castable ones. So "debuff enemy" is: poisons on the
weapon (a Use Poison action, which NPCsUsePotions already does for NPCs),
Marked for Death if the follower can shout, or a spell mod. Buffing an
ally's armour or resistance is the same story: vanilla's Stoneflesh line
and the resist spells are self-only; targeted versions need a mod's
records or ones of our own in the ESP.

## 2. Shouts

**Any NPC with the shout in their spell list can shout**; the voice type
only decides whether the words are audible. Ulfric and the Greybeards are
ordinary actors with a TESShout listed; Shouty People distributes seven
shouts through SPID and they are used. The combat AI scores a shout entry
by `shoutScoreMult` and equips it when the voice recovery time is below a
game setting.

**The cooldown is per actor**, in the high process data: `voiceRecoveryTime`
and `voiceTimeElapsed`, read by `Actor::GetVoiceRecoveryTime()`. Each word
has its own recovery time on the shout record, scaled by the
ShoutRecoveryMult actor value. CommonLib has no setter; the field can be
written.

**On demand:** the engine has a `Shout` package procedure (Shout, Target,
HoldWhenBlocked), used by the Greybeard training; it is the shout twin of
UseMagic and splices into the alias the same way. `EquipShout` alone only
selects the power; the package or the AI fires it. Untested whether the
procedure honours the recovery time.

So a **Shout action** is: an ESP `Shout` package pool beside the UseMagic
one, a lease per cast, a snapshot field for the recovery time so the rule
can report "cooling down" rather than fire into it, and the follower must
know the shout (a `Learn shout` action, or the player teaches it).

## 3. Transformations

Werewolf and Vampire Lord are **powers whose effect scripts support
NPCs**: the effect always casts the transform-visual spell, which on a
non-player does `SetRace(WerewolfRace)` / `SetRace(DLC1VampireLordRace)`
after the animation event. Vanilla uses it on Sinding, Aela and Harkon.

What an NPC does **not** get is everything the player quest handles:
duration, the shift back, feeding, re-equipping gear afterwards. That is
ours to do: `Actor::SwitchRace` back to the original race on a rule or a
timer, and put the outfit back on (the pin book can hold it). Vampire Lord
combat on an NPC is the known problem, magic mode "extremely buggy with
NPCs"; werewolf mods (Beastblood, The Beast Within, Growl) are all script
and all cope. A **Transform action** is therefore: cast the power at the
follower through the instant caster or a UseMagic package, remember the
race, and a paired **Revert** with the outfit restore. Plausible; test
werewolf first.

## 4. Dual casting

**NPCs can dual-cast; vanilla mage bosses do.** The combat behaviour tree
has a PrepareDualCast node, and 139 vanilla NPC records carry the school
dual-casting perks. The gate is the perk entry point CanDualCastSpell,
which the school's Dual Casting perk sets, so a follower needs that perk
(`Actor::AddPerk`). No combat-style flag is involved.

Through **our package**, the UseMagic procedure has a DualCast boolean
input, false on our eight slots today; flip it per lease as the spell and
target are repointed. Whether the perk is still required through the
package is unverified. Direct API: dual cast is state on the hand caster
(`ActorMagicCaster::SetDualCasting`), not a cast argument. Watch for po3's
Dual Casting Fix: scripted casts clear the dual state, and our faction-rank
abilities might too.

## 5. What to build, in order

1. **Cast on a chosen target**: Execute takes the step's target; the
   Then cascade's first level names it (self, the ally or enemy the
   condition matched, the player, a named follower, the target, the
   attacker), as the If cascade's names the subject. Heal the player, heal the hurt ally, buff the one about
   to engage. Vanilla spells only.
2. **Use poison** on the weapon: done, the Apply cascade (below).
3. **Shout**: the package pool, the recovery time in the snapshot.
4. **Dual cast** as a flag on a cast action, needing the perk.
5. **Transform / Revert**, werewolf first.

## 6. Attack: focus aggression on one enemy

Research 2026-09-04. **The engine has no "attack" verb.** An actor in combat attacks whoever the combat controller's *target selector* has chosen; the behaviour tree and the inventory scoring then decide how (swing, shoot, cast). So the action is "focus on this enemy", and once the target changes the follower's own scoring picks the weapon or spell for it. Nothing here needs to branch by class.

**What exists, verified against the 3.7.0 headers in this build tree:** `CombatController` has `targetHandle`, `previousTargetHandle`, a `targetSelectors` array and a `currentTargetSelector`; `Actor` mirrors the target as `currentCombatTarget`. Two selector classes are in the RTTI and vtable tables, `CombatTargetSelectorStandard` (the AI's own choice) and `CombatTargetSelectorFixed` (the engine's own "this one, regardless"). Neither has a mapped layout, so using the fixed one means reverse engineering. The `CombatGroup` holds one `targets` list for the whole group with a threat value and attacker count per target; followers and the player share a group.

**What the game files say (Skyrim.esm through houseCARL):** no game setting tunes target selection. The 96 `*Combat*` settings cover stealth points, regen, dialogue timers and radii; the only threat-related one is `fCombatThreatRatioUpdateTime` = 5 s. The INIs (game default, My Games, MO2 profiles) carry no target or selector settings either. So the standard selector's weights are hardcoded and can only be measured.

**Papyrus `StartCombat(target)` is not the tool.** The CK wiki says only "attempts to get this actor to initiate combat with the target". All 155 vanilla scripts that call it start a fight (Jorrvaskr brawl, Vilkas training, Cicero, the Silver Hand ambush); none retargets an actor already fighting, and that case is undocumented. A one-console-command test would settle what it does mid-combat.

**Bethesda's own "keep attacking this one" is a UseWeapon combat override.** Karliah and Brynjolf in Blindsighted run `TG08B*UseWeaponCombatOverride`: template `UseWeaponAlreadyHeld`, Target to Attack = an alias, Never End = true, `InterruptOverride = Combat`. The template's inputs are Weapon Type, Target to Attack, Always Hit, Do No Damage, Hold when Blocked, Never End, barrage counts and pauses, Max Time spent Attacking, Always Power Attack, Headtrack Target. The CK wiki adds: the actor attacks only "if the actor has a weapon of the specified Weapon type", and with an object list only the first target counts. So it is weapon-bound: a caster would need UseMagic instead, and the package drives the attacks rather than the combat style. Our eight `FT_CastSlot` records are the UseMagic twin (template `UseMagic`, flag IgnoreCombat, faction-rank condition) spliced at runtime to the front of the follower alias's combat-override form list, so a UseWeapon pool would splice the same way.

**Routes, in the order to try them:**

1. **Assert the target directly.** Each tick while a focus lease is held, write `targetHandle` and `currentCombatTarget` to the enemy and measure whether the standard selector snaps back before the next 500 ms tick. Twenty lines. If it holds, the whole AI stays in charge of everything but the target, which is the "bias, don't puppet" principle exactly.
2. **Hook the standard selector.** A vtable write like the score hook, returning our target while the lease lives. Cleanest, but the virtual's index and signature are unknown. Unverified.
3. **A UseWeapon combat-override pool**, spliced like the cast slots, with UseMagic for casters. Heaviest, class-branching, and it puppets.

Availability: the target is alive, hostile and in the follower's combat group. Release on death, on combat end, or after a timeout, like a cast lease.

**Built 2026-09-04 as the `Target` action, route 1.** `IF Ally: Attacked by Ranged THEN Attacker: Target` is the shape. Core: the action is available only in combat, only when its target is one of the snapshot's enemies, and not when they are already the follower's current target, so a standing rule falls through instead of re-firing; its cooldown is 2 s keyed by the action alone, so two Target rules cannot flick the follower between two enemies on consecutive ticks. Game: `Execute` writes the combat controller's `targetHandle`, `previousTargetHandle` and `cachedTarget`, and the actor's `currentCombatTarget`, and logs the old and new target. There is no separate instrumentation for the open question: if the standard selector lets the choice stand the rule reports "already fighting them" on the next tick, and if it snaps back the rule fires again after its cooldown, so the log answers it. Not yet measured in play. Ranged is a damage kind of its own: a physical hit whose event carries a projectile; a blow without one is Melee.

## 7. Use power, eat food and ingredients (built 2026-09-04)

**A power is a spell record** (`Type = Power` or `LesserPower`, `EquipmentType = Voice`, no cost). Embrace of Shadows 00088821 is one: Self, fire-and-forget, invisibility and night eye for 180 s. The racial powers live on the race record's spell list (`TESRace::actorEffects`), which the spell walk now covers, so an Imperial follower shows Voice of the Emperor under Powers without a console grant. Vanilla NPCs never use theirs: the combat inventory lists logged in `docs/MAGIC.md` had no power entries.

**The UseMagic package does not fire a power. Measured 2026-09-04**, Voice of the Emperor on Marcurio: on every request the package was selected (`current package after evaluate: OURS`) and the AI never cast it. Five leases, five `deadline, never cast`, not one spell-fire or shout event in between, only his own `CastStop`. The likely reason is the equip type: the procedure casts from a hand, and a power's slot is the voice. Nothing about the record is missing (no asset, no effect); it is the wrong performer.

**So `Use power` is performed through a Shout slot. Works in play (2026-09-05): Marcurio shouts Voice of the Emperor and the bandit stops fighting.** Skyrim.esm carries `ImperialVoiceOfTheEmperor` (0E40CB), a Shout record whose first word's spell IS the power spell 0E40CA, referenced only by the guards' shout-reaction dialogue: the game's own way of treating that power as a shout, and the player's power animation is the shout animation. The `Shout` package procedure is the UseMagic twin that casts from the voice. The ESP has eight of each: `FT_ShoutSlot1..8` (0x819..0x820, template Shout, IgnoreCombat, `GetFactionRank(FT_CastNow) == 8..15`, Location NearSelf, Target Self, HoldWhenBlocked off, copied from `MQ206HeroDragonrendShout`), each pointing at its own wrapper `FT_PowerShout1..8` (0x811..0x818, one word, spell = the Fast Healing canary), each word `FT_PowerWord1..8` (0x809..0x810). The pool in `Packages.cpp` is one array of sixteen slots, rank = slot index; `RequestShout` takes a free slot from the upper eight, writes the power into `wrapper->variations[0].spell` (a plain pointer on the record, no canary probe), adds the wrapper to the follower's base shout list for the lease (`TESSpellList::SpellData::AddShout`; the Shout procedure only fires a shout the actor has), and arms it as a cast is armed. Release is the voice's fire event; the deadline (3 s) is the never-started fallback, and steps back once on `BeginCastVoice` so a shout that has begun is never cut off.

Four runs settled three things, each by one change per run:

1. **The spell in the word must be Type Voice.** With a Power-typed spell in the word the follower shouted (`BeginCastVoice`, `Voice_SpellFire_Event`, the wrapper current) and nothing landed, every time. Every vanilla word spell is Type Voice. So for the lease the power's record reads `kVoicePower`, in memory, restored on release and on a game load; `IsLeasedPower` keeps it listed under Powers meanwhile. The alternative, our own Voice spells in the ESP carrying the power's effects, would need the active-effect check taught which spell stood for which power; this needs nothing, and the window is the two seconds of the lease.
2. **An NPC shouts the highest filled word.** With all three words filled the engine shouted variation 2, the three-word animation, a second long; with word one alone, variation 0, 0.3 s. Bethesda's own NPC shouts (Greybeard, draugr) fill all three and steer strength by word three's spell, which is the same rule seen from the other side. So a power goes in word one alone.
3. **No perk is needed.** The player's `AllowShoutingPerk` (0F11A9, entry point ModShoutOk, conditioned on Dragon Rising stage 90) is the player's alone: Arngeir has no perks and the Ebony Warrior's 36 do not include it. A copy without the condition was granted for the lease in one run and made no difference; removed.

Voice of the Emperor's Pacify is a calm on people. It showed nothing on the cave bear and stopped a bandit, which is why `bat ftman` exists (`docs/TESTING.md`).

**The Shout action** (section 2) rides the same slot with the shout itself in the package's Shout input, no wrapper and no type change: the follower already has the shout, and its words are Voice spells. A shout's delivery, for the menu, is its first word's. Built 2026-09-05, not yet played. The per-actor voice recovery (`Actor::GetVoiceRecoveryTime`, the high process's `voiceRecoveryTime` less `voiceTimeElapsed`, set from the word's recovery when a shout fires, NPCs included) is in the snapshot, and a Shout or Use power rule inside it reports Recovering and waits, spending no cooldown: the engine's own cooldown, not a fixed one of ours. The wrapper's word carries a one-second recovery, so a power is gated by the same number.

**Equip power is deliberately absent.** A pin is a promise the AI will use the thing, and the vanilla AI never reaches for a power. The Magic tab's Equipped cell for powers and shouts stays read-only. A mod that has NPCs choose between powers would make it one more kind in the pin book.

**Charge with a soul gem** and **Apply a poison** are the Then menu's Charge and Poison entries, a group of their own between the casts and the equips.

**Charge** spends a filled soul gem into the enchanted weapon in hand that cannot pay for its next hit, the right hand before the left. Strongest is the largest gem carried that would not overfill it; when every gem would, the smallest, which overfills least; weakest is the smallest carried; then every spendable gem by name with its soul, "Common Soul Gem (Lesser)". A reusable gem (Azura's Star, the ReusableSoulGem keyword) is a candidate like any other and is emptied rather than removed, as the engine's own routine does: the soul it holds is ExtraSoul on its inventory entry, and that is what comes off. The rule reports "no weapon" without an enchanted weapon in hand, and "charged" and waits when none in hand needs a charge. Performing it is what the engine's own recharge routine does, read from the executable: the gem's soul value (the five `iSoulLevelValue` settings, 250 to 3000) through the Mod Soul Gem Recharge perk entry point, added to what is left and capped at the full charge, written to ExtraCharge on the worn copy; the weapon's ability refreshed -- which is the step that copies the record into the hand's live actor value; the gem removed, or a reusable one emptied; the `UIEnchantRecharge` sound. The weapon's sheet shows the charge as "89 / 100 (89%)".

**Where the live charge is.** A wielded enchanted item's charge is not on the item: the engine keeps it as an actor value, `RightItemCharge` or `LeftItemCharge` by hand, which the HUD meter and the container menu read, and writes it back to the item's charge record only on unequip. Measured 2026-09-08: a staff cast down showed no charge record until it changed hands, and then 491 appeared. So the scan reads the hand's actor value for a weapon in hand and the record for one in the bag. Built and played 2026-09-08.

**Apply a poison** is the cascade's other half. Six policies as the potions have them -- the strongest health, stamina and magicka poison, a divider, the weakest of each -- then every poison carried by name. A poison's strength is its largest magnitude on the actor value it damages; kinds that damage none of the three (Frenzy, Fear, Paralysis, a weakness) are offered by name only, because their magnitudes do not compare. The rule needs a weapon in hand that takes a poison (anything but a staff) and reports "no weapon" without one; it goes to the right hand's weapon if that is clean, else the left's, so two firings dress a dual-wielder's both hands, and with every weapon in hand poisoned it reports "poisoned" and waits, as a buff rule waits on its buff. The engine's own routine, read from the executable: the inventory menu poisons the RIGHT hand's weapon only, never the left, refuses a second poison with "The current weapon is already poisoned." rather than replacing it, excludes staffs, and gives one dose plus the Concentrated Poison perk's entry point. The `Weapon: Poison none / active` condition (`docs/CONDITIONS.md` 7) is the pair for it. Performing it adds one dose to the worn copy's extra list and takes one bottle from the bag: the engine's own `PoisonObject` writes to an entry's first list, which for a follower carrying two of the sword need not be the one in hand. One dose, as the inventory menu gives a player without the Concentrated Poison perk. The vial sound the menu plays (`ITMPoisonUse`, a UI sound) is played too; the engine has no character animation for poisoning, the player's own goes on inside the menu. The Inventory tab lists poisons under their own heading, and a poisoned weapon carries the poison glyph after its name with a Poison section (name, hits left, effects) on its sheet: a dose is hits, not seconds, and stays on a sheathed weapon. Verified in play 2026-09-08: a dagger left and a sword right both dosed by two firings.

**Strongest and weakest.** The Potion list opens with the three "drink strongest" policies, a divider, then the three "drink weakest", then every potion carried by name. Neither is a list of potions: the sensor reads every carried potion's effects each tick, counts one whose primary actor value is Health (or Magicka, Stamina) and is not a poison or food, and ranks it by its largest such magnitude. So a custom or player-brewed potion is found on its own. Weakest is for the everyday case, the cheap potion first with the strong one kept; strongest for the emergency. Added 2026-09-08.

**Food and ingredients** are the two lists beside Potion at the top of the Then menu. Both go through the same `EquipObject` call as a potion; the game consumes the item through its normal path. The snapshot tags every carried consumable with its kind, and a named consume rule checks form AND kind, so a hand-edited profile cannot drink a cabbage. Unverified in play: whether an NPC gets a food's or an ingredient's effect (the player eating an ingredient learns its first effect; an NPC has nothing to learn, and what the engine does instead is not documented). The potion's 3 s settle is used for both until one of their own is measured.

The Then cascade under Self reads, the more active thing first and a divider between the groups: Potion, Food, Ingredient; Cast, Shout, Power; Charge, Poison; Weapon, Armor, Arrows, Spell -- no Equip, Consume or Weapon heading above them (2026-09-08). Under Enemy and Attacker: Target, a divider, then Cast, Shout, Power. Under the player, an ally or a named follower: Cast, Shout, Power; under a corpse, Cast alone. A heading with nothing under it -- no food carried, no spell that suits the target -- is not drawn, and the dividers follow what is.

## Sources

CK wiki UseMagic (Procedure), UseWeapon (Procedure), Shout (Procedure), StartCombat - Actor, GetCombatTarget - Actor, IsCombatTarget, Package Flags, Combat
Style, Magic Effect; UESP Shouts, Magic Overview, Weakness to Fire;
CommonLibSSE-NG ActorMagicCaster and MagicCaster; powerof3/DualCastingFix;
Nexus: Shouty People, Heroes of Yore, Shout Recovery Utilities, Vampire
Lord Traveler, Vampire Bloodline, Beastblood, The Beast Within, Growl, All
NPC use Healing, Be With Healer, MSFF, Apocalypse, Odin, Mysticism,
Apocalypse Spells For NPCs; 3DNPC's Combat AI article; vanilla Papyrus
sources (WerewolfChangeEffectScript, DLC1VampireTurnScript,
DLC1VampireChangeEffectScript); Skyrim.esm records through houseCARL (the UseWeapon, UseWeaponAlreadyHeld, UseWeaponMultiTarget and UseMagic templates, the TG08B combat overrides, dunCGUseWeaponArcher, the Combat game settings); CommonLibSSE-NG CombatController, CombatGroup, Offsets_RTTI and Offsets_VTABLE.
