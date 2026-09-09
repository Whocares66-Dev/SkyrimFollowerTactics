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
    // One particular other follower, named by Rule::subjectForm: an ally
    // asked about alone.
    Follower,
    // The dead nearby, a group like Enemy: a corpse the rule can bind and
    // aim a Reanimate at. Its own predicates: none about, or the highest
    // or lowest level -- filtered to what the rule's spell can raise, so
    // "highest" never picks a corpse the spell then fails on.
    Corpse,

    COUNT
};

// What is being asked about the subject. Rule::conditionArg carries the
// threshold where one applies: a 0..1 fraction for the Pct predicates, a
// plain count for CountAtLeast.
enum class PredicateKind : std::uint8_t
{
    Any,
    HealthPctBelow,
    StaminaPctBelow,
    MagickaPctBelow,
    // The edges of a fight, true on one tick each. There is no "in combat"
    // beside them: tactics only run in a fight, so it would always hold,
    // and bleeding out is a Status. CombatBegins holds on the
    // first evaluation of a fight and the list runs on as usual beneath it.
    // CombatEnds holds on one farewell evaluation after the follower leaves
    // combat -- and on THAT evaluation nothing else holds: a standing "Any"
    // rule must not re-pin the bow the moment the after-fight restore has
    // put the travelling gear back.
    CombatBegins,
    CombatEnds,
    CountAtLeast,
    // The subject is in the status Rule::statusKind names: poisoned,
    // burning, fleeing ... Any subject.
    Status,
    // The follower's own weapons, each hand asked, Self only, under one
    // "Weapon" heading above Armor as the editor walks this enum. Charge
    // needed: an enchanted weapon in hand cannot pay for one more hit (one
    // hit draws one fixed number, the enchantment's cost, whatever the
    // attack; the game's own "Uses" is the charge over that number). The
    // poison pair: a weapon in hand takes a poison and has none (None), or
    // carries one (Active); with a poisoned sword right and a clean dagger
    // left both hold.
    WeaponChargeNeeded,
    WeaponPoisonNone,
    WeaponPoisonActive,
    // The share of a blow the subject's armour turns away, 0 to 0.8, under
    // conditionArg. Any subject.
    ArmorPctBelow,
    // The subject's resistance to Rule::damageKind, the game's percent as
    // a fraction (50% is 0.5; a weakness is below zero), under
    // conditionArg. Any subject.
    ResistancePctBelow,
    // The subject has been hit with Rule::damageKind in the last few
    // seconds. Any subject.
    AttackedBy,
    // The enemy is going for the player, or is the one the player is
    // going for: the two that make a party fight as one -- peel the one
    // on the player, or hit what the player hits. Enemy and Target.
    AttackingPlayer,
    TargetOfPlayer,
    // The group's extremes: true of the group when it has anyone, binding
    // the member with the least or the most of the measure. Ally and Enemy
    // only. The resistance ones are of Rule::damageKind.
    HealthLowest,
    HealthHighest,
    StaminaLowest,
    StaminaHighest,
    MagickaLowest,
    MagickaHighest,
    ArmorLowest,
    ArmorHighest,
    ResistanceLowest,
    ResistanceHighest,
    // The other side of the five Pct predicates. Listed after the rest so
    // the editor's menu, which walks this enum, keeps them beneath their
    // below-counterparts; AboveOf pairs the two.
    HealthPctAbove,
    StaminaPctAbove,
    MagickaPctAbove,
    ArmorPctAbove,
    ResistancePctAbove,
    // The subject commands a summon or a raised corpse right now, or does
    // not. Any actor the snapshot has traits for: the engine keeps a
    // commanded-actor list per actor. Listed under one "Summon" heading.
    SummonNone,
    SummonActive,
    // The corpses: none about, or the one of the highest or lowest level.
    // Corpse only. Not IsExtreme: they have no below-predicate to hang
    // under, and their measure is a level, not a fraction.
    CorpseNone,
    LevelHighest,
    LevelLowest,

    COUNT
};

