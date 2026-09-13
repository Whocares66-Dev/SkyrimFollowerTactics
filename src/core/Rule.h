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
// threshold where one applies: a 0..1 fraction for the Pct predicates.
enum class PredicateKind : std::uint8_t
{
    Any,
    HealthPctBelow,
    StaminaPctBelow,
    MagickaPctBelow,
    // The edges of a fight, true on one tick each. There is no "in combat"
    // beside them: tactics only run in a fight, so it would always hold,
    // and bleeding out is a Status. CombatBegins holds on the first
    // evaluation of a fight, CombatEnds on one farewell evaluation after
    // the follower leaves combat. On its edge, every rule with that
    // predicate is looked at FIRST, wherever it sits, and their lists run
    // one after another in list order, one action per tick (Evaluate); the
    // rest of the list gets a turn only if none of them can do anything.
    // On the farewell evaluation nothing else holds at all: a standing
    // "Any" rule must not re-pin the bow the moment the after-fight restore
    // has put the travelling gear back.
    CombatBegins,
    CombatEnds,
    // (A count of the group -- "at least N enemies" -- was here until
    // 2026-09-08. Nobody offered it in the end: an ally's count changes too
    // rarely to be a condition, and the enemy's was not wanted.)
    // The enemy is attacking a member of the party -- their combat target
    // is that member -- or is attacked by one, being that member's target:
    // the two that make a party fight as one -- peel the one on the
    // player, or hit what the player hits. The member is
    // Rule::subjectForm: 0 for the player, the follower's own id for
    // themself, another follower's id otherwise. Enemy only.
    Attacking,
    AttackedBy,
    // The subject's hit type: wielding Rule::damageKind -- a melee weapon,
    // a bow or crossbow, a spell or a staff (Magic), or anything that does
    // that kind of damage, an enchanted blade, a staff of flames, a
    // poisoned dagger. Any subject; Any is "anything at all in hand".
    HitType,
    // The subject has been hit with Rule::damageKind in the last few
    // seconds. Any subject. Listed here, between the fight's edges and
    // Status, as the editor's menu groups them.
    HitBy,
    // The subject is the kind of being Rule::typeKind names: a Nord, an
    // elf of any kind, undead, a dragon ... Asked of an enemy or an ally,
    // the subjects that are a group; the follower, the player and a named
    // follower are each one being. Above Status, as the editor's menu has
    // it.
    Type,
    // The subject is in the status Rule::statusKind names: poisoned,
    // burning, fleeing ... Any subject; a few kinds are not asked about
    // the follower themself (IsStatusValidFor).
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
    // An enemy: THE enemy the condition matched when the condition is about
    // one; otherwise whoever the follower is fighting, else the nearest.
    Enemy,
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
    // (The menu draws these in its own order, the more active thing first
    // -- see ActionItems in game/UI.cpp; the enum's order is nothing.)
    //
    // Attack the one the rule aims at: make them the follower's combat
    // target, and leave HOW to the AI -- a warrior swings, an archer shoots,
    // a mage casts, each by their own scoring. There is no "attack" in the
    // engine, only a target; this sets it. Done already when they are the
    // current target, so the rule falls through instead of re-firing.
    Attack,
    // One power attack, now, with what is in the hands: the right hand's
    // blade or two-hander, the left's alone, both at once, or the fists;
    // never a bow, a staff or a spell hand. At an enemy, as Attack is: it
    // points the follower at them first when they are not the target.
    // Needs the stamina the swing costs, which the snapshot prices. One
    // swing per firing.
    PowerAttack,
    // A bash, and a power bash, with what blocks: a shield or a torch in
    // the left hand, or the right hand's weapon with the left hand empty.
    // The interrupt against a caster; the power one staggers hardest of
    // any blow. Aimed and priced as a power attack is.
    Bash,
    PowerBash,
    // The equip actions PIN: what they put on stays on, against the engine's
    // own swap and the combat AI's choice, until another rule or the panel
    // lets it go. A plain equip would not do -- the AI re-derives what to
    // hold on its own schedule, so a sword put in their hand without a pin
    // lasts until its next decision, which may be the same second. Each
    // names a thing by actionForm, and Weapon and Spell a hand as well; a
    // form of 0 is "none": let go of every pin of that kind -- in that hand,
    // for a weapon or a spell -- and take those things off, so the AI
    // decides again. The arrows have two policies beside the named kinds:
    // the hardest-hitting arrows carried, or the weakest, chosen fresh each
    // time the rule fires, so the pin follows the quiver as kinds run out.
    EquipWeapon,
    EquipArrows,
    EquipStrongestArrows,
    EquipWeakestArrows,
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
    // The strongest poison carried with Action::effect, or the weakest --
    // the cheap ones first, the strong ones kept for when they matter --
    // or one named by actionForm. Needs a weapon that takes a poison in
    // hand (anything but a staff) and not already poisoned; the evaluator
    // reports each.
    ApplyStrongest,
    ApplyWeakest,
    ApplyPoison,
    // Consume: the strongest potion carried with Action::effect, the
    // weakest, then one named thing of each consumable kind. All go
    // through the game's own equip routine, which is what consumes an item.
    DrinkStrongest,
    DrinkWeakest,
    DrinkPotion, // one specific potion, named by actionForm
    // Food, and the few ingredients that are food (a follower does not
    // taste the rest to learn them), the same three ways each.
    EatStrongestFood,
    EatWeakestFood,
    EatFood, // one specific food, named by actionForm
    EatStrongestIngredient,
    EatWeakestIngredient,
    EatIngredient, // one specific ingredient, named by actionForm
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
    // A scroll carried, read from a hand through the UseMagic package with
    // the scroll as its spell: no magicka, and the scroll is spent. Named
    // by actionForm; carried scrolls are in the snapshot's known list, as
    // spells are, since knowing and carrying are the one question here.
    UseScroll,
    // No "stop fighting", "flee" or "hold position": the combat AI decides
    // whether it respects a pushed package, and a rule that may or may not
    // be obeyed is worse than none (removed 2026-09-04).

