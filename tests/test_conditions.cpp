// The conditions, subject by subject: every stat predicate reads the stat it
// names of the actor it names; hit-by and using are asked of everyone; and
// the action targets that read from the binding. Written against what the
// coverage report (build/core-cov) showed the other tests never reach.
//
// No Skyrim, no SKSE, no CommonLibSSE -- see docs/PLAN.md section 3.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Evaluator.h"
#include "core/Vocabulary.h"

#include <string>

using namespace ft;
using ft::test::Player;

namespace
{

constexpr ActorId kFollower = 0xA2C94;
constexpr ActorId kOtherFollower = 0x201;
constexpr ActorId kEnemy = 0x101;
constexpr ActorId kFarEnemy = 0x102;
constexpr std::uint32_t kFirebolt = 0x00012FCD;
constexpr std::uint32_t kPettyGem = 0x0002E4E2;
constexpr std::uint32_t kCommonGem = 0x0002E4F4;

// A party of three and two enemies, everyone at full everything.
Snapshot Party()
{
    Snapshot s;
    s.self = kFollower;
    s.now = 100.0;
    s.inCombat = true;
    s.health = s.magicka = s.stamina = {100.0f, 100.0f};
    s.allies.push_back({kPlayerFormID, {100.0f, 100.0f}, 100.0f});
    s.allies.push_back({kOtherFollower, {100.0f, 100.0f}, 300.0f});
    for (auto &a : s.allies)
        a.magicka = a.stamina = {100.0f, 100.0f};
    s.enemies.push_back({kEnemy, {100.0f, 100.0f}, 200.0f});
    s.enemies.push_back({kFarEnemy, {100.0f, 100.0f}, 900.0f});
    for (auto &e : s.enemies)
        e.magicka = e.stamina = {100.0f, 100.0f};
    return s;
}

enum class Which
{
    Health,
    Magicka,
    Stamina
};

// The one stat of the one actor a subject names, for writing; and the same
// stat of the others in that actor's group, which a group condition may
// bind instead.
Stat &PickStat(Which which, Stat &h, Stat &m, Stat &st)
{
    switch (which)
    {
    case Which::Health:
        return h;
    case Which::Magicka:
        return m;
    default:
        return st;
    }
}

Stat &StatOf(Snapshot &s, SubjectKind subject, Which which)
{
    switch (subject)
    {
    case SubjectKind::Self:
        return PickStat(which, s.health, s.magicka, s.stamina);
    case SubjectKind::Player:
        return PickStat(which, Player(s).health, Player(s).magicka, Player(s).stamina);
    case SubjectKind::Enemy: {
        auto &e = s.enemies[1]; // the far one, so distance is not what binds
        return PickStat(which, e.health, e.magicka, e.stamina);
    }
    default: { // Ally, and Follower naming the same one
        auto &a = s.allies[1];
        return PickStat(which, a.health, a.magicka, a.stamina);
    }
    }
}

void SetOthers(Snapshot &s, SubjectKind subject, Which which, Stat value)
{
    if (subject == SubjectKind::Enemy)
        PickStat(which, s.enemies[0].health, s.enemies[0].magicka, s.enemies[0].stamina) = value;
    else if (subject == SubjectKind::Ally)
        PickStat(which, s.allies[0].health, s.allies[0].magicka, s.allies[0].stamina) = value;
}

bool IsGroup(SubjectKind subject)
{
    return subject == SubjectKind::Ally || subject == SubjectKind::Enemy;
}

// Whom the subject's condition must bind.
ActorId Expected(SubjectKind subject)
{
    switch (subject)
    {
    case SubjectKind::Self:
        return kFollower;
    case SubjectKind::Player:
        return kPlayerFormID;
    case SubjectKind::Enemy:
        return kFarEnemy;
    default:
        return kOtherFollower;
    }
}

PredicateKind Below(Which which)
{
    switch (which)
    {
    case Which::Health:
        return PredicateKind::HealthPctBelow;
    case Which::Magicka:
        return PredicateKind::MagickaPctBelow;
    default:
        return PredicateKind::StaminaPctBelow;
    }
}

PredicateKind Above(Which which)
{
    switch (which)
    {
    case Which::Health:
        return PredicateKind::HealthPctAbove;
    case Which::Magicka:
        return PredicateKind::MagickaPctAbove;
    default:
        return PredicateKind::StaminaPctAbove;
    }
}

Rule About(SubjectKind subject, PredicateKind predicate)
{
    Rule r;
    r.subject = subject;
    r.predicate = predicate;
    if (subject == SubjectKind::Follower)
        r.subjectForm = kOtherFollower;
    return r;
}

Verdict FirstVerdict(const RuleSet &rs, const Snapshot &s, Decision *out = nullptr)
{
    EvalContext ctx;
    Trace trace;
    const Decision d = Evaluate(rs, s, ctx, &trace);
    if (out)
        *out = d;
    return trace.at(0);
}

} // namespace