// Who the action is applied to. The same cast as the condition's subject,
// so THEN reads like IF: "Player: Attacked by fire -> Player: Cast fire
// shield". Ally and Enemy mean THE ally or enemy the condition matched --
// "Ally attacked by fire -> Ally cast fire shield" is one ally -- so they
// are only valid when the condition is about one (IsActionTargetValidFor).
// Not every action makes sense on every target: a potion is only ever
// drunk by oneself (IsActionValidFor).
enum class ActionTargetKind : std::uint8_t
{
    Self,
    Player,
    Ally,
    Enemy,
    CurrentTarget,
    // Whoever last attacked the condition's subject: the enemy at the
    // ally's throat, for the rule that answers it.
    Attacker,
    // One particular other follower, named by Rule::actionTargetForm.
    Follower,
    // THE corpse the condition bound: where a Reanimate goes.
    Corpse,

    COUNT
};

enum class ActionKind : std::uint8_t
{
    None,
    // The order of the enum is the order of the menu: Target first, which
    // is only offered under Enemy and Attacker; then, under Self, Equip,
    // Consume, Cast spell, Use power.
    //
    // Fight the one the rule aims at: make them the follower's combat
    // target, and leave HOW to the AI -- a warrior swings, an archer shoots,
    // a mage casts, each by their own scoring. There is no "attack" in the
    // engine, only a target; this sets it. Done already when they are the
    // current target, so the rule falls through instead of re-firing.
    Target,
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
    // Charge: a soul gem into the weapon in hand that cannot pay for its
    // next hit, the right before the left. Strongest is the largest gem that would not
    // overfill it (the smallest carried when every one would); weakest the
    // smallest carried; then one named gem. A reusable gem (Azura's Star)
    // is emptied, not lost. Grouped with the poisons under Weapon in the Then
    // cascade, Charge before Poison.
    ChargeStrongestSoulGem,
    ChargeWeakestSoulGem,
    ChargeSoulGem, // one specific gem, named by actionForm
    // Apply: a poison on the weapon in hand, after the equips because that
    // is the order of the thing -- set the gear, then choose the poison.
    // Three "weakest carried" and three "strongest carried" policies by
    // what the poison damages, then one named poison. Needs a weapon that
    // takes a poison in hand (anything but a staff) and not already
    // poisoned; the evaluator reports each.
    ApplyWeakestHealthPoison,
    ApplyWeakestMagickaPoison,
    ApplyWeakestStaminaPoison,
    ApplyStrongestHealthPoison,
    ApplyStrongestMagickaPoison,
    ApplyStrongestStaminaPoison,
    ApplyPoison, // one specific poison, named by actionForm
    // Consume: the three "strongest carried" potion policies, the three
    // "weakest carried" ones -- the cheap potions first, the strong ones
    // kept for when they matter -- then one named thing of each consumable
    // kind. All go through the game's own equip routine, which is what
    // consumes an item.
    DrinkHealthPotion,         // the strongest carried
    DrinkMagickaPotion,        // the strongest carried
    DrinkStaminaPotion,        // the strongest carried
    DrinkWeakestHealthPotion,  // the weakest carried
    DrinkWeakestMagickaPotion, // the weakest carried
    DrinkWeakestStaminaPotion, // the weakest carried
    DrinkPotion,               // one specific potion, named by actionForm
    EatFood,                   // one specific food, named by actionForm
    EatIngredient,             // one specific ingredient, named by actionForm
    CastSpell,
    // A power (Embrace of Shadows, Battle Cry): a spell record cast from
    // the voice rather than a hand, no magicka. Performed through a Shout
    // package wrapping the power as a one-word shout, since the UseMagic
    // package never fires one (measured 2026-09-04, docs/ACTIONS.md 7).
    // Otherwise a cast: a lease, a deadline, and it waits on the
    // follower's own cast in progress.
    UsePower,
    // A shout (Unrelenting Force): a TESShout record the follower has,
    // through the same Shout package with the shout itself in its input.
    Shout,
    // No "stop fighting", "flee" or "hold position": the combat AI decides
    // whether it respects a pushed package, and a rule that may or may not
    // be obeyed is worse than none (removed 2026-09-04).

