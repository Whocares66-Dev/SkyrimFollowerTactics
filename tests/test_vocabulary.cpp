// The vocabulary: a stable wire id and a translatable display name, kept
// apart; the words a verdict is explained with; and the effects arranged
// for the menu. No Skyrim, no SKSE, no CommonLibSSE.

#include <catch2/catch_test_macros.hpp>

#include "Build.h"
#include "core/Effects.h"
#include "core/Evaluator.h"
#include "core/Vocabulary.h"

#include <string>

using namespace ft;
using namespace ft::test;

namespace
{
// Catch2 in this configuration has no StringMaker for std::string_view, so
// comparing one inside REQUIRE fails to link. Compare owned strings instead --
// it is also what the failure output wants to print.
std::string Str(std::string_view v)
{
    return std::string(v);
}
} // namespace

TEST_CASE("target points the follower at an enemy, once, and not at anyone else", "[target]")
{
    // Ally -> Attacked by -> Ranged -> Target on their attacker: the archer
    // shooting the ally becomes the follower's fight. Beneath it, the enemy
    // on the player, and a potion as the witness that the rules above fell
    // through.
    Snapshot s = Healthy();
    s.allies.push_back({kPlayerFormID, {100.0f, 100.0f}, 100.0f});
    s.allies.push_back({0x201, {60.0f, 100.0f}, 300.0f});
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 900.0f}); // the archer
    s.enemies.push_back({0x102, {100.0f, 100.0f}, 150.0f}); // the one in the follower's face
    s.enemies[1].target = kPlayerFormID;
    s.currentTarget = 0x102;
    s.allies[1].traits.hitBy = Bit(DamageKind::Melee) | Bit(DamageKind::Ranged);
    s.allies[1].traits.attacker = 0x101;

    Rule archer;
    archer.subject = SubjectKind::Ally;
    archer.predicate = PredicateKind::HitBy;
    archer.damageKind = DamageKind::Ranged;
    archer.actionTarget = ActionTargetKind::Attacker;
    archer.FirstAction().kind = ActionKind::Attack;

    Rule peel;
    peel.subject = SubjectKind::Enemy;
    peel.predicate = PredicateKind::Attacking;
    peel.actionTarget = ActionTargetKind::Enemy;
    peel.FirstAction().kind = ActionKind::Attack;

    RuleSet rs;
    rs.rules.push_back(archer);
    rs.rules.push_back(peel);
    rs.rules.push_back(HealBelow(2.0f)); // always true: the witness

    EvalContext ctx;
    Trace trace;
    Decision d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.action() == ActionKind::Attack);
    REQUIRE(d.targetId() == 0x101);

    // The engine took it: the archer rule is done and falls through. The
    // peel rule wants a different enemy, and is held by the SAME cooldown --
    // the action's, not the target's -- so the follower is not flicked
    // between the two. The witness gets the tick.
    s.now += 0.5;
    s.currentTarget = 0x101;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    REQUIRE(trace.at(1) == Verdict::ActionCooldown);
    REQUIRE(d.ruleIndex == 2);

    // Cooldown over: the peel rule gets its turn.
    s.now += 2.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(trace.at(0) == Verdict::EffectActive);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(d.targetId() == 0x102);

    // The engine did NOT take it: the target snaps back, and the rule
    // fires again once its cooldown is over, which is how the log shows
    // whether the choice stands.
    s.now += 2.5;
    s.currentTarget = 0x102;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.targetId() == 0x101);

    // An attacker who is not an enemy -- dead, fled, or an ally's stray
    // arrow -- is no one to point at.
    s.now += 5.0;
    s.allies[1].traits.attacker = 0x103;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(trace.at(0) == Verdict::NoTarget);
    REQUIRE(std::string(Explain(Verdict::NoTarget, ActionKind::Attack)) == "no enemy to point at");
    s.allies[1].traits.attacker = 0x101;

    // Out of a fight there is no target to set. The one way a rule is looked
    // at out of one is the farewell pass, so: on Combat end, attack the
    // enemy.
    {
        Rule farewell;
        farewell.subject = SubjectKind::Self;
        farewell.predicate = PredicateKind::CombatEnds;
        farewell.actionTarget = ActionTargetKind::Enemy;
        farewell.FirstAction().kind = ActionKind::Attack;
        RuleSet ending;
        ending.rules = {farewell};
        EvalContext fresh;
        Snapshot over = s;
        over.inCombat = false;
        over.combatEnded = true;
        REQUIRE_FALSE(Evaluate(ending, over, fresh, &trace).Fired());
        REQUIRE(trace.at(0) == Verdict::NotInCombat);
    }
    REQUIRE(std::string(Explain(Verdict::NotInCombat, ActionKind::Attack)) == "not in a fight");
    REQUIRE(std::string(Explain(Verdict::EffectActive, ActionKind::Attack)) == "already fighting them");

    // Ranged is a kind of its own: a sword blow on the ally is not it.
    s.inCombat = true;
    s.allies[1].traits.hitBy = Bit(DamageKind::Melee);
    REQUIRE_FALSE(EvaluateCondition(archer, s).ok);
    REQUIRE(MinimumCooldown(ActionKind::Attack) == 2.0);
}