TEST_CASE("every stat predicate reads the stat it names of the actor it names", "[conditions]")
{
    // The named actor sits on one side of 50% and the rest of their group
    // on the other, with every other stat at 100%. A predicate reading the
    // wrong stat sees 100% everywhere; one reading the wrong actor binds
    // the wrong id. For Self, Player and a named Follower there is no
    // group, so the far side simply does not match.
    for (const auto subject :
         {SubjectKind::Self, SubjectKind::Player, SubjectKind::Ally, SubjectKind::Follower, SubjectKind::Enemy})
    {
        for (const auto which : {Which::Health, Which::Magicka, Which::Stamina})
        {
            INFO("subject " << DisplayName(subject) << ", stat " << static_cast<int>(which));
            REQUIRE(IsPredicateValidFor(subject, Below(which)));
            REQUIRE(IsPredicateValidFor(subject, Above(which)));

            Rule below = About(subject, Below(which));
            below.conditionArg = 0.5f;
            Rule above = About(subject, Above(which));
            above.conditionArg = 0.5f;

            Snapshot s = Party();
            StatOf(s, subject, which) = {30.0f, 100.0f};
            SetOthers(s, subject, which, {70.0f, 100.0f});
            REQUIRE(EvaluateCondition(below, s).id == Expected(subject));
            const Binding aboveAt30 = EvaluateCondition(above, s);
            if (IsGroup(subject))
                REQUIRE((aboveAt30.ok && aboveAt30.id != Expected(subject)));
            else
                REQUIRE_FALSE(aboveAt30.ok);

            StatOf(s, subject, which) = {70.0f, 100.0f};
            SetOthers(s, subject, which, {30.0f, 100.0f});
            REQUIRE(EvaluateCondition(above, s).id == Expected(subject));
            const Binding belowAt70 = EvaluateCondition(below, s);
            if (IsGroup(subject))
                REQUIRE((belowAt70.ok && belowAt70.id != Expected(subject)));
            else
                REQUIRE_FALSE(belowAt70.ok);
        }
    }
}

TEST_CASE("any is true of every subject, and binds the nearest of a group", "[conditions]")
{
    const Snapshot s = Party();
    REQUIRE(EvaluateCondition(About(SubjectKind::Self, PredicateKind::Any), s).id == kFollower);
    REQUIRE(EvaluateCondition(About(SubjectKind::Player, PredicateKind::Any), s).id == kPlayerFormID);
    REQUIRE(EvaluateCondition(About(SubjectKind::Ally, PredicateKind::Any), s).id == kPlayerFormID);
    REQUIRE(EvaluateCondition(About(SubjectKind::Follower, PredicateKind::Any), s).id == kOtherFollower);
    REQUIRE(EvaluateCondition(About(SubjectKind::Enemy, PredicateKind::Any), s).id == kEnemy);
}

