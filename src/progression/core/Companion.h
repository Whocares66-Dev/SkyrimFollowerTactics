#pragma once
// One companion's record: everything Progression knows and owns about them,
// and every operation on it (dev/PROGRESSION.md). The record is the ledger
// DESIGN.md asks for -- what they have learned by doing and where it sits,
// the points assigned, perks bought, spells taught. None of it is written to
// the actor: the engine is shown it as it reads their values, perks and
// spells (progression/game/ValueView.h, PerkView.h, SpellView.h), so the
// save holds nothing of it and the game without Progression has the
// follower as they were.
//
// The operations decide; the game side carries out what they decide on the
// actor and reports back (progression/game/Service.cpp). No Skyrim.

#include "progression/core/Ids.h"
#include "progression/core/Levelling.h"
#include "progression/core/Perks.h"
#include "progression/core/Skills.h"
#include "progression/core/Spells.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace fp
{

struct LearnedPerk
{
    FormKey form;
    std::string name; // "Armsman", kept for a form gone missing
    int rank{1};      // which rank of its node, from 1
    bool operator==(const LearnedPerk &) const = default;
};

struct TaughtSpell
{
    FormKey spell;
    std::string name;
    bool operator==(const TaughtSpell &) const = default;
};

// One of their own spells, set aside: the engine is told they do not know
// it (progression/game/SpellView.h). The name is kept for a form gone
// missing.
struct SpellAside
{
    FormKey spell;
    std::string name;
    bool operator==(const SpellAside &) const = default;
};

// What they have learned by doing, and where it sits (progression/core/Levelling.h).
struct Learning
{
    // Levels of each skill on top of what the engine gives them: learned by
    // use, bought from the pool, or taken back below it (negative).
    PerSkill<int> skills{};
    PerSkill<double> progress{}; // skill XP toward each skill's next level
    // Attribute points assigned, and what they add in the engine's units:
    // each point iAVDhmsLevelUp as it was when assigned, as the player's own
    // level-ups keep theirs.
    PerAttribute<int> attributePoints{};
    PerAttribute<int> attributes{};
    double xp{0.0};   // character XP from skill-ups
    double pool{0.0}; // character XP from levels taken back, to buy others with
    bool operator==(const Learning &) const = default;
};

struct Companion
{
    FormKey key; // the placed reference
    std::string name;
    Learning learning;
    int level{0}; // the last level noted: a rise is a level-up
    std::vector<LearnedPerk> perks;
    // Perks on their own record that the player has set aside: every rank of
    // the node they hold. The record keeps them; the engine is told they are
    // not held (progression/game/PerkView.h), and restoring them costs nothing.
    std::vector<FormKey> setAside;
    // Spells taught here: the engine is told they know them (progression/game/SpellView.h).
    std::vector<TaughtSpell> spells;
    std::vector<SpellAside> spellsSetAside;
};

[[nodiscard]] Companion Enroll(FormKey key, std::string name);

// --- the level ---------------------------------------------------------------------

// Their level: the engine's own, or higher by what they have learned on top
// of it -- their character XP stacked on the XP of the engine's level, on the
// player's curve -- but no more than Rules::levelsAbovePlayer above the
// player, and never below the engine's.
struct LevelProgress
{
    int level{1};
    int engine{1};      // what the engine gives them
    bool capped{false}; // learning would take them past the player's level + the allowance
    double into{0.0};   // character XP into the level learning has reached
    double toNext{0.0}; // and what that level costs in all
};
[[nodiscard]] LevelProgress Progress(const Companion &c, int engineLevel, int playerLevel, const Rules &r) noexcept;
[[nodiscard]] int Level(const Companion &c, int engineLevel, int playerLevel, const Rules &r) noexcept;

// --- learning by doing ------------------------------------------------------------

struct Practice
{
    int skillUps{0};
    int reached{0}; // the skill's level after
};

// One use of a skill worth `points`, as the engine reports it: skill XP,
// skill-ups, character XP (41561). `base` is the skill as the engine has it,
// `usage` its record's values. Nothing at the cap, or for nothing worth.
Practice Practise(Companion &c, Skill skill, double points, int base, const SkillUsage &usage, const Rules &r) noexcept;

// Each skill as requirements read it: `base`, the engine's, plus theirs.
[[nodiscard]] PerSkill<int> Effective(const Companion &c, const PerSkill<int> &base) noexcept;

// --- points ------------------------------------------------------------------------
// What a player at `level` would have had: one perk point and one attribute
// point for each level after the first. What they have already is spent.

// Perk ranks of the skill trees they hold: their own (less the set-aside)
// and bought here.
[[nodiscard]] int HeldRanks(const PerkGraph &graph, const Holdings &holdings) noexcept;
[[nodiscard]] int PerkPoints(int level, int heldRanks) noexcept;

// What their own health, magicka and stamina already carry above their
// race's starting values, in attribute points (whole ones, never below 0).
[[nodiscard]] int OwnAttributePoints(const PerAttribute<int> &base, const PerAttribute<int> &raceStart,
                                     const Rules &r) noexcept;
// The attribute points to assign: what a player at `level` would have had,
// less what their own values carry (`ownPoints`), never below 0; less the
// points assigned here, a point taken back below their own values counting
// as one returned. So a follower whose class carried them past the player's
// count has none from their level, and has what they take back.
[[nodiscard]] int AttributePoints(const Companion &c, int level, int ownPoints) noexcept;

// Those two, and the reassigning pool, in words: "an attribute point and a
// perk point"; empty when there is nothing to assign.
[[nodiscard]] std::string ToAssign(int perkPoints, int attributePoints, double pool);

// --- reassigning --------------------------------------------------------------------

enum class AssignBlock : std::uint8_t
{
    None,
    NoPoints,    // not enough in the pool, or no attribute points left
    AtCap,       // the skill is at the cap
    AtFloor,     // the skill is at its starting value
    PerkNeedsIt, // a perk bought here would no longer meet its requirement
};

struct AssignCheck
{
    AssignBlock block{AssignBlock::None};
    std::string perk; // for PerkNeedsIt: the perk that needs the level
};

// A skill's floor: what a new character starts it at, iAVDSkillStart plus
// their race's bonus to it.
[[nodiscard]] int SkillFloor(const Rules &r, int raceBonus) noexcept;

// One level onto a skill from the pool (+1), or back into it (-1).
// Buying costs what the level is worth (XpForSkillLevel); taking it back
// returns the same. `base` is the skill as the engine has it.
[[nodiscard]] AssignCheck CheckSkill(const Companion &c, Skill skill, int delta, const PerSkill<int> &base, int floor,
                                     const PerkGraph &graph, const Holdings &holdings, const Rules &r);
void AssignSkill(Companion &c, Skill skill, int delta, int base, const Rules &r) noexcept;
// A skill moved as far as it goes one way (`direction` -1 down, +1 up): a
// level at a time, while CheckSkill allows the next. How many it moved.
int AssignSkillAll(Companion &c, Skill skill, int direction, const PerSkill<int> &base, int floor,
                   const PerkGraph &graph, const Holdings &holdings, const Rules &r);

// One attribute point onto an attribute (+1) or off it (-1), as a skill's
// level is: off takes back a point assigned here, or, with none, a point of
// their own value, down to `floor`, their race's starting value, and returns
// it to spend elsewhere. `base` is the value as the engine gives them, `step`
// what a point is worth (iAVDhmsLevelUp now).
[[nodiscard]] AssignBlock CheckAttribute(const Companion &c, Attribute attribute, int delta, int available, int base,
                                         int floor, int step) noexcept;
// A point moving away from none adds or takes `step`; one moving back
// toward none gives back what it was worth when it moved, as the player's
// level-ups keep theirs: a setting changed later moves only the points
// after it.
void AssignAttribute(Companion &c, Attribute attribute, int delta, int step) noexcept;
// An attribute moved as far as it goes one way (`direction` -1 down, +1
// up): a point at a time, while CheckAttribute allows the next, `available`
// the points to assign before the first. How many it moved.
int AssignAttributeAll(Companion &c, Attribute attribute, int direction, int available, int base, int floor,
                       int step) noexcept;

// An attribute's buttons as the character sheet offers them, as a skill's
// are (ButtonsFor): - and + a point, << and >> as far as it goes.
struct AttributeButtons
{
    bool canLower{false};
    bool canRaise{false};
    std::string lower;   // -
    std::string lowest;  // <<
    std::string raise;   // +
    std::string highest; // >>
};
[[nodiscard]] AttributeButtons AttributeButtonsFor(const Companion &c, Attribute attribute, int available, int base,
                                                   int floor, int step);

// The perks bought in a skill's tree unlearned, their points free again; the
// skill left where it is. Their names, a rank after the first as
// "Armsman (2)".
std::vector<std::string> ResetPerks(Companion &c, Skill skill, const PerkGraph &graph);
[[nodiscard]] bool BoughtInTree(const Companion &c, Skill skill, const PerkGraph &graph);

// A skill's buttons as a page offers them: whether each can act, and what
// a click does or why it cannot, in the panel's words. - and + move a
// level, << and >> as far as it goes (as long as - and + can act); Reset
// perks gives back the perks bought in its tree. Whether the companion is
// here, and whether leveling is on, are the caller's to add.
struct SkillButtons
{
    bool canLower{false};
    bool canRaise{false};
    bool canResetPerks{false};
    std::string lower;   // -
    std::string lowest;  // <<
    std::string raise;   // +
    std::string highest; // >>
    std::string resetPerks;
};
[[nodiscard]] SkillButtons ButtonsFor(const Companion &c, Skill skill, const PerSkill<int> &base, int floor,
                                      const PerkGraph &graph, const Holdings &holdings, const Rules &r);

// --- as the engine reads them (progression/game/ValueView.h) --------------------------

// A skill's base as the engine is to read it: `base`, what the engine gives
// them, with `learned` on top. Never past `cap` -- the learned levels above
// it wait while the engine's own base is that high -- and never below 0. A
// base already past the cap is left as it is.
[[nodiscard]] float WithLearned(float base, int learned, int cap) noexcept;

// --- perks ---------------------------------------------------------------------------

// `onRecord` is the perks on their own record: set-aside ones are dropped
// from it, and anything also bought here reads as theirs.
[[nodiscard]] Holdings HoldingsOf(const Companion &c, std::unordered_set<FormKey, FormKeyHash> onRecord);
[[nodiscard]] bool Bought(const Companion &c, const FormKey &form) noexcept;

// Records a rank bought. The caller has checked Status() said None.
void Learn(Companion &c, const PerkNode &node, int rankIndex);
// Records a rank given back: its point returns. False when it was not
// bought here.
bool Unlearn(Companion &c, const FormKey &form);

// Setting aside one of their own perks: every rank of `node` found in
// `onRecord`. Nothing is refunded -- it was never bought -- and restoring it
// is free. The caller has checked that nothing bought here needs it
// (progression/core/Perks.h, WouldBreak).
void SetAside(Companion &c, const PerkNode &node, const std::unordered_set<FormKey, FormKeyHash> &onRecord);
// False when none of the node's ranks were set aside.
bool Restore(Companion &c, const PerkNode &node);
[[nodiscard]] bool IsSetAside(const Companion &c, const PerkNode &node) noexcept;

// --- spells ----------------------------------------------------------------------------------

[[nodiscard]] bool Taught(const Companion &c, const FormKey &spell) noexcept;
// Nothing when taught already, or when it is one of theirs set aside: that
// is restored, for nothing, rather than taught.
void Teach(Companion &c, const SpellFacts &spell);
// False when the spell was not taught here: those are not ours to take.
bool Forget(Companion &c, const FormKey &spell);

// A spell they know that was not taught here -- their record's, their
// race's, one a quest gave them -- set aside, and taken up again; both free.
// A spell taught here is forgotten instead, so neither applies to one. False
// when nothing changed.
bool SetAsideSpell(Companion &c, const SpellFacts &spell);
bool RestoreSpell(Companion &c, const FormKey &spell);
[[nodiscard]] bool IsSpellSetAside(const Companion &c, const FormKey &spell) noexcept;

// A tome read, as the player reads one: a spell set aside is taken up
// again, any other is taught; nothing when they know it. `known` is the
// engine's answer through the view, so one set aside is not known.
enum class TomeRead : std::uint8_t
{
    Known,
    Taught,
    Restored,
};
TomeRead ReadTome(Companion &c, const SpellFacts &spell, bool known);

// A spell they know, forgotten: one taught here is taken back, any other
// set aside, so the engine is told they do not know it either way, and a
// tome of it brings it back (ReadTome). Nothing when they do not know it.
enum class SpellForgotten : std::uint8_t
{
    NotKnown,
    Forgotten,
    SetAside,
};
SpellForgotten ForgetSpell(Companion &c, const SpellFacts &spell, bool known);

} // namespace fp