    COUNT
};

// The consume actions, named or by policy; and the kind a named one names
// (Potion for the drink policies, which are potions too).
[[nodiscard]] bool IsConsume(ActionKind action) noexcept;
// The eight that choose by an effect: the strongest or weakest potion,
// food, ingredient or poison carried with Action::effect.
[[nodiscard]] bool IsPolicy(ActionKind action) noexcept;
// Of the policies, the four that take the strongest; the rest the weakest.
[[nodiscard]] bool IsStrongest(ActionKind action) noexcept;
// The apply-a-poison actions and the charge-with-a-soul-gem actions, which
// the Then cascade groups under Weapon.
[[nodiscard]] bool IsApply(ActionKind action) noexcept;
[[nodiscard]] bool IsCharge(ActionKind action) noexcept;
// The two arrow policies: equips that choose their form by damage rather
// than name one, so they pin like an equip and resolve like a policy.
[[nodiscard]] bool IsArrowsPolicy(ActionKind action) noexcept;
[[nodiscard]] ConsumableKind ConsumableOf(ActionKind action) noexcept;

// The four actions that fire through the package pool: a spell and a
// scroll from a hand, a power and a shout from the voice.
[[nodiscard]] bool IsCast(ActionKind action) noexcept;

// The three blows sent to the animation graph: a power attack, a bash, a
// power bash. Each priced in stamina and reach by the snapshot's Blow.
[[nodiscard]] bool IsBlow(ActionKind action) noexcept;

// The actions that name one thing by Action::form: the casts, the equips,
// and the named consumables -- one potion, food, ingredient, poison or soul
// gem. The policies choose their form at evaluation; Attack and the blows
// name nothing. What the profile writes and reads a form for, what the
// panel offers a picker for, and what the evaluator checks is carried: one
// answer, so a new action cannot be named in the editor and lost by the
// save (ApplyPoison and ChargeSoulGem were, until 2026-09-11).
[[nodiscard]] bool NamesForm(ActionKind action) noexcept;
[[nodiscard]] bool NamesConsumable(ActionKind action) noexcept;

