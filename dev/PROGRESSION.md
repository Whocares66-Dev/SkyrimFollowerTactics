# Companion progression — the design

Written 2026-09-21, for the proof of concept. [BRAINSTORM.md](BRAINSTORM.md) chose the direction, [DESIGN.md](DESIGN.md) the engineering gates, [PRIOR_ART.md](PRIOR_ART.md) what others did. This page is the game design those left open: what the player does, what the numbers are, what each screen shows, and how it sits beside Follower Tactics. What the proof of concept built, and what of it has been verified, is [POC.md](POC.md).

The rules are the player's own, read from the game while it runs ([ENGINE_SKILLS.md](ENGINE_SKILLS.md)). None of it has been played.

**Folded into Follower Tactics on 2026-09-21** (branch `wip-progression`): the code is `src/progression/` (`core/` tested by `tests/progression/`, `game/` in the plugin), its records are in Tactics' co-save block, and a follower's own pages carry it: the skill page on their Skills tab, the experience bar and the attribute controls on their Character tab, *Learn* on a tome's page and *Forget* on a spell's, and the switch in the Progression section of Tactics' Settings page, *Manage follower progression*. Progression's own pages (an Overview, its Settings, a page per companion) were taken out the same day, before they were seen in play; what they held that the followers' pages do not yet -- attributes, setting their own perks and spells aside, teaching and forgetting -- comes back there as each is taken up. The stand-alone repository (`C:\project\SkyrimFollowerProgression`) keeps the history before that.

## In one paragraph

A companion levels as you do. Using a skill raises it: a spell cast, a blow landed, a hit taken on their armour or their shield. Skill-ups make levels, by your own levelling's rules, read from the game so that a mod that changes them for you changes them for companions. Each level brings what your own level-up brings, an **attribute point** (+10 health, magicka or stamina) and a **perk point**, and you decide where they go. A skill can be taken back into a pool and spent on others, or reset to where a new character starts it. There is no class and no plan, as there is none for the player. A spell tome they carry can be **learned**, and any spell forgotten. Nothing a follower was recruited with is taken away, and every change is one you can see, trace to its cause, and undo. Follower Tactics decides *when* a companion does something; Progression decides *what they are able to do*.

```
  a skill used ──► skill XP ──► skill-up ──► character XP ──► level ──┬─► 1 attribute point ─► +10 health / magicka / stamina
  spells, blows,     (your rules, read from the game)                  └─► 1 perk point ─────► perks you choose
  hits taken                       spell tomes ──────────────────────────────────────────────► spells you teach
```

## Pillars

1. **The player's rules.** A companion learns what they do, at the rate you would, and levels on your curve. Nothing comes from anywhere you don't: no experience for places or kills that a player wouldn't get.
2. **Yours to assign, like your own.** Vanilla Skyrim has no class for the player, and followers don't need one. Attribute and perk points go where you put them; a skill's levels can be moved to another. What makes it progression rather than an editor is that everything has to be learned first.
3. **Their own person.** A companion keeps their skills, spells and perks. Everything here is added on top; taking something back only ever takes back what this mod added, or what you chose to move.
4. **Nothing unexplained.** Every figure says where it came from: how far a skill is from its next level, why a perk is locked and what would unlock it, why a level can't be taken back.
5. **Beside Tactics, not inside it.** Progression changes capabilities. Using them — which spell, when, on whom — is Follower Tactics' job. The two read the same and link where they meet, and neither needs the other.

## What Companions' Path teaches