TEST_CASE("a cast rule waits while the follower is casting a spell of their own", "[spell]")
{
    // Lightning Bolt on "magicka above half": fired into the AI's own cast
    // it interrupted every spell the follower began. So while they are
    // mid-cast the rule waits, spends no cooldown, and the rules beneath
    // get the tick; when the hands are free it fires.
    constexpr std::uint32_t kBolt = 0x000C96A2;
    Rule bolt;
    bolt.subject = SubjectKind::Self;
    bolt.predicate = PredicateKind::MagickaPctAbove;
    bolt.conditionArg = 0.5f;
    bolt.actionTarget = ActionTargetKind::Enemy;
    bolt.FirstAction() = {ActionKind::CastSpell, kBolt};
    // Enemy on the action side needs a condition about an enemy.
    bolt.subject = SubjectKind::Enemy;
    bolt.predicate = PredicateKind::Any;

    RuleSet rs;
    rs.rules.push_back(bolt);
    rs.rules.push_back(HealBelow(2.0f)); // always true: the witness

    Snapshot s = Healthy();
    s.spells.known.push_back(kBolt);
    s.enemies.push_back({0x101, {100.0f, 100.0f}, 300.0f});
    s.traits.Set(StatusKind::Casting);

    EvalContext ctx;
    Trace trace;
    Decision d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(trace.at(0) == Verdict::Casting);
    REQUIRE(d.ruleIndex == 1);
    REQUIRE(std::string(ToString(Verdict::Casting)) == "mid-cast on their own spell, waiting");

    // Still casting a second later: still waiting, still no cooldown.
    s.now += 1.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(trace.at(0) == Verdict::Casting);

    // Hands free: the bolt goes at once -- nothing was spent while waiting.
    s.traits.status = 0;
    s.now += 0.5;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(d.ruleIndex == 0);
    REQUIRE(d.action() == ActionKind::CastSpell);
    REQUIRE(d.targetId() == 0x101);

    // Our own cast in progress is the pool's business, reported before
    // this: Busy, not Casting.
    ctx.caps.busy[static_cast<std::size_t>(ActionKind::CastSpell)] = true;
    s.traits.Set(StatusKind::Casting);
    s.now += 5.0;
    d = Evaluate(rs, s, ctx, &trace);
    REQUIRE(trace.at(0) == Verdict::Busy);
}