TEST_CASE("hit by and using are asked of every subject, from that actor's own traits", "[conditions]")
{
    Snapshot s = Party();
    s.traits.hitBy = Bit(DamageKind::Fire);
    Player(s).traits.hitBy = Bit(DamageKind::Frost);
    s.enemies[0].traits.hitBy = Bit(DamageKind::Melee);
    s.allies[1].traits.Wield(DamageKind::Ranged);
    Player(s).traits.Wield(DamageKind::Shock);

    SECTION("hit by, of the follower, the player and an enemy")
    {
        Rule r = About(SubjectKind::Self, PredicateKind::HitBy);
        r.damageKind = DamageKind::Fire;
        REQUIRE(EvaluateCondition(r, s).ok);
        r.damageKind = DamageKind::Frost;
        REQUIRE_FALSE(EvaluateCondition(r, s).ok);

        r = About(SubjectKind::Player, PredicateKind::HitBy);
        r.damageKind = DamageKind::Frost;
        REQUIRE(EvaluateCondition(r, s).ok);
        r.damageKind = DamageKind::Fire;
        REQUIRE_FALSE(EvaluateCondition(r, s).ok);

        r = About(SubjectKind::Enemy, PredicateKind::HitBy);
        r.damageKind = DamageKind::Melee;
        REQUIRE(EvaluateCondition(r, s).id == kEnemy);
        r.damageKind = DamageKind::Any;
        REQUIRE(EvaluateCondition(r, s).id == kEnemy);
        r.damageKind = DamageKind::Fire;
        REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    }

    SECTION("using, of an ally and the player")
    {
        Rule r = About(SubjectKind::Ally, PredicateKind::HitType);
        r.damageKind = DamageKind::Ranged;
        REQUIRE(EvaluateCondition(r, s).id == kOtherFollower);
        r.damageKind = DamageKind::Melee;
        REQUIRE_FALSE(EvaluateCondition(r, s).ok);

        r = About(SubjectKind::Player, PredicateKind::HitType);
        r.damageKind = DamageKind::Shock;
        REQUIRE(EvaluateCondition(r, s).ok);
        r.damageKind = DamageKind::Ranged;
        REQUIRE_FALSE(EvaluateCondition(r, s).ok);
    }
}

TEST_CASE("armour and resistance are asked of an ally, and of the player", "[conditions]")
{
    Snapshot s = Party();
    s.allies[1].traits.armor = 0.6f;
    Player(s).traits.armor = 0.2f;

    Rule r = About(SubjectKind::Ally, PredicateKind::ArmorPctAbove);
    r.conditionArg = 0.5f;
    REQUIRE(EvaluateCondition(r, s).id == kOtherFollower);
    r.predicate = PredicateKind::ArmorPctBelow;
    REQUIRE(EvaluateCondition(r, s).id == kPlayerFormID);

    r = About(SubjectKind::Player, PredicateKind::ArmorPctBelow);
    r.conditionArg = 0.5f;
    REQUIRE(EvaluateCondition(r, s).ok);
    r.predicate = PredicateKind::ArmorPctAbove;
    REQUIRE_FALSE(EvaluateCondition(r, s).ok);
}

TEST_CASE("an action aimed at the ally goes to the ally the condition bound", "[target]")
{
    Snapshot s = Party();
    s.spells.known.push_back(kFirebolt);
    s.allies[1].health = {30.0f, 100.0f};

    Rule r = About(SubjectKind::Ally, PredicateKind::HealthPctBelow);
    r.conditionArg = 0.5f;
    r.actionTarget = ActionTargetKind::Ally;
    r.FirstAction().kind = ActionKind::CastSpell;
    r.FirstAction().form = kFirebolt;
    RuleSet rs;
    rs.rules.push_back(r);

    Decision d;
    REQUIRE(FirstVerdict(rs, s, &d) == Verdict::Fired);
    REQUIRE(d.targetId() == kOtherFollower);

    // The same by name, under Follower.
    rs.rules[0] = About(SubjectKind::Follower, PredicateKind::HealthPctBelow);
    rs.rules[0].conditionArg = 0.5f;
    rs.rules[0].actionTarget = ActionTargetKind::Ally;
    rs.rules[0].FirstAction() = r.FirstAction();
    REQUIRE(FirstVerdict(rs, s, &d) == Verdict::Fired);
    REQUIRE(d.targetId() == kOtherFollower);

    // Under a condition about the follower themself there is no "the ally":
    // the pair is unanswerable, and says so rather than firing at no one.
    rs.rules[0].subject = SubjectKind::Self;
    rs.rules[0].subjectForm = 0;
    REQUIRE(FirstVerdict(rs, s) == Verdict::InvalidCondition);
}