    COUNT
};

// The consume actions, named or by policy; and the kind a named one names
// (Potion for the three policies, which are potions too).
[[nodiscard]] bool IsConsume(ActionKind action) noexcept;
// The apply-a-poison actions and the charge-with-a-soul-gem actions, which
// the Then cascade groups under Weapon.
[[nodiscard]] bool IsApply(ActionKind action) noexcept;
[[nodiscard]] bool IsCharge(ActionKind action) noexcept;
[[nodiscard]] ConsumableKind ConsumableOf(ActionKind action) noexcept;

// The three actions that fire through the package pool: a spell from a
// hand, a power and a shout from the voice.
[[nodiscard]] bool IsCast(ActionKind action) noexcept;

// One thing to do. A rule carries a list of these, in order.
struct Action
{
    ActionKind kind{ActionKind::None};

    // Which spell, for CastSpell; which power, for UsePower; which potion,
    // food or ingredient, for the named consume actions; which thing, for
    // the equip actions. A FormID, and deliberately opaque here:
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
    // Which follower, for SubjectKind::Follower: the actor's FormID, as
    // opaque here as an action's form is.
    std::uint32_t subjectForm{0};
    PredicateKind predicate{PredicateKind::Any};
    float conditionArg{0.0f};
    // Which status, for PredicateKind::Status. Ignored by every other
    // predicate.
    StatusKind statusKind{StatusKind::Poisoned};
    // Which kind of damage, for the Resistance predicates and AttackedBy.
    // Ignored by every other predicate.
    DamageKind damageKind{DamageKind::Fire};

    ActionTargetKind actionTarget{ActionTargetKind::Self};
    // Which follower, for ActionTargetKind::Follower.
    std::uint32_t actionTargetForm{0};

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

// The same for the THEN side. A target of Ally or Enemy is "the one the
// condition matched", so it needs a condition about an ally (Ally, or a
// named follower) or an enemy (Enemy, or the current target); every other
// target stands on its own. And an action must make sense on its target: a
// potion, an equip, a retreat are the follower's own; Target picks an
// enemy, so it takes Enemy or Attacker and nothing else -- on the current
// target it would always be done already. Cast is the one action aimed
// anywhere; which spells suit which target is the menu's business, since
// core does not know a spell's delivery.
[[nodiscard]] bool IsActionTargetValidFor(SubjectKind subject, ActionTargetKind target) noexcept;
[[nodiscard]] bool IsActionValidFor(ActionTargetKind target, ActionKind action) noexcept;

// Put a rule back in order after its condition changed: a target the new
// subject cannot supply falls back to Self, and an action that makes no
// sense on the target that results is blanked. What the editor calls after
// every change to the IF side, so the THEN side never shows a pair the
// menus would not offer.
void Reconcile(Rule &rule) noexcept;

// The above-counterpart of a below predicate -- HealthPctAbove for
// HealthPctBelow -- or the predicate itself for one with no counterpart.
// The editor lists both under one heading, the below values first.
[[nodiscard]] PredicateKind AboveOf(PredicateKind predicate) noexcept;
[[nodiscard]] bool IsAbove(PredicateKind predicate) noexcept;

// The group extremes a predicate's heading offers -- Lowest and Highest
// under Health, under Armor -- or the predicate itself twice when it has
// none. The editor lists them first under the heading; IsExtreme says a
// predicate is one of them.
struct Extremes
{
    PredicateKind lowest;
    PredicateKind highest;
};
[[nodiscard]] Extremes ExtremesOf(PredicateKind predicate) noexcept;
[[nodiscard]] bool IsExtreme(PredicateKind predicate) noexcept;

// The resistance family -- below, above, lowest, highest -- the predicates
// that read Rule::damageKind as the resistance asked about.
[[nodiscard]] bool IsResistance(PredicateKind predicate) noexcept;

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