TEST_CASE("attacked by anything, and an ally's magicka and stamina", "[conditions]")
{
    Snapshot s = Healthy();
    s.allies.push_back({kPlayerFormID, {100.0f, 100.0f}, 100.0f});
    s.allies.push_back({0x201, {100.0f, 100.0f}, 300.0f});
    s.allies.push_back({0x202, {100.0f, 100.0f}, 200.0f});
    s.allies[0].magicka = {100.0f, 100.0f};
    s.allies[0].stamina = {60.0f, 100.0f};
    s.allies[1].magicka = {10.0f, 100.0f};
    s.allies[2].magicka = {40.0f, 100.0f};
    s.allies[1].stamina = {90.0f, 100.0f};
    s.allies[2].stamina = {20.0f, 100.0f};

    // Attacked by Any: hit with anything at all in the window.
    Rule hit;
    hit.subject = SubjectKind::Ally;
    hit.predicate = PredicateKind::HitBy;
    hit.damageKind = DamageKind::Any;
    REQUIRE_FALSE(EvaluateCondition(hit, s).ok);
    s.allies[2].traits.hitBy = Bit(DamageKind::Frost);
    REQUIRE(EvaluateCondition(hit, s).id == 0x202);
    hit.damageKind = DamageKind::Fire;
    REQUIRE_FALSE(EvaluateCondition(hit, s).ok);

    // An ally's magicka and stamina, binding the one with the least of the
    // stat asked about -- not the least health.
    Rule low;
    low.subject = SubjectKind::Ally;
    low.predicate = PredicateKind::MagickaPctBelow;
    low.conditionArg = 0.5f;
    REQUIRE(EvaluateCondition(low, s).id == 0x201);
    low.predicate = PredicateKind::StaminaPctBelow;
    REQUIRE(EvaluateCondition(low, s).id == 0x202);
    low.predicate = PredicateKind::StaminaPctAbove;
    low.conditionArg = 0.5f;
    REQUIRE(EvaluateCondition(low, s).id == 0x201);

    // The player's too.
    Rule player;
    player.subject = SubjectKind::Player;
    player.predicate = PredicateKind::MagickaPctBelow;
    player.conditionArg = 0.3f;
    Player(s).magicka = {50.0f, 100.0f};
    REQUIRE_FALSE(EvaluateCondition(player, s).ok);
    Player(s).magicka = {20.0f, 100.0f};
    REQUIRE(EvaluateCondition(player, s).ok);
}

TEST_CASE("every wire name round-trips", "[vocabulary]")
{
    // A name that does not parse back is a rule that cannot be loaded from the
    // profile it was just saved to. Walk every enumerator rather than spot
    // checking, so adding one without a name fails here rather than in
    // somebody's save file.
    for (std::size_t i = 0; i < static_cast<std::size_t>(SubjectKind::COUNT); ++i)
    {
        const auto v = static_cast<SubjectKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(SubjectFromWireName(WireName(v)) == v);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
    {
        const auto v = static_cast<PredicateKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(PredicateFromWireName(WireName(v)) == v);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionTargetKind::COUNT); ++i)
    {
        const auto v = static_cast<ActionTargetKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(ActionTargetFromWireName(WireName(v)) == v);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto v = static_cast<ActionKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(ActionFromWireName(WireName(v)) == v);
    }
    for (const Hand v : {Hand::None, Hand::Left, Hand::Right, Hand::Both})
    {
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(IsWireName(WireName(v)));
        REQUIRE(HandFromWireName(WireName(v)) == v);
        REQUIRE(DisplayName(v).size() > 0);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(StatusKind::COUNT); ++i)
    {
        const auto v = static_cast<StatusKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(IsWireName(WireName(v)));
        REQUIRE(StatusFromWireName(WireName(v)) == v);
        REQUIRE(DisplayName(v).size() > 0);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(TypeKind::COUNT); ++i)
    {
        const auto v = static_cast<TypeKind>(i);
        REQUIRE(Str(WireName(v)) != "Unknown");
        REQUIRE(IsWireName(WireName(v)));
        REQUIRE(TypeFromWireName(WireName(v)) == v);
        REQUIRE(DisplayName(v).size() > 0);
    }
}

TEST_CASE("every wire name is a slug, and no display name is", "[vocabulary]")
{
    // THE GUARD. Display text and the file format must stay separable, or the
    // first person to translate the UI translates the data format with it and
    // profiles stop loading across languages. Asserting the shapes are disjoint
    // means the two cannot be quietly merged later: a translated string carries
    // capitals, spaces or accents and cannot satisfy IsWireName.
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto v = static_cast<ActionKind>(i);
        REQUIRE(IsWireName(WireName(v)));
        REQUIRE(Str(WireName(v)) != Str(DisplayName(v)));
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
    {
        REQUIRE(IsWireName(WireName(static_cast<PredicateKind>(i))));
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(SubjectKind::COUNT); ++i)
    {
        REQUIRE(IsWireName(WireName(static_cast<SubjectKind>(i))));
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionTargetKind::COUNT); ++i)
    {
        REQUIRE(IsWireName(WireName(static_cast<ActionTargetKind>(i))));
    }
}