TEST_CASE("an action aimed at the enemy needs one to exist", "[target]")
{
    Snapshot s = Party();
    s.spells.known.push_back(kFirebolt);

    Rule r = About(SubjectKind::Self, PredicateKind::Any);
    r.actionTarget = ActionTargetKind::Enemy;
    r.FirstAction().kind = ActionKind::CastSpell;
    r.FirstAction().form = kFirebolt;
    RuleSet rs;
    rs.rules.push_back(r);

    SECTION("with no target and no enemy sensed, no one to aim at")
    {
        s.enemies.clear();
        s.currentTarget = 0;
        REQUIRE(FirstVerdict(rs, s) == Verdict::NoTarget);
    }

    SECTION("under an enemy condition, only the enemy it matched")
    {
        rs.rules[0].subject = SubjectKind::Enemy;
        rs.rules[0].predicate = PredicateKind::HealthPctBelow;
        rs.rules[0].conditionArg = 0.5f;
        REQUIRE(FirstVerdict(rs, s) == Verdict::ConditionFalse);
        s.enemies[1].health = {10.0f, 100.0f};
        Decision d;
        REQUIRE(FirstVerdict(rs, s, &d) == Verdict::Fired);
        REQUIRE(d.targetId() == kFarEnemy);
    }
}

TEST_CASE("a blow at a target the senses have lost has no target", "[target]")
{
    // The follower is fighting someone the snapshot no longer lists: gone
    // round a corner, dead, fled. The target resolves, the enemy does not.
    Snapshot s = Party();
    s.enemies.clear();
    s.currentTarget = 0x999;
    s.powerAttack = {true, true, 10.0f, 200.0f};

    Rule r = About(SubjectKind::Self, PredicateKind::Any);
    r.actionTarget = ActionTargetKind::Enemy;
    r.FirstAction().kind = ActionKind::PowerAttack;
    RuleSet rs;
    rs.rules.push_back(r);
    REQUIRE(FirstVerdict(rs, s) == Verdict::NoTarget);
}

TEST_CASE("the attacker of an enemy is not a target: the pair is unanswerable", "[target]")
{
    // "Attacker" is whoever is at the subject's throat; under an enemy
    // condition that would be one of us, and the menu does not offer it.
    // The rule engine says so rather than firing at a party member.
    Snapshot s = Party();
    s.spells.known.push_back(kFirebolt);
    s.enemies[0].traits.attacker = kOtherFollower;

    Rule r = About(SubjectKind::Enemy, PredicateKind::Any);
    r.actionTarget = ActionTargetKind::Attacker;
    r.FirstAction().kind = ActionKind::CastSpell;
    r.FirstAction().form = kFirebolt;
    RuleSet rs;
    rs.rules.push_back(r);
    REQUIRE_FALSE(IsActionTargetValidFor(SubjectKind::Enemy, ActionTargetKind::Attacker));
    REQUIRE(FirstVerdict(rs, s) == Verdict::InvalidCondition);

    // Under an ally condition it is the one who hit that ally: the player,
    // being the nearest, from the player's own traits.
    rs.rules[0].subject = SubjectKind::Ally;
    REQUIRE(FirstVerdict(rs, s) == Verdict::NoTarget);
    Player(s).traits.attacker = kEnemy;
    Decision d;
    REQUIRE(FirstVerdict(rs, s, &d) == Verdict::Fired);
    REQUIRE(d.targetId() == kEnemy);

    // A named follower, from that follower's entry in the party.
    rs.rules[0].subject = SubjectKind::Follower;
    rs.rules[0].subjectForm = kOtherFollower;
    REQUIRE(FirstVerdict(rs, s) == Verdict::NoTarget);
    s.allies[1].traits.attacker = kFarEnemy;
    REQUIRE(FirstVerdict(rs, s, &d) == Verdict::Fired);
    REQUIRE(d.targetId() == kFarEnemy);
}