// One thing to do. A rule carries a list of these, in order.
//
// (The NOLINT: clang's analyzer reports "value assigned to field 'kind' is
// garbage" here for Decision's std::optional<Step> moved out of Evaluate
// after the call that fills it, which the analyzer does not follow. There
// is no line of ours on that path to mark; the false positive is the
// library's optional as the analyzer models it.)
struct Action // NOLINT(clang-analyzer-core.uninitialized.Assign)
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

    // Which variant of the form, for the equip actions: the row picked, as
    // the Inventory tab has them, so a rule can say the smithed dagger and
    // not the plain one; none for the form, whichever variant. A variant
    // cannot be reissued to another kind of copy, so the rule is available
    // whenever a row of it is in the bag, and set aside as not carried
    // while none is.
    std::optional<ItemVariant> variant;
    // The thing's name as the panel last saw it, for any action that names
    // a form: "Iron Dagger (+4)", "Potion of Minor Healing", "Firebolt".
    // What the rule reads while the thing is away -- drunk, handed over,
    // a copy whose id nothing holds -- and refreshed by the game whenever
    // it is there, so it is always the current name of what the rule
    // names now. For display only; nothing is matched by it.
    std::string name;

    // Which hand, for EquipWeapon and EquipSpell: Left, Right, or Both for a
    // two-hander, a bow, a master spell -- or an either-hand spell in each
    // hand at once. Ignored by every other action.
    Hand hand{Hand::None};

    // A number the action takes, when it does: the sustain time of a
    // concentration spell, for CastSpell.
    float arg{0.0f};

    // For CastSpell: cast from both hands at once, for the stronger and
    // dearer spell. Only a spell the snapshot says the follower CAN dual
    // cast -- one the school's Dual Casting perk covers, whose record does
    // not take both hands (vanilla's master spells do; a mod's may not;
    // the slot decides, never the level) -- and the menu offers no other.
    bool dual{false};

    // Which effect, for the eight policies: the magic effect's name as the
    // game shows it -- "Restore Health", "Resist Fire", "Damage Stamina" --
    // read off the bottles the follower carries. A name rather than a form
    // so that the same rule reads any mod's potion of the effect, and so a
    // profile shares across load orders. Ignored by every other action.
    std::string effect;

    [[nodiscard]] bool operator==(const Action &) const = default;
};

struct Rule
{
    bool enabled{true};

    SubjectKind subject{SubjectKind::Self};
    // Which follower, for SubjectKind::Follower: the actor's FormID, as
    // opaque here as an action's form is. For Attacking and AttackedBy, the
    // party member: 0 for the player, the follower's own id for themself.
    std::uint32_t subjectForm{0};
    PredicateKind predicate{PredicateKind::Any};
    float conditionArg{0.0f};
    // Which status, for PredicateKind::Status. Ignored by every other
    // predicate.
    StatusKind statusKind{StatusKind::Poisoned};
    // Which kind of being, for PredicateKind::Type. Ignored by every other
    // predicate.
    TypeKind typeKind{TypeKind::Man};
    // Which kind of damage, for the Resistance predicates, Hit type and
    // Hit by. Ignored by every other predicate.
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

    [[nodiscard]] bool operator==(const Rule &) const = default;
};

struct RuleSet
{
    std::vector<Rule> rules;
};

// How long the world takes to reflect this action, in seconds.
//
// After a rule fires, the same ACTION cannot be repeated until this has passed
// -- "the same action" as EvalContext::ActionKey defines it. The reason is not
// a policy about remedies:
//
//     we acted, the world has not caught up, so do not decide again on
//     numbers that predate what we just did.
//
// Drinking a potion is the case that needs it: measured in game, about two
// seconds pass between the equip call and health changing, and without a block
// the rule would drink again on the next turn, on health that predates the
// first bottle.
//
// Nothing is keyed by the RULE or by the CONDITION, so
//     health < 25% -> drink a potion
//     health < 25% -> cast a healing spell
//     health < 25% -> eat food
// is not one response per problem. The potion fires; on the next turn that
// rule reports its cooldown and the heal fires; on the turn after, the food.
// The list is a PREFERENCE ORDER, worked down a remedy per turn, and the
// follower stops when health rises past 25% and the condition goes false.
// Tested as "one situation draws its remedies in list order, one per turn".
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
// The two equips that name a hand: a weapon's, a spell's.
[[nodiscard]] bool TakesHand(ActionKind action) noexcept;

// Not every predicate means anything about every subject: the fight's
// edges and the weapons in hand are the follower's own, the extremes a
// group's, the corpse questions the corpses'. Rather than quietly
// answering false -- which would look identical to a condition that was
// simply untrue -- the pair is rejected outright.
//
// This drives two things: the UI builds its cascading menu from it, so an
// impossible pair is never offered; and the evaluator reports InvalidCondition,
// so a rule that cannot work says so instead of never firing for no visible
// reason.
[[nodiscard]] bool IsPredicateValidFor(SubjectKind subject, PredicateKind predicate) noexcept;