TEST_CASE("the slug format rejects anything a translator would produce", "[vocabulary]")
{
    REQUIRE(IsWireName("drink-strongest-health-potion"));
    REQUIRE(IsWireName("self"));
    REQUIRE(IsWireName("weapon-charge-needed"));

    REQUIRE_FALSE(IsWireName("Drink Strongest Healing Potion")); // display text
    REQUIRE_FALSE(IsWireName("SanteEnDessousDe"));               // a translation
    REQUIRE_FALSE(IsWireName("sante-en-dessous-de-Ã©"));         // non-ASCII
    REQUIRE_FALSE(IsWireName("DrinkStrongest"));                 // capitals
    REQUIRE_FALSE(IsWireName("-leading"));
    REQUIRE_FALSE(IsWireName("trailing-"));
    REQUIRE_FALSE(IsWireName("double--hyphen"));
    REQUIRE_FALSE(IsWireName(""));
}

TEST_CASE("an unknown wire name is rejected, not guessed at", "[vocabulary]")
{
    // A profile written by a newer build will name things this one has never
    // heard of. Returning nullopt lets the loader drop that rule with a log
    // line instead of refusing the whole file.
    REQUIRE_FALSE(PredicateFromWireName("armour-rating-below").has_value());
    REQUIRE_FALSE(ActionFromWireName("cast-healing-spell").has_value());
    REQUIRE_FALSE(SubjectFromWireName("").has_value());

    // And display text is not a key. This is the property that keeps the file
    // format independent of the player's language.
    REQUIRE_FALSE(ActionFromWireName(DisplayName(ActionKind::DrinkStrongest)).has_value());
}

TEST_CASE("the argument shape tells the UI which widget to draw", "[vocabulary]")
{
    REQUIRE(ArgumentFor(PredicateKind::HealthPctBelow) == ArgumentKind::Percent);

    // A predicate that takes no argument must not be given a slider that
    // silently writes a meaningless number into the profile.
    REQUIRE(ArgumentFor(PredicateKind::Any) == ArgumentKind::None);
    REQUIRE(ArgumentFor(PredicateKind::CombatBegins) == ArgumentKind::None);
}

TEST_CASE("every value has display text and help text", "[vocabulary]")
{
    // Cheap way to make adding a vocabulary entry force a decision about what
    // it means to a player, rather than shipping a blank dropdown or tooltip.
    for (std::size_t i = 0; i < static_cast<std::size_t>(PredicateKind::COUNT); ++i)
    {
        const auto v = static_cast<PredicateKind>(i);
        REQUIRE(DisplayName(v).size() > 0);
        // Any says it all in its name, and a Type leaf is the kind's own
        // name: neither has a tooltip, on purpose.
        if (v != PredicateKind::Any && v != PredicateKind::Type)
            REQUIRE(Describe(v).size() > 0);
    }
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto v = static_cast<ActionKind>(i);
        REQUIRE(DisplayName(v).size() > 0);
        REQUIRE(Describe(v).size() > 0);
    }
}