TEST_CASE("a named soul gem is spent only while carried", "[evaluator]")
{
    Snapshot s = Party();
    s.rightWeapon = {true, false, true, 10.0f, 100.0f, 20.0f}; // cannot pay for the next hit

    Rule r = About(SubjectKind::Self, PredicateKind::Any);
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::ChargeSoulGem;
    r.FirstAction().form = kCommonGem;
    RuleSet rs;
    rs.rules.push_back(r);

    s.soulGems = {{kPettyGem, 3, 250.0f}};
    REQUIRE(FirstVerdict(rs, s) == Verdict::NoResource);
    s.soulGems.push_back({kCommonGem, 1, 1000.0f});
    REQUIRE(FirstVerdict(rs, s) == Verdict::Fired);

    // No gem named is nothing to spend, as a freshly added rule is.
    rs.rules[0].FirstAction().form = 0;
    REQUIRE(FirstVerdict(rs, s) == Verdict::NoResource);
}

TEST_CASE("every verdict has a word, plain and for each action", "[vocabulary]")
{
    // The log prints these; a verdict added without one would
    // show as "?". NotReached is the last, so the loop knows where to stop.
    for (int v = 0; v <= static_cast<int>(Verdict::NotReached); ++v)
    {
        const auto verdict = static_cast<Verdict>(v);
        INFO("verdict " << v);
        const std::string plain = ToString(verdict);
        REQUIRE_FALSE(plain.empty());
        REQUIRE(plain != "?");
        for (const auto action :
             {ActionKind::None, ActionKind::CastSpell, ActionKind::UseScroll, ActionKind::EquipArrows,
              ActionKind::EquipArmor, ActionKind::PowerAttack, ActionKind::Bash, ActionKind::ApplyPoison,
              ActionKind::ChargeSoulGem, ActionKind::DrinkStrongest})
        {
            const std::string worded = Explain(verdict, action);
            REQUIRE_FALSE(worded.empty());
            REQUIRE(worded != "?");
        }
    }
}

TEST_CASE("the predicate grid: every cell names one predicate, and every grid predicate its cell", "[grid]")
{
    // The five measures by four sides are the whole of the numeric
    // predicates; nothing else has a measure or a side, and the pairings
    // the editor and the evaluator read (AboveOf, BelowOf, ExtremesOf) are
    // the grid read back.
    int onGrid = 0;
    for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
    {
        const auto p = static_cast<PredicateKind>(i);
        const Grid g = GridOf(p);
        if (g.measure == Measure::None)
        {
            REQUIRE(g.side == Side::None);
            REQUIRE(AboveOf(p) == p);
            REQUIRE(BelowOf(p) == p);
            REQUIRE_FALSE(IsAbove(p));
            REQUIRE_FALSE(IsExtreme(p));
            continue;
        }
        ++onGrid;
        REQUIRE(PredicateAt(g.measure, g.side) == p);
        REQUIRE(IsAbove(p) == (g.side == Side::Above));
        REQUIRE(IsExtreme(p) == (g.side == Side::Lowest || g.side == Side::Highest));
        REQUIRE(IsResistance(p) == (g.measure == Measure::Resistance));
        REQUIRE(BelowOf(AboveOf(p)) == (g.side == Side::Below ? p : BelowOf(p)));
        REQUIRE((ArgumentFor(p) == ArgumentKind::Percent) == (g.side == Side::Below || g.side == Side::Above));
    }
    REQUIRE(onGrid == 20);
    REQUIRE(AboveOf(PredicateKind::ArmorPctBelow) == PredicateKind::ArmorPctAbove);
    REQUIRE(BelowOf(PredicateKind::ResistancePctAbove) == PredicateKind::ResistancePctBelow);
    REQUIRE(ExtremesOf(PredicateKind::StaminaPctBelow).lowest == PredicateKind::StaminaLowest);
    REQUIRE(ExtremesOf(PredicateKind::StaminaPctBelow).highest == PredicateKind::StaminaHighest);
    REQUIRE(ExtremesOf(PredicateKind::Status).lowest == PredicateKind::Status);
    REQUIRE(PredicateAt(Measure::None, Side::Below) == PredicateKind::Any);
}

