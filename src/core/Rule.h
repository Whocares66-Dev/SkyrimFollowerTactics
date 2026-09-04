#pragma once
// A rule is one row of the tactics grid:
//
//     IF <subject> <predicate> <arg>   THEN <action> ON <target>
//
// The subject/predicate split is deliberate and mirrors Dragon Age: Origins,
// whose editor cascades subject -> predicate -> argument. Baking the subject
// into the predicate name -- the earlier SelfHealthPctBelow / AllyHealthPctBelow
// / PlayerHealthPctBelow -- multiplied every new predicate by every subject and
// left the two UI columns secretly dependent on one another. Split, the menu is
// a genuine cascade and the engine composes: any valid subject may be paired
// with any valid predicate, and IsPredicateValidFor says which pairs are valid.

#include "Kinds.h"
#include "Loadout.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ft
{

// Who the condition asks about.
//
// Ally and Enemy are *group* subjects: the predicate holds if any member
// satisfies it, and the member that best satisfies it becomes the rule's
// binding (see Evaluator.h). That binding is what lets "enemy below 30% health
// -> attack it" be written without naming the enemy twice.
enum class SubjectKind : std::uint8_t
{
    Self,
    Player,
    Ally,
    Enemy,
    CurrentTarget,

    COUNT
};

// What is being asked about the subject. Rule::conditionArg carries the
// threshold where one applies: a 0..1 fraction for the Pct predicates, game
// units for WithinDistance, a plain count for CountAtLeast.
enum class PredicateKind : std::uint8_t
{
    Any,
    HealthPctBelow,
    StaminaPctBelow,
    MagickaPctBelow,
    InBleedout,
    InCombat,
    // The edges of a fight, true on one tick each. CombatBegins holds on the
    // first evaluation of a fight and the list runs on as usual beneath it.
    // CombatEnds holds on one farewell evaluation after the follower leaves
    // combat -- and on THAT evaluation nothing else holds: a standing "Any"
    // rule must not re-pin the bow the moment the after-fight restore has
    // put the travelling gear back.
    CombatBegins,
    CombatEnds,
    WithinDistance,
    CountAtLeast,
    // The subject is in the status Rule::statusKind names: poisoned,
    // burning, fleeing ... Any subject.
    Status,
    // The other side of the three Pct predicates. Listed after the rest so
    // the editor's menu, which walks this enum, keeps them beneath their
    // below-counterparts; AboveOf pairs the two.
    HealthPctAbove,
    StaminaPctAbove,
    MagickaPctAbove,

    COUNT
};

// Who the action is applied to. ConditionSubject -- the default -- means
// whoever the condition matched.
enum class ActionTargetKind : std::uint8_t
{
    ConditionSubject,
    Self,
    Player,
    CurrentTarget,

    COUNT
};

enum class ActionKind : std::uint8_t
{
    None,
    DrinkHealthPotion,  // the strongest carried
    DrinkMagickaPotion, // the strongest carried
    DrinkStaminaPotion, // the strongest carried
    DrinkPotion,        // one specific potion, named by actionForm
    CastSpell,
    // The equip actions PIN: what they put on stays on, against the engine's
    // own swap and the combat AI's choice, until another rule or the panel
    // lets it go. A plain equip would not do -- the AI re-derives what to
    // hold on its own schedule, so a sword put in her hand without a pin
    // lasts until its next decision, which may be the same second. Each
    // names a thing by actionForm, and Weapon and Spell a hand as well; a
    // form of 0 is "none": let go of every pin of that kind and take those
    // things off, so the AI decides again.
    EquipWeapon,
    EquipArrows,
    EquipSpell,
    EquipArmor,
    StopCombat,
    Flee,
    HoldPosition,

    COUNT
};

// One thing to do. A rule carries a list of these, in order.
struct Action
{
    ActionKind kind{ActionKind::None};

    // Which spell, for CastSpell; which potion, for DrinkPotion; which
    // thing, for the equip actions. A FormID, and deliberately opaque here:
    // core has no idea what a spell is, it only compares this against the
    // ids the snapshot reports as known, carried, running or pinned.
    //
    // A separate field from arg because arg is a float, and a float cannot
    // hold a 32-bit FormID without loss -- the mantissa is 24 bits, so ids
    // above 0xFFFFFF would silently round to a different form.
    std::uint32_t form{0};

    // Which hand, for EquipWeapon and EquipSpell: Left, Right, or Both for a
    // two-hander, a bow, a master spell -- or an either-hand spell in each
    // hand at once. Ignored by every other action.
    Hand hand{Hand::None};

    // A number the action takes, when it does: the sustain time of a
    // concentration spell, for CastSpell.
    float arg{0.0f};
};

struct Rule
{
    bool enabled{true};

    SubjectKind subject{SubjectKind::Self};
    PredicateKind predicate{PredicateKind::Any};
    float conditionArg{0.0f};
    // Which status, for PredicateKind::Status. Ignored by every other
    // predicate.
    StatusKind statusKind{StatusKind::Poisoned};

    ActionTargetKind actionTarget{ActionTargetKind::ConditionSubject};

    // What to do, in order -- ALL of it. The rule is the unit of the list:
    // the first rule whose condition holds and which can do something wins
    // the tick, and then every action it carries is done in order, each
    // on its own availability. One that cannot be done at all is skipped;
    // one that cannot be done YET -- a cast while the last is still in the
    // air, a potion inside its settle -- is waited for, and nothing else
    // is decided until the list is through. See Evaluator.h. The common
    // rule carries one action, and FirstAction is the short way to it.
    std::vector<Action> actions;

    [[nodiscard]] Action &FirstAction()
    {
        if (actions.empty())
            actions.emplace_back();
        return actions.front();
    }

    // No per-rule cooldown, deliberately. A cooldown is a property of the
    // remedy -- how long a potion takes to show, how long a cast takes to
    // land -- so it belongs to the action (MinimumCooldown) and is tracked per
    // action, not per rule. The per-rule field this replaced was invisible in
    // the editor, survived a change of action (a potion rule turned into a
    // cast rule kept its 10 s), and its state was keyed by list position, so
    // reordering rules handed one rule's cooldown to another.
    std::string label; // free text, shown in the UI, ignored by the engine
};

struct RuleSet
{
    int schemaVersion{1};
    std::string name{"unnamed"};
    std::vector<Rule> rules;
};

// How long the world takes to reflect this action, in seconds.
//
// This one number governs two things after a rule fires: the same ACTION cannot
// be repeated, and the same CONDITION -- the (subject, predicate) pair -- cannot
// draw another response. Both for the same reason, which is worth stating
// plainly because it is not a policy about remedies:
//
//     we acted, the world has not caught up, so do not decide again on
//     numbers that predate what we just did.
//
// Drinking a potion is the case that needs it. Measured in game, about two
// seconds pass between the equip call and health changing. Without a block,
//     health < 25% -> drink a potion
//     health < 25% -> cast a healing spell
//     health < 25% -> eat food
// applies all three inside 450 ms, each deciding on the same stale health.
// With one, the follower drinks and waits; if the potion worked, health is now
// above 25%, the other two conditions are false, and they never fire at all.
//
// What this number CANNOT do, and it is worth being explicit because the
// obvious guess is wrong:
//
//     enemy is high level -> equip the best armour
//     enemy is high level -> equip the best shield
//
// are complementary preparations rather than competing remedies, and no value
// here lets both happen. Rules are first-match-wins, so while the armour rule
// is available it wins every time: a zero settle makes it re-fire every tick,
// and a non-zero one merely slows the monopoly. The shield is never equipped.
//
// The mechanism that does solve it is AVAILABILITY. An action that is already
// in effect must report itself unavailable -- "equip the best armour" is not
// available when the best armour is already worn -- and evaluation then falls
// through to the next rule exactly as it does for "no potion in the bag". So
// every state-setting action added in Phase 4 owes an is-this-already-done
// check, or it will starve every rule beneath it.
//
// The number also doubles as an anti-thrash limit, which is why the state
// setters carry one despite nothing going stale. Those two concerns could want
// different values; they have not yet, so this stays a single table.
[[nodiscard]] double MinimumCooldown(ActionKind action) noexcept;

// Is this one of the equip actions, and what kind of thing does it name?
[[nodiscard]] bool IsEquip(ActionKind action) noexcept;
[[nodiscard]] Kind KindOf(ActionKind action) noexcept;

// Not every predicate means anything about every subject. The Snapshot carries
// no magicka for allies, and "distance" is meaningless for Self. Rather than
// quietly answering false -- which would look identical to a condition that was
// simply untrue -- the pair is rejected outright.
//
// This drives two things: the UI builds its cascading menu from it, so an
// impossible pair is never offered; and the evaluator reports InvalidCondition,
// so a rule that cannot work says so instead of never firing for no visible
// reason.
[[nodiscard]] bool IsPredicateValidFor(SubjectKind subject, PredicateKind predicate) noexcept;

// The above-counterpart of a below predicate -- HealthPctAbove for
// HealthPctBelow -- or the predicate itself for one with no counterpart.
// The editor lists both under one heading, the below values first.
[[nodiscard]] PredicateKind AboveOf(PredicateKind predicate) noexcept;
[[nodiscard]] bool IsAbove(PredicateKind predicate) noexcept;

// Which actions the current runtime can actually perform. src/game/ fills this
// in at startup. The UI greys out unsupported actions rather than letting
// someone author a rule that silently never fires -- see docs/PLAN.md 3.5.
struct Capabilities
{
    std::array<bool, static_cast<std::size_t>(ActionKind::COUNT)> supported{};

    // Supported in general but not available for THIS evaluation -- a resource
    // pool that is momentarily exhausted. A busy action is skipped exactly
    // like an unsupported one, so the next rule gets its turn and no cooldown
    // is spent; unlike unsupported, it is expected to clear on its own.
    std::array<bool, static_cast<std::size_t>(ActionKind::COUNT)> busy{};

    [[nodiscard]] bool Supports(ActionKind a) const noexcept
    {
        return supported[static_cast<std::size_t>(a)];
    }

    [[nodiscard]] bool Busy(ActionKind a) const noexcept
    {
        return busy[static_cast<std::size_t>(a)];
    }

    static Capabilities All() noexcept
    {
        Capabilities c;
        c.supported.fill(true);
        return c;
    }
};

} // namespace ft