TEST_CASE("every verdict has a word for the column, for every action", "[vocabulary]")
{
    // The status column's word and its tooltip's sentence come from the
    // same place, so a new verdict cannot reach the panel wordless, and
    // the word for a blow out of a fight says so (it said "count: 0").
    for (std::size_t v = 0; v <= static_cast<std::size_t>(Verdict::NotReached); ++v)
    {
        const auto verdict = static_cast<Verdict>(v);
        for (std::size_t a = 0; a < static_cast<std::size_t>(ActionKind::COUNT); ++a)
        {
            const auto action = static_cast<ActionKind>(a);
            const std::string word = Brief(verdict, action);
            REQUIRE(word != "?");
            REQUIRE((word.empty() == (verdict == Verdict::NotReached)));
            REQUIRE(std::string(Explain(verdict, action)) != "?");
        }
    }
    REQUIRE(std::string(Brief(Verdict::NotInCombat, ActionKind::Attack)) == "no fight");
    REQUIRE(std::string(Brief(Verdict::NoResource, ActionKind::Shout)) == "no shout");
    REQUIRE(std::string(Brief(Verdict::NoResource, ActionKind::DrinkPotion)) == "count: 0");
    REQUIRE(std::string(Brief(Verdict::EffectActive, ActionKind::EquipArmor)) == "pinned");
    // The nouns the menus head with: the equips' things, the charges' gems.
    REQUIRE(std::string(Noun(ActionKind::EquipWeapon)) == "weapon");
    REQUIRE(std::string(Noun(ActionKind::ChargeWeakestSoulGem)) == "weakest soul gem");
    REQUIRE(Noun(ActionKind::DrinkStrongest).empty());
}

TEST_CASE("a verdict is worded for the action it happened to", "[vocabulary]")
{
    // The log said "previous dose still active" about an EQUIP rule, which is
    // true of nothing and sent a reader looking for a potion that was never in
    // the rule. Same verdict, different action, different sentence.
    REQUIRE(std::string(Explain(Verdict::EffectActive, ActionKind::DrinkStrongest)) == "previous dose still active");
    REQUIRE(std::string(Explain(Verdict::EffectActive, ActionKind::EquipSpell)) ==
            "already pinned, or nothing of that kind pinned to let go");

    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::DrinkStrongest)) == "none in inventory");
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::EquipSpell)) == "does not know that spell");
    REQUIRE(std::string(Explain(Verdict::NoResource, ActionKind::EquipWeapon)) == "does not carry that weapon");

    // Everything else is action-independent and must not drift from ToString.
    for (std::size_t i = 0; i < static_cast<std::size_t>(ActionKind::COUNT); ++i)
    {
        const auto action = static_cast<ActionKind>(i);
        REQUIRE(std::string(Explain(Verdict::ConditionFalse, action)) ==
                std::string(ToString(Verdict::ConditionFalse)));
        REQUIRE(std::string(Explain(Verdict::ActionCooldown, action)) ==
                std::string(ToString(Verdict::ActionCooldown)));
    }
}

TEST_CASE("the carried effects arrange themselves for the menu", "[effects]")
{
    const auto potions =
        ArrangeEffects(ConsumableKind::Potion, {"Zesty Zap", "Resist Fire", "Restore Magicka", "Fortify Conjuration",
                                                "Restore Health", "Aetherial Boon", "Restore Health"});
    std::vector<std::string> order;
    for (const auto &e : potions)
        order.push_back(e.name);
    REQUIRE(order == std::vector<std::string>{"Restore Health", "Restore Magicka", "Resist Fire", "Fortify Conjuration",
                                              "Aetherial Boon", "Zesty Zap"});
    // The restores share a group; the unknown pair share the last one, past
    // every known group.
    REQUIRE(potions[0].group == potions[1].group);
    REQUIRE(potions[1].group != potions[2].group);
    REQUIRE(potions[4].group == potions[5].group);
    REQUIRE(potions[4].group > potions[3].group);

    const auto poisons = ArrangeEffects(ConsumableKind::Poison, {"Paralysis", "Damage Health", "Weakness to Fire"});
    REQUIRE(poisons[0].name == "Damage Health");
    REQUIRE(poisons[1].name == "Weakness to Fire");
    REQUIRE(poisons[2].name == "Paralysis");

    REQUIRE(std::string(EffectLabel("Restore Health")) == "Health");
    REQUIRE(std::string(EffectLabel("Damage Stamina")) == "Stamina");
    REQUIRE(std::string(EffectLabel("Resist Fire")) == "Resist Fire");
    REQUIRE(std::string(EffectLabel("Restore Healthiness")) == "Restore Healthiness");
    REQUIRE(EffectUseless("Cure Disease"));
    REQUIRE(EffectUseless("Resist Disease"));
    REQUIRE_FALSE(EffectUseless("Cure Poison"));
}