// --- Not: the condition negated ---------------------------------------------

TEST_CASE("a negated condition holds exactly when the plain one does not", "[not]")
{
    Snapshot s = Party();
    RuleSet rs;
    Rule r = About(SubjectKind::Self, PredicateKind::HealthPctBelow);
    r.conditionArg = 0.5f;
    r.FirstAction().kind = ActionKind::DrinkStrongest;
    r.FirstAction().effect = "Restore Health";
    s.potions.Add(0x3EADE, 5, ConsumableKind::Potion, {"Restore Health", 50.0f, 0.0f});
    rs.rules.push_back(r);

    // Plain: false at full health, true when hurt.
    REQUIRE(FirstVerdict(rs, s) == Verdict::ConditionFalse);
    s.health = {10.0f, 100.0f};
    REQUIRE(FirstVerdict(rs, s) == Verdict::Fired);

    // Negated: the other way round, and it binds the follower.
    rs.rules[0].negated = true;
    REQUIRE(FirstVerdict(rs, s) == Verdict::ConditionFalse);
    s.health = {100.0f, 100.0f};
    Decision d;
    REQUIRE(FirstVerdict(rs, s, &d) == Verdict::Fired);
    REQUIRE(d.subjectId() == s.self);
}

TEST_CASE("a negated group condition means no one, and binds nobody", "[not]")
{
    // "No enemy is undead" -- true while none is, false the moment one is.
    Snapshot s = Party();
    s.spells.known.push_back(kFirebolt);
    Rule r = About(SubjectKind::Enemy, PredicateKind::Type);
    r.typeKind = TypeKind::Undead;
    r.negated = true;
    r.actionTarget = ActionTargetKind::Self;
    r.FirstAction().kind = ActionKind::CastSpell;
    r.FirstAction().form = kFirebolt;
    RuleSet rs;
    rs.rules.push_back(r);

    Decision d;
    REQUIRE(FirstVerdict(rs, s, &d) == Verdict::Fired);
    // Nobody matched, so the follower is the subject, not an enemy.
    REQUIRE(d.subjectId() == s.self);

    s.enemies[1].traits.kinds = Bit(TypeKind::Undead);
    REQUIRE(FirstVerdict(rs, s) == Verdict::ConditionFalse);
}