[Companions' Path](https://github.com/JVeluz/companions-path) is the closest prior art and the one to learn most from. Its source was read in full for this design (commit `4a5475b`; engine findings in [PRIOR_ART.md](PRIOR_ART.md)). Its interface, in the same SKSE Menu Framework panel we use, is three pages: **Stats** (a level, attribute and skill point pools, `<< - + >>` buttons on every value), **Perks** (a skill combo box, the perks of that tree grouped by required level, a button per perk), and **Settings** (multipliers and "harmonize" switches). An Overview tab per page lays followers side by side.

| What it does | What it teaches | What we do |
|---|---|---|
| Points come from the follower's level, or the player's with *Sync Level* on | A budget that fills by itself is an editor, not progression | The engine's level stands; what a companion learns by doing adds levels on top, up to 5 past yours ([Levels](#levels)) |
| Five skill points a level, one attribute point, one perk point, each placed with `- +` and taken back freely | Direct assignment works: it is the player's own level-up | Kept for attributes and perks. Skills rise by use instead, as yours do, and can be moved or reset ([Points and reassigning](#points-and-reassigning)) |
| *Harmonize* rewrites base stats to profile values; *Reset all perks (incl. native)* removes perks the follower came with | Players lose what made a follower themselves, with one click and no preview | Their own skills, attributes and perks stay unless you move them, one at a time, and every move can be undone ([Reconsidering](#reconsidering)) |
| A perk button is greyed with no reason given; the requirement is the largest number in its conditions | "Why can't I take this?" is the question a tree most needs to answer | Every locked perk says what it needs and what the companion has ([Perks](#perks)) |
| Every perk in every tree is offered, crafting and lockpicking included | A perk that does nothing for them is a wasted point | Kept: every skill and every tree is offered alike, since a mod may have followers smith or pick locks ([Every perk alike](#every-perk-alike)) |
| The Overview shows everyone at once, and a reset per tree | A party view is the right starting place | Per-perk unlearning, refused while a perk bought on it needs it, and a reset per tree; the party is Tactics' list of followers |
| Perk tooltips carry the game's own descriptions | Good; players know these words | Kept, with a second line saying what the perk does on a companion |

## Learning by doing

A companion's skill rises when they use it, by exactly the rules that raise yours. The engine already works out a use for anyone who casts; for everyone but you it throws the result away. Blows and hits it only works out for you. [ENGINE_SKILLS.md](ENGINE_SKILLS.md) has both, and how each is caught.

| They | Trains | Worth, as for you |
|---|---|---|
| Cast a spell that lands, heals, wards, summons or conjures | Its school | The spell's cost × its effect's skill multiplier; a concentration spell for each moment it's held |
| Land a blow | The weapon's skill | The weapon's base damage; a bash by the game's bash formula |
| Block a blow | Block | What the block took, by the game's block settings |
| Take a blow | Heavy or Light Armor, weighted by what they wear | The physical damage |

Sneak, Lockpicking, Pickpocket, Speech, Smithing, Alchemy and Enchanting are trained only for the player in the engine's own code, so a companion's uses of them are not heard; their levels move through the reassigning pool, as any skill's can. Scrolls, staffs, enchanted weapons, powers and shouts train nothing, for you or anyone.

The rate is yours: a skill's next level takes *improve × level ^ `fSkillUseCurve` + offset* skill XP, from each skill's own record. The engine still levels a follower with you as vanilla does, and their class still raises their own skills with it. What they learn by doing is added on top, in the actor's permanent modifier.

## Levels

Each skill-up gives character XP equal to the new level (× `fXPPerSkillRank`), and a level takes `fXPLevelUpBase` + `fXPLevelUpMult` × level (75 + 25 × level in Skyrim.esm), as yours does. A companion's level is the greater of two:

- **the engine's own**: your level × their record's multiplier, between its minimum and maximum. Most followers are ×1.0 with a cap: Onmund at 30, Jenassa at 40, Serana at 50 ([research/follower-levels.md](research/follower-levels.md));
- **what they've learned on top of it**: their character XP added to the XP of the engine's level, at most 5 levels past yours.

So Onmund, held at 30, keeps levelling by what they learn once you're past 30; Serana, who is always your level, gets up to 5 more; Erandur, whom the engine puts at ×1.5, is never pulled down. A follower who always levels with you gains little from learning — the price of a design that gives levels away.

Each level brings:

| | |
|---|---|
| **1 attribute point** | `iAVDhmsLevelUp` (10) to health, magicka or stamina, your choice |
| **1 perk point** | Spent by you, on the Perks tab ([Perks](#perks)) |

## Points and reassigning

**Points are the difference.** At level N a companion has what a player at N would have had — N−1 perk points and N−1 attribute points — less what they hold already: the perks of the skill trees they hold, their own or bought, and what their own health, magicka and stamina carry above their race's starting values. A level the engine gives them is a level-up too, with its points.

**Moving a skill.** The Skills tab lists the twelve skills a companion uses, each with what the engine gives them, what they've learned, the total, how far the next level is, and `-` `+`:

- `-` takes a level back into a pool, worth what that level gave (its level × `fXPPerSkillRank`); down to where a new character of their race starts the skill, `iAVDSkillStart` plus the race's bonus, even below their own value;
- `+` buys the next level from the pool at the same rate; it teaches nothing, so it gives no character XP;
- **Reset** takes a skill to its floor at once, the levels into the pool and the perks bought in its tree returned, as making a skill Legendary does for you; free;
- a level a bought perk needs can't be taken back with `-` — unlearn the perk, or reset the skill.

Attributes are assigned with `-` `+` as well, a point at a time.

## Perks

A perk point is spent on the Perks tab. A companion can learn a perk rank when:

1. they have an unspent point;
2. their skill in the tree — their own plus what they've learned — meets that rank's requirement, read from the rank's own conditions (`GetBaseActorValue OneHanded >= 40`);
3. they hold a perk the node is connected from (any one parent, as the player's trees work), learned here or innate.

A perk that does nothing for a companion ([next section](#every-perk-alike)) is learned like any other, as the player's is: it costs a point and opens what needs it.

The Perks tab says which of these is missing, in words: each row's *Needs* lists the next rank's requirements, the met ones dimmed and the unmet ones in full with what the companion has — *One-Handed 40 (has 34)*, *Fighting Stance*. Having no points is said once, above the trees.

**Learning asks once.** Clicking *Learn* turns the row into *Learn Armsman (rank 3)? Confirm · Cancel*. There is no global "apply" step to forget.

**Innate perks** — those on the follower's own record, like Marcurio's six — are chosen for them in advance: held, counted against their perk points as the player's own choices are, and theirs to give back like any perk bought here. A right click on the top rank gives it back and returns its point, unless a perk they hold needs it; the record keeps it, and a click takes it up again for a point, whatever it asks, since it was theirs. *Reset perks* gives back the tree's own perks with the bought. So a follower can be rebuilt whole, perk by perk. This is possible because the engine is told what a companion holds rather than having their record edited ([ENGINE_PERKS.md](ENGINE_PERKS.md)). *(Until 2026-09-22 an innate perk could only be set aside, for nothing, and restored for nothing.)*

**Ranks stack, as the player's do.** Each rank of a perk is its own record, and a companion holds every rank they have learned: Armsman's first rank switches itself off once the second is present (its entry's condition is `HasPerk Armsman20 == 0`, read from Skyrim.esm). So learning adds the next rank's record and unlearning removes the top one; nothing is replaced.

### Every perk alike

Every perk in every tree is offered and learned as the player's is, whatever it does. Whether it does anything for a follower is the engine's answer, not ours: an effect through an entry point the engine evaluates for any actor (damage, armour, spell cost and magnitude, critical hits, blocking), an ability, or a spell's own conditions reading it (Impact, Deep Freeze) reach a follower; what only the player's own systems read (zoom, slow time, crafting, barter, lockpicks) does not, unless a mod makes it. Eagle Eye does nothing for a companion but stands between Overdraw and Power Shot, and is bought to reach it, as the player's is.

*(Until 2026-09-22 each perk carried a verdict -- works, situational, unverified, no effect -- from a catalog of vanilla perks and a reading of entry points, for the hovers. The hovers stopped showing it on 2026-09-21, and with nothing reading it the catalog went. Until 2026-09-21 a no-effect perk was a bridge, counted as held for nothing; in play it read as a perk that could not be clicked, so it went too.)*

**With Follower Tactics**, two perk families gain a second meaning: Tactics' settings can require the school's Dual Casting perk before a follower dual-casts, and the Power Bash perk before they power-bash. The perk rows say so when Tactics is installed.

## Spells

**Learning from a tome** is the player's reading of one, done by the companion: give them the tome, open it on their Inventory tab, and *Learn* at the top right asks once and teaches it. The tome is used, and the page goes back to their books. Nothing is asked but that they do not know the spell already (*Already knows Flames*): the player can read any tome, so a companion can too, whatever their skill or magicka; whether and when they cast it is Follower Tactics' question. Tomes are named in the enchanted items' blue on the list. Only books that teach a castable spell, through the book's own *teaches* link, have *Learn*, so modded tomes work without a list. *(Changed 2026-09-21: the first design taught from the player's pack and asked for the school's skill at the spell's level and the magicka to cast it.)*

**Forgetting.** A spell's page on their Magic tab has *Forget* at the top right, asked once, for any spell they know. One taught here is taken back; any other -- their record's, their race's, a quest's -- is set aside, the record keeping it. Either way the engine is told they do not know it, so it leaves every list that asks the engine: their Magic tab, the combat AI's choice, their hands, Tactics' rules (which read it as not known) and its pins and bans on it (dropped, as for an item no longer carried). The tome is not returned -- forgetting is not a way to copy books -- and a tome of the spell brings it back: taught again, or theirs taken up again. Both work because the engine is told what a companion knows, rather than having their record edited or the spell added to them ([ENGINE_SPELLS.md](ENGINE_SPELLS.md)), so a save without the mod has them as their record has them. Only castable spells; powers, shouts and abilities stay as they are.

Follower Tactics' *ban* is not this. A banned spell is still known — to conditions, scripts, other mods and Tactics' own rules; the combat AI just never picks it. Forgetting means they no longer know it at all.

**Using it** is Tactics' business. A vanilla companion's AI may or may not choose a new spell by itself; with Follower Tactics installed, the Spells tab says so under its tables: *Follower Tactics decides when a spell is cast: give it a rule on Marcurio's Tactics tab.*

## Reconsidering

Everything here can be taken back, for nothing:

| Action | What changes |
|---|---|
| **Take a skill's level back** (`-`) | Into the pool, to buy another skill's with. Refused while a perk bought here needs the level |
| **Reset a skill** | To where a new character starts it; every level into the pool, the perks bought in its tree returned |
| **Take back an attribute point** | The point returns; with none assigned, a point of their own value, down to their race's starting value |
| **Give back a perk** | The top rank held, bought here or their own, while nothing held needs it; the point comes back, and one of their own costs a point to take up again |
| **Forget a spell** | One taught here is taken back; one of their own set aside, and a tome of it brings it back |

Their level and character XP are never touched: moving levels between skills can't make more of them, as a Legendary skill doesn't lower yours.

## Pace

The pace is yours: a companion who fights as much as you do learns about as fast as you do, by the same curve. Two differences, neither measured yet. A follower fights nearly all the time and takes most of the blows, so their weapon, armour and Block skills may climb faster than a player's would. And they learn nothing from the skills they can't use. The first sessions in play set whether that needs a setting.

## Interface

### Where it lives

| Surface | Used for | In the proof of concept |
|---|---|---|
| **The panel** (SKSE Menu Framework, F1) | The companion's sheet: skills, perks, spells; the party overview; settings | Built, then taken out on 2026-09-21 for Follower Tactics' own pages (below) |
| **Notifications** | "Lydia reached level 31: an attribute point and a perk point to assign." | Yes |
| **Dialogue** | "Let's talk about your training" opens the companion's sheet as a window; "I have a spell for you" opens the Spells tab | Designed below; needs an ESP |
| **Follower Tactics' panel** | The skill page on its Skills tab, opened by a skill's name (its level, a caret before the number, opens the perks held beneath the row; the table is headed *Skill*, *Level*): the perk tree as the menu draws it (a circle a perk, filled by the ranks held, labelled at whichever of eight places about its circle keeps it clearest of the others; across in the tree's columns as the menu orders them, evenly spaced and stretched to the page's width, and up the page by the level its first rank asks, a perk a hair off the column beside it drawn straight above in it, so nothing moves as ranks are taken and the page never scrolls; the hover gives the name and, at its right, what it needs (and, short of it, what they have, both greyed, the labels aligned), the description, and on one line *Click to acquire perk* at the left and *Right click to remove perk* at the right where each can act). In the header `<<` `-` level `+` `>>` (a level, or as far as it goes), the perks to spend when there are any, and *Reset perks*, which asks once and returns the tree's bought perks, the skill left as it is. A click on a circle acquires its next rank, a right click returns the top one unless a perk bought on it needs it, with the game's own sounds (the perk menu's for a perk taken, the skills menu's step back for one returned, a failed activation's where nothing can be done, and no message either way); a click on a name opens the perk's page, held or not. The page is rebuilt behind each of these, so it shows at once. The buttons and their reasons are core's (`ButtonsFor`); the player's page has the tree alone. A link from a learned spell to a rule | The skill page built 2026-09-21, not yet seen in play; the spell link designed below |

The panel matches Tactics' look and behaviour so the two read as one product: the same tables, headings, dimmed-with-a-reason rows, hover notes, and one entry per companion. A clickable version of the screens, drawn with only what Dear ImGui can draw, was built in the stand-alone repository (`prototype/` there); it predates learning by doing, still shows the first design's focus-driven Training tab, and was not brought into Tactics. Building it changed the design in the places marked *(from the prototype)*.

```
▼ Follower Tactics
    Settings              the tactics and levelling switches
    Player
    ▼ Followers
        Lydia             Character (level and experience), Skills (a skill's page: its perk tree), Magic, ...
        Marcurio
```

### A companion's page

The design below was Progression's own page, and was built so; since 2026-09-21 a follower's pages in Tactics carry it instead. It stays as the list of what those pages are to take up.

A header that is always there, then three tabs.

```
 Lydia                                                        Level 31
 ████████████████████████░░░░░░░░░░░░  340 / 825 experience
 An attribute point and a perk point to assign                     With you
 ┌────────┬───────┬────────┐
 │ Skills │ Perks │ Spells │
```

The bar is the character XP toward their next level; hovering it says how much of the level is the engine's and how much they've learned. The points line is the page's call to action; *at the limit, 5 levels past yours* joins it when learning is held back. *Away* or *Waiting* replaces *With you* when the companion is not in the party. Their page stays readable while they are away, but every button that would change the actor — `-` `+`, Learn, Teach, Forget, Unlearn — is greyed with *Lydia must be with you for this* *(from the prototype: an away page that let you spend points invited changes the engine could not yet make)*.

**Skills.**

```
 They learn by doing, as you do: a spell cast, a blow landed, a hit taken on their armour or their shield raises
 the skill used, by your own rules. Their level rises with what they learn.
 1,230 XP to reassign: + buys a level at what it is worth.

 Skill            Their own   Learned   Total   Next level
 One-Handed          42          +14       56    [██████░░░ 312 / 490]   [-] [+]   Reset
 Two-Handed          20           —        20    [░░░░░░░░░   0 / 87]    [-] [+]   Reset
 Block               30           +9       39    [███░░░░░░ 121 / 312]   [-] [+]   Reset
 …

 1 attribute point to assign, 10 each
 Attribute        Their own   Assigned   Total
 Health             190          +30      220     [-] [+]
 Magicka             50           —        50     [-] [+]
 Stamina            110          +10      120     [-] [+]
```

Rows keep their places: sorting them by total would move a row out from under the pointer between two clicks. *Their own* is what the engine gives the follower today; it still rises with their level, and hovering it says so. Each button says on hover what it costs or returns — *Into the pool: 56 XP*, *57 XP from the pool; 1,230 in it* — or why it's greyed: at the floor, at 100, or the perk that needs the level. *Reset* asks once, and says what it returns.

**Perks.**

```
 1 perk point to spend                        [ ] Learnable now only
 ▼ One-Handed 56 · 1 of 10 held, 4 ready
   Perk              Learned   Needs
   Armsman            2/5      One-Handed 40                 [ Learn rank 3 ]
   Fighting Stance     —       Armsman · One-Handed 20       [ Learn ]
   Bladesman           —       Armsman · One-Handed 30       [ Learn ]
   Savage Strike       —       Fighting Stance · One-Handed 50
   Critical Charge     —       Fighting Stance · One-Handed 50        situational
   Paralyzing Strike   —       One-Handed 100 (has 56) · Critical Charge or Savage Strike
 ▶ Block 39 · 0 of 10 held, 1 ready
 ▶ Heavy Armor 44 · 0 of 8 held, 1 ready
 ▶ Destruction 15 · 0 of 16 held
```

Met requirements are dimmed and unmet ones are in full ink, with what the companion has; the two parents of an OR group read *Critical Charge or Savage Strike*. Trees with anything held first, then the rest, each collapsible, each headed by the skill's total and a count: *One-Handed 54 · 2 of 10 held, 7 ready* — where a point can go without opening every tree *(from the prototype)*. A filter shows only what can be learned now. Hovering a perk gives the game's description, then a line for companions: *Works in combat — expected; not yet measured on an NPC*. A learned perk bought here has an *Unlearn* button, always visible rather than on hover, so a gamepad can reach it *(from the prototype)*; one they came with says *their own*. When there are no points to spend it is said once above the trees, not on every row.

**Spells.** On Tactics' own tabs since 2026-09-21 ([Spells](#spells)): *Learn* on a tome's page in their Inventory, *Forget* on a spell's page in their Magic tab.

### The Overview

```
 Companion   Level  Experience              To assign
 Lydia        31    ██████████░░  340/825   an attribute point and a perk point   With you
 Marcurio     30    ███░░░░░░░░░   90/800                                         With you
 Jenassa      28    ███████░░░░░  130/775   210 XP to reassign                    Away
 Aela          —    not enrolled                                   [ Enroll ]     With you
```

Everyone the save knows, present or not. *To assign* is the reason to open a page. The *not enrolled* row appears only for a follower who is not enrolled automatically: with the setting off, or one this build does not train (not unique) *(from the prototype: with the default settings the row as first drawn could never appear)*.

### Notifications

On a level: *Lydia reached level 31: an attribute point and a perk point to assign.* Several levels at once — a big test gift — are said once, with the level reached and everything waiting, not a line per level *(from the prototype)*. Optional, off by default: each skill increase, as *Lydia's One-Handed increased to 57*, the way yours are shown.

### Dialogue (after the proof of concept)

The brainstorm's preferred entry point, kept, but narrowed to what dialogue is good at. Our own quest adds topics to enrolled followers without touching `DialogueFollower`:

> *Let's talk about your training.* → the companion's page opens as a window, on the Skills tab; closing it returns to the conversation.
> *I have a spell tome for you.* → the page opens on Spells. *Teach* there plays a short acknowledgement ("Let me see… yes, I can learn this.") before the conversation resumes.

A window beside the conversation keeps the social framing and avoids the problems the brainstorm found with fully dynamic dialogue (arbitrary spell names as topics, paging, localisation). SKSE Menu Framework can open a standalone window from native code, which a Papyrus fragment can call.

## Complementing Follower Tactics

| | Follower Tactics | Follower Progression |
|---|---|---|
| Answers | *What will they do, and when?* | *What are they able to do?* |
| Owns | Rules, spell choice, equipment pins, combat style | What they've learned and where it sits, points assigned, perks bought here, spells taught here |
| Reads | The follower's skills, perks, spells | The party, their skill use, the tomes they carry |
| Changes | Nothing permanent about the follower | Skills, attributes, perks and spells, recorded |

Where they meet:

1. **A taught or forgotten spell** is known, or not, through the spell view, not the actor's own lists, so Tactics reads spells through the engine's `VisitSpells` (its `ForEachSpell`, since 2026-09-21; seen working in play: Jenassa's taught Sparks), and asks the engine's `HasSpell` whether a pin or ban on a spell still holds (`StillCarried`). A spell learned can be given a rule at once; one forgotten reads as not known to every rule naming it.
2. **Dual Casting and Power Bash perks** are what Tactics' *Require …* settings check. Progression shows that on the perk rows.
3. **What they've learned, in Tactics' breakdowns.** Tactics' Skills and Character hovers attribute every modifier they can and call the rest *Other*; learned levels and assigned points land in the permanent modifier, so today they would read as *Other*. A two-function interface — "how much of this actor value is Progression's?" — through SKSE's messaging lets Tactics name it *Progression +14*.
4. **One look.** The same panel conventions, the same one-entry-per-follower layout, the same words (a follower is *they*).

Neither mod requires the other. Without Tactics, a companion uses what they learned through vanilla AI.

### The interface, concretely

SKSE's messaging interface carries it: a plugin dispatches a typed message with a data pointer to a named receiver, and the receiver's listener runs synchronously on the dispatching thread, so a query can be answered in place. Both sides only ever act on the game thread, and either side's absence is simply no listener.

```cpp
// Shared header, copied into both repositories; versioned, never reordered.
namespace fp::api
{
inline constexpr std::uint32_t kVersion = 1;

// Progression -> "FollowerTactics", after any committed change to a
// companion's assigned points, perks or spells. Tactics re-reads that actor's
// sheet (its spell list, perks and skills) on its next tick.
inline constexpr std::uint32_t kGrowthChanged = 'FPGC';
struct GrowthChanged
{
    std::uint32_t version;
    RE::FormID actor;
    std::uint32_t what; // bit 0 points, bit 1 perks, bit 2 spells
};

// "FollowerTactics" -> Progression: how much of an actor value is
// Progression's own, for Tactics' breakdown hovers ("Progression +14").
// Progression's listener fills `amount` and sets `answered`.
inline constexpr std::uint32_t kTrainingQuery = 'FPTQ';
struct TrainingQuery
{
    std::uint32_t version;
    RE::FormID actor;
    std::int32_t actorValue;
    float amount;   // out
    bool answered;  // out
};
} // namespace fp::api
```

That is all it needs: one notification, one question, and Tactics reading spells through `VisitSpells`. Then a taught spell appears without the notification; the notification only makes it prompt. Nothing here lets either mod change the other's state.

## Engine approach for the proof of concept

[DESIGN.md](DESIGN.md) lists the P0 risks; this is what the proof of concept does about each, and none of it is verified in play yet.

| Change | How | Why this way | Unverified |
|---|---|---|---|
| Learned skills and assigned attributes | A view in front of the engine: Character's `GetBaseActorValue` is replaced, so for a managed companion a skill reads with the learned levels on top (held to the cap) and health, magicka and stamina with the points' worth; the permanent and current values read the base through it ([ENGINE_SKILLS.md](ENGINE_SKILLS.md), "Read, not written"). Until 2026-09-22 it was `ModActorValue(kPermanent)`, which the save kept | Nothing written to the actor or the save: without Progression, or with it off, the follower is as their record makes them at once, and the engine's own levelling moves only the base under what they learned | That the hook runs; that no reader bypasses the interface; that the engine's recalculation of an NPC's values does not write a read value back |
| Perks | A view in front of the engine: Character's `ForEachPerk` and `ApplyPerksFromBase` virtuals are replaced, so for a managed companion the engine's `HasPerk` and its effect registration see the record's perks less the set-aside plus the bought ([ENGINE_PERKS.md](ENGINE_PERKS.md)) | The record, shared by every copy of the NPC, is never edited: nothing leaks between saves, and a follower's own perks can be set aside and restored. An ability perk's spell is saved on the actor; turning progression off takes it off with everything else | That the hooks run, their timing at load, that releasing leaves a save clean, and the measured effect |
| Spells | A view in front of the engine: `Actor::VisitSpells` detoured and Character's `CheckCast` replaced, so for a managed companion `HasSpell`, the combat AI's inventory and the UseMagic procedure see what they know less the set-aside plus the taught ([ENGINE_SPELLS.md](ENGINE_SPELLS.md)); one tome removed from the companion | Nothing written to the record or the actor, so a follower's own spells can be set aside, and taught ones go with the mod | That the hooks run, and that a spell known only through the view is cast |
| Skill use | Character's `UseSkill` slot replaced (magic), and the hit handler's call to the victim's processing hooked, the player-only weapon, Block and armour uses worked out the same way for companions ([ENGINE_SKILLS.md](ENGINE_SKILLS.md)) | The engine already works out a caster's use and throws it away; the combat uses it works out only for the player, so they are mirrored with the game's own settings | That both hooks are heard in play, and the rate |
| Level | The engine's own is untouched; their level for points is the greater of it and what they've learned on top, at most 5 past the player's | Owning engine level is the unsolved P0; supplementing it is robust and honest | — |

State is one record per companion in the SKSE co-save, keyed by plugin + local id: what they've learned (levels, progress, character XP, the reassigning pool), attribute points, perks bought, spells taught. Nothing of it is applied to the actor, so there is nothing to reconcile but what the engine keeps of perks and spells (ranks registered, spells in hand).

## Not in the first build

Sneak and the crafting skills by use, skill books and trainers for companions, *Mod Skill Use* perks for companions, dialogue, setting aside abilities, owning engine level, overhaul perk trees beyond the entry-point heuristic, non-unique followers, creatures, restoring a companion to pre-enrolment state, and the Tactics interface. Each is designed above or in [DESIGN.md](DESIGN.md) and waits on the proof of concept's in-game results.

## Decisions for you

1. **Supplement vanilla levelling** (this build) or push on owning engine level? Supplementing ships sooner and is safer; owning is purer.
2. ~~Bridges for no-effect perks~~: decided 2026-09-21, bought like any other perk (above).
3. **How far past the player** learning may take a companion: 5 levels now.
4. ~~Attribute points from their own values~~: decided 2026-09-22. What their class put into their health, magicka and stamina counts as points spent, and can be taken back, down to their race's starting values, a point returned for each 10 to spend elsewhere, as a skill can be taken below their own value to its floor. So a follower their class carried past the player's count (Serana at 50) has no points from their level, and can move what they have.
5. ~~Spells outside a companion's calling~~: decided 2026-09-21, a tome teaches anyone who does not know the spell, as it does the player (above).