// Can this status be asked about this subject? Bleeding out, casting,
// fleeing and staggered are not asked about the follower themself: no
// action can be taken while any of them holds, so the rule could never do
// anything. The player neither bleeds out nor flees. About anyone else they
// are fair questions.
[[nodiscard]] bool IsStatusValidFor(SubjectKind subject, StatusKind status) noexcept;

// The same for a kind of damage under the predicates that read one. Nothing
// resists a blow or an arrow but armour, which is its own condition, and
// nothing resists "any": a resistance is asked about a kind that something
// resists. Hit type: Any would be true of everyone -- hands with nothing in
// them read as Melee, so every actor hits with something -- which is the
// plain Any condition wearing a heading that promises a filter. Any belongs
// to Hit by alone, where it is a real question: hit with anything at all,
// inside the window. A predicate that reads no damage kind is unaffected.
[[nodiscard]] bool IsDamageKindValidFor(PredicateKind predicate, DamageKind kind) noexcept;

// The same for the THEN side. A target of Ally or Corpse is "the one the
// condition matched", so it needs a condition about an ally (Ally, or a
// named follower) or a corpse; Enemy reads from an enemy condition and
// stands on its own under any other; Attacker is whoever hit one of us.
// And an action must make sense on its target: a potion and an equip are
// the follower's own; Attack and the blows go at an enemy, so they take
// Enemy or Attacker and nothing else. The casts are aimed anywhere but a
// corpse (a spell may be); which spells suit which target is the menu's
// business, since core does not know a spell's delivery.
[[nodiscard]] bool IsActionTargetValidFor(SubjectKind subject, ActionTargetKind target) noexcept;
[[nodiscard]] bool IsActionValidFor(ActionTargetKind target, ActionKind action) noexcept;

// Put a rule back in order after its condition changed: a target the new
// subject cannot supply falls back to Self, and an action that makes no
// sense on the target that results is blanked. What the editor calls after
// every change to the IF side, so the THEN side never shows a pair the
// menus would not offer.
void Reconcile(Rule &rule) noexcept;

// The grid the numeric predicates make: five measures by four sides. A
// measure is what is read off the actor -- a fraction of health, stamina
// or magicka, the armour share, a resistance -- and a side is how it is
// asked: below or above the rule's number, or the group's lowest or
// highest. Every function beneath reads the grid, so widening it -- a
// sixth measure -- is one row per predicate here and nothing else. A
// predicate off the grid (Any, the edges, Type, Status, the corpse
// questions) has no measure and no side.
enum class Measure : std::uint8_t
{
    None,
    Health,
    Stamina,
    Magicka,
    Armor,
    Resistance
};
enum class Side : std::uint8_t
{
    None,
    Below,
    Above,
    Lowest,
    Highest
};
struct Grid
{
    Measure measure{Measure::None};
    Side side{Side::None};
};
[[nodiscard]] Grid GridOf(PredicateKind predicate) noexcept;
// The predicate at a cell of the grid, or Any for an empty one.
[[nodiscard]] PredicateKind PredicateAt(Measure measure, Side side) noexcept;

// The above-counterpart of a below predicate -- HealthPctAbove for
// HealthPctBelow -- and back, or the predicate itself for one with no
// counterpart. The editor lists both under one heading, the below values
// first.
[[nodiscard]] PredicateKind AboveOf(PredicateKind predicate) noexcept;
[[nodiscard]] PredicateKind BelowOf(PredicateKind predicate) noexcept;
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

// What the runtime can do for this evaluation. src/game/ fills it in each
// tick. Every action is supported but the casts, which need the package
// pool (game/Packages.h): with it unavailable a cast rule reports
// Unsupported rather than silently never firing.
struct Capabilities
{
    bool castingAvailable{true};

    // Supported in general but not available for THIS evaluation -- a resource
    // pool that is momentarily exhausted. A busy action is skipped exactly
    // like an unsupported one, so the next rule gets its turn and no cooldown
    // is spent; unlike unsupported, it is expected to clear on its own.
    std::array<bool, static_cast<std::size_t>(ActionKind::COUNT)> busy{};

    [[nodiscard]] bool Supports(ActionKind a) const noexcept
    {
        return a != ActionKind::None && (castingAvailable || !IsCast(a));
    }

    [[nodiscard]] bool Busy(ActionKind a) const noexcept
    {
        return busy[static_cast<std::size_t>(a)];
    }
};

} // namespace ft