TEST_CASE("a negated condition has no matched one to act on", "[not]")
{
    // The targets that mean "the one the condition matched" are refused:
    // there is no such one, by definition.
    REQUIRE(IsActionTargetValidFor(SubjectKind::Ally, ActionTargetKind::Ally));
    REQUIRE_FALSE(IsActionTargetValidFor(SubjectKind::Ally, ActionTargetKind::Ally, true));
    REQUIRE_FALSE(IsActionTargetValidFor(SubjectKind::Corpse, ActionTargetKind::Corpse, true));
    // Enemy and Attacker do not need one: the follower's own fight answers
    // the first, whoever last hit them the second.
    REQUIRE(IsActionTargetValidFor(SubjectKind::Enemy, ActionTargetKind::Enemy, true));
    REQUIRE(IsActionTargetValidFor(SubjectKind::Self, ActionTargetKind::Attacker, true));

    // Reconcile puts a rule back in order when the Not goes on.
    Rule r = About(SubjectKind::Ally, PredicateKind::HealthPctBelow);
    r.conditionArg = 0.5f;
    r.actionTarget = ActionTargetKind::Ally;
    r.negated = true;
    Reconcile(r);
    REQUIRE(r.actionTarget == ActionTargetKind::Self);

    // And an enemy rule aims at whoever the follower is fighting.
    Snapshot s = Party();
    s.currentTarget = kEnemy;
    s.spells.known.push_back(kFirebolt);
    Rule e = About(SubjectKind::Enemy, PredicateKind::Type);
    e.typeKind = TypeKind::Undead;
    e.negated = true;
    e.actionTarget = ActionTargetKind::Enemy;
    e.FirstAction().kind = ActionKind::CastSpell;
    e.FirstAction().form = kFirebolt;
    RuleSet rs;
    rs.rules.push_back(e);
    Decision d;
    REQUIRE(FirstVerdict(rs, s, &d) == Verdict::Fired);
    REQUIRE(d.targetId() == kEnemy);
}

TEST_CASE("three conditions cannot be negated, and a Not on them is dropped", "[not]")
{
    REQUIRE_FALSE(CanNegate(PredicateKind::Any));
    REQUIRE_FALSE(CanNegate(PredicateKind::CombatBegins));
    REQUIRE_FALSE(CanNegate(PredicateKind::CombatEnds));
    REQUIRE(CanNegate(PredicateKind::HealthPctBelow));
    REQUIRE(CanNegate(PredicateKind::Status));

    // Reconcile clears it, so switching a negated rule to Any leaves a rule
    // that still fires rather than one that never can.
    Rule r = About(SubjectKind::Self, PredicateKind::Status);
    r.negated = true;
    r.predicate = PredicateKind::Any;
    Reconcile(r);
    REQUIRE_FALSE(r.negated);

    // Hand-edited past the editor, it is reported rather than obeyed.
    Snapshot s = Party();
    Rule bad = About(SubjectKind::Self, PredicateKind::Any);
    bad.negated = true;
    bad.FirstAction().kind = ActionKind::DrinkStrongest;
    bad.FirstAction().effect = "Restore Health";
    RuleSet rs;
    rs.rules.push_back(bad);
    REQUIRE(FirstVerdict(rs, s) == Verdict::InvalidCondition);
}

TEST_CASE("a negation does not invert the two guards around the condition", "[not]")
{
    Snapshot s = Party();
    s.spells.known.push_back(kFirebolt);

    // The farewell pass after a fight: only a Combat end rule runs on it,
    // and a negated rule must not fire there just because its condition is
    // false.
    Rule r = About(SubjectKind::Self, PredicateKind::HealthPctBelow);
    r.conditionArg = 0.5f;
    r.negated = true;
    r.FirstAction().kind = ActionKind::CastSpell;
    r.FirstAction().form = kFirebolt;
    RuleSet rs;
    rs.rules.push_back(r);
    REQUIRE(FirstVerdict(rs, s) == Verdict::Fired);
    s.inCombat = false;
    s.combatEnded = true;
    REQUIRE(FirstVerdict(rs, s) == Verdict::ConditionFalse);

    // A pair that cannot be asked at all stays unanswerable: negating a
    // question nobody can answer does not make it true.
    Rule invalid = About(SubjectKind::Player, PredicateKind::WeaponChargeNeeded);
    invalid.negated = true;
    invalid.FirstAction().kind = ActionKind::CastSpell;
    invalid.FirstAction().form = kFirebolt;
    RuleSet bad;
    bad.rules.push_back(invalid);
    Snapshot fresh = Party();
    REQUIRE(FirstVerdict(bad, fresh) == Verdict::InvalidCondition);
}
