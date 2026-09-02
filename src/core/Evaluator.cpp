#include "Evaluator.h"

namespace ft
{
namespace
{

// Does one specific enemy satisfy the rule's predicate? Predicates that are not
// answerable about an enemy return false; IsPredicateValidFor rejects those
// pairs before we get here, so this is belt and braces.
bool EnemySatisfies(const EnemyView &e, const Rule &r)
{
    switch (r.predicate)
    {
    case PredicateKind::Always:
        return true;
    case PredicateKind::HealthPctBelow:
        return e.health.Pct() < r.conditionArg;
    case PredicateKind::WithinDistance:
        return e.distance <= r.conditionArg;
    default:
        return false;
    }
}

bool AllySatisfies(const AllyView &a, const Rule &r)
{
    switch (r.predicate)
    {
    case PredicateKind::Always:
        return true;
    case PredicateKind::HealthPctBelow:
        return a.health.Pct() < r.conditionArg;
    case PredicateKind::InBleedout:
        return a.inBleedout;
    case PredicateKind::WithinDistance:
        return a.distance <= r.conditionArg;
    default:
        return false;
    }
}

// When several group members match, which one does the rule bind to? Ordering
// by the predicate's own dimension is what makes the answer intuitive: asking
// about health should hand you the most hurt one, asking about distance the
// closest one.
bool OrdersByHealth(PredicateKind p)
{
    return p == PredicateKind::HealthPctBelow || p == PredicateKind::MagickaPctBelow ||
           p == PredicateKind::StaminaPctBelow;
}

const EnemyView *SelectEnemy(const Snapshot &s, const Rule &r)
{
    const EnemyView *best = nullptr;
    for (const auto &e : s.enemies)
    {
        if (!EnemySatisfies(e, r))
            continue;
        if (!best)
            best = &e;
        else if (OrdersByHealth(r.predicate) ? (e.health.Pct() < best->health.Pct()) : (e.distance < best->distance))
            best = &e;
    }
    return best;
}

const AllyView *SelectAlly(const Snapshot &s, const Rule &r)
{
    const AllyView *best = nullptr;
    for (const auto &a : s.allies)
    {
        if (!AllySatisfies(a, r))
            continue;
        if (!best)
            best = &a;
        else if (OrdersByHealth(r.predicate) ? (a.health.Pct() < best->health.Pct()) : (a.distance < best->distance))
            best = &a;
    }
    return best;
}

const EnemyView *NearestEnemy(const Snapshot &s)
{
    const EnemyView *best = nullptr;
    for (const auto &e : s.enemies)
    {
        if (!best || e.distance < best->distance)
            best = &e;
    }
    return best;
}

const AllyView *NearestAlly(const Snapshot &s)
{
    const AllyView *best = nullptr;
    for (const auto &a : s.allies)
    {
        if (!best || a.distance < best->distance)
            best = &a;
    }
    return best;
}

const EnemyView *FindEnemy(const Snapshot &s, ActorId id)
{
    for (const auto &e : s.enemies)
    {
        if (e.id == id)
            return &e;
    }
    return nullptr;
}

constexpr Binding Match(ActorId id)
{
    return Binding{id, true};
}

constexpr Binding NoMatch()
{
    return Binding{};
}

// CountAtLeast asks about the group, not a member, so there is no natural
// binding. Binding the nearest member keeps the action targetable.
Binding EvaluateCount(const Snapshot &s, const Rule &r)
{
    const bool enemies = r.subject == SubjectKind::Enemy;
    const auto count = enemies ? s.enemies.size() : s.allies.size();
    if (static_cast<float>(count) < r.conditionArg)
        return NoMatch();

    if (enemies)
    {
        const auto *e = NearestEnemy(s);
        return e ? Match(e->id) : NoMatch();
    }
    const auto *a = NearestAlly(s);
    return a ? Match(a->id) : NoMatch();
}

Binding EvaluateSelf(const Snapshot &s, const Rule &r)
{
    bool held = false;
    switch (r.predicate)
    {
    case PredicateKind::Always:
        held = true;
        break;
    case PredicateKind::HealthPctBelow:
        held = s.health.Pct() < r.conditionArg;
        break;
    case PredicateKind::MagickaPctBelow:
        held = s.magicka.Pct() < r.conditionArg;
        break;
    case PredicateKind::StaminaPctBelow:
        held = s.stamina.Pct() < r.conditionArg;
        break;
    case PredicateKind::InBleedout:
        held = s.inBleedout;
        break;
    case PredicateKind::InCombat:
        held = s.inCombat;
        break;
    default:
        break;
    }
    return held ? Match(s.self) : NoMatch();
}

Binding EvaluatePlayer(const Snapshot &s, const Rule &r)
{
    bool held = false;
    switch (r.predicate)
    {
    case PredicateKind::Always:
        held = true;
        break;
    case PredicateKind::HealthPctBelow:
        held = s.playerHealth.Pct() < r.conditionArg;
        break;
    case PredicateKind::InCombat:
        held = s.playerInCombat;
        break;
    case PredicateKind::WithinDistance:
        held = s.distanceToPlayer <= r.conditionArg;
        break;
    default:
        break;
    }
    return held ? Match(kPlayerFormID) : NoMatch();
}

Binding EvaluateCurrentTarget(const Snapshot &s, const Rule &r)
{
    if (!s.currentTarget)
        return NoMatch();
    if (r.predicate == PredicateKind::Always)
        return Match(s.currentTarget);

    // Anything beyond mere existence needs sensed data about the target, which
    // only exists if it is also in the enemy list.
    const auto *e = FindEnemy(s, s.currentTarget);
    if (!e)
        return NoMatch();
    return EnemySatisfies(*e, r) ? Match(s.currentTarget) : NoMatch();
}

} // namespace

Binding EvaluateCondition(const Rule &r, const Snapshot &s)
{
    if (!IsPredicateValidFor(r.subject, r.predicate))
        return NoMatch();

    if (r.predicate == PredicateKind::CountAtLeast)
        return EvaluateCount(s, r);

    switch (r.subject)
    {
    case SubjectKind::Self:
        return EvaluateSelf(s, r);

    case SubjectKind::Player:
        return EvaluatePlayer(s, r);

    case SubjectKind::Ally: {
        const auto *a = SelectAlly(s, r);
        return a ? Match(a->id) : NoMatch();
    }

    case SubjectKind::Enemy: {
        const auto *e = SelectEnemy(s, r);
        return e ? Match(e->id) : NoMatch();
    }

    case SubjectKind::CurrentTarget:
        return EvaluateCurrentTarget(s, r);

    default:
        return NoMatch();
    }
}

ActorId ResolveActionTarget(const Rule &r, const Snapshot &s, Binding binding, bool *ok)
{
    const auto yes = [&](ActorId id) {
        if (ok)
            *ok = true;
        return id;
    };
    const auto no = [&]() -> ActorId {
        if (ok)
            *ok = false;
        return 0;
    };

    switch (r.actionTarget)
    {
    case ActionTargetKind::ConditionSubject:
        return binding.ok ? yes(binding.id) : no();
    case ActionTargetKind::Self:
        return yes(s.self);
    case ActionTargetKind::Player:
        return yes(kPlayerFormID);
    case ActionTargetKind::CurrentTarget:
        return s.currentTarget ? yes(s.currentTarget) : no();
    default:
        return no();
    }
}

// Takes the whole rule, not just the action: a spell rule is only answerable
// with the spell in hand, and splitting that across two lookups is how the two
// drift apart.
bool HasResource(const Rule &r, const Snapshot &s)
{
    switch (r.action)
    {
    case ActionKind::DrinkHealthPotion:
        return s.potions.healthCount > 0;
    case ActionKind::DrinkMagickaPotion:
        return s.potions.magickaCount > 0;
    case ActionKind::DrinkStaminaPotion:
        return s.potions.staminaCount > 0;
    case ActionKind::DrinkPotion:
        return r.actionForm != 0 && s.potions.CountOf(r.actionForm) > 0;

    case ActionKind::CastSpell:
    case ActionKind::EquipSpell:
        // Knowing the spell is the inventory equivalent. Whether she can AFFORD
        // to cast it is a separate question and deliberately not asked here:
        // magicka cost depends on perks and skill, which live on the game side.
        // The action reports that back instead.
        return r.actionForm != 0 && s.spells.Knows(r.actionForm);

    default:
        return true; // most actions cost nothing from inventory
    }
}

bool EffectAlreadyActive(const Rule &r, const Snapshot &s)
{
    // Reported separately from "no potion" because the fix is different: the
    // follower has plenty, she is simply still absorbing the last one.
    switch (r.action)
    {
    case ActionKind::DrinkHealthPotion:
        return s.potions.healthEffectActive;
    case ActionKind::DrinkMagickaPotion:
        return s.potions.magickaEffectActive;
    case ActionKind::DrinkStaminaPotion:
        return s.potions.staminaEffectActive;
    case ActionKind::DrinkPotion:
        // A named potion could restore anything or nothing; only the per-form
        // cooldown spaces it.
        return false;

    case ActionKind::CastSpell:
        // The sustained-buff case. Oakflesh runs sixty seconds and no cooldown
        // worth picking is that long, so re-casting can only be stopped by
        // seeing the effect still running.
        return r.actionForm != 0 && s.spells.IsActive(r.actionForm);

    case ActionKind::EquipSpell:
        // Two ways this is already done, and both matter.
        //
        // IsEquipped is availability, the mechanism the note in Rule.h says
        // every state-setting action owes: without it a rule that equips what
        // is already in hand wins every evaluation and starves every rule below
        // it -- first-match-wins makes that a monopoly, not a nuisance.
        //
        // IsActive is the sustained-buff case. Oakflesh runs sixty seconds, and
        // no cooldown worth picking is that long; putting it back in her hand
        // while it is still up asks her to spend magicka renewing a buff that
        // never lapsed. Only the effect list can answer that.
        return r.actionForm != 0 && (s.spells.IsEquipped(r.actionForm) || s.spells.IsActive(r.actionForm));

    default:
        return false;
    }
}

Decision Evaluate(const RuleSet &rs, const Snapshot &snap, EvalContext &ctx, Trace *trace)
{
    if (trace)
        trace->assign(rs.rules.size(), Verdict::NotReached);

    Decision decision;

    for (std::size_t i = 0; i < rs.rules.size(); ++i)
    {
        const Rule &r = rs.rules[i];
        const auto put = [&](Verdict v) {
            if (trace)
                (*trace)[i] = v;
        };

        if (!r.enabled)
        {
            put(Verdict::Disabled);
            continue;
        }
        if (r.action == ActionKind::None || !ctx.caps.Supports(r.action))
        {
            put(Verdict::Unsupported);
            continue;
        }
        if (ctx.caps.Busy(r.action))
        {
            put(Verdict::Busy);
            continue;
        }
        // Reported separately from ConditionFalse on purpose: a pair that can
        // never be answered is an authoring mistake, not a condition that
        // happens to be untrue right now, and the debug column must not send
        // someone off to investigate a follower's health for nothing.
        if (!IsPredicateValidFor(r.subject, r.predicate))
        {
            put(Verdict::InvalidCondition);
            continue;
        }

        const Binding binding = EvaluateCondition(r, snap);
        if (!binding)
        {
            put(Verdict::ConditionFalse);
            continue;
        }
        if (!HasResource(r, snap))
        {
            put(Verdict::NoResource);
            continue;
        }
        // A cast she cannot pay for is not a cast. The AI would decline the
        // package and the rule would have spent its cooldown on nothing -- the
        // 12:20 run fired four heals at empty magicka. Reported, not fired, so
        // no cooldown moves and the next rule gets its turn.
        if (r.action == ActionKind::CastSpell && snap.magicka.current < snap.spells.CostOf(r.actionForm))
        {
            put(Verdict::CannotAfford);
            continue;
        }
        // Exact where the settle time is a guess: on a game whose potions
        // restore over time, the previous dose may still have seconds to run.
        if (EffectAlreadyActive(r, snap))
        {
            put(Verdict::EffectActive);
            continue;
        }

        bool targetOk = false;
        const ActorId target = ResolveActionTarget(r, snap, binding, &targetOk);
        if (!targetOk)
        {
            put(Verdict::NoTarget);
            continue;
        }

        // The cooldown key needs the resolved target, which is why this check
        // sits after target resolution rather than with the other cheap ones.
        const EvalContext::ActionKey key{r.action, r.actionForm, target};
        if (snap.now < ctx.BlockedUntil(key))
        {
            put(Verdict::ActionCooldown);
            continue;
        }

        put(Verdict::Fired);
        decision.ruleIndex = static_cast<int>(i);
        decision.action = r.action;
        decision.targetId = target;
        decision.actionArg = r.actionArg;
        decision.actionForm = r.actionForm;

        // The one cooldown there is: the ACTION goes on cooldown for as long
        // as its effect takes to show, and every rule that uses that action
        // reports it. Nothing is keyed by rule or by condition. Two rules on
        // the same condition with different actions therefore fire on
        // successive turns -- the list expresses preference, and the next
        // remedy applies if the first has not fixed things yet.
        ctx.Block(key, snap.now + MinimumCooldown(r.action));
        break;
    }

    return decision;
}

const char *Explain(Verdict v, ActionKind action) noexcept
{
    const bool spell = action == ActionKind::EquipSpell || action == ActionKind::CastSpell;

    switch (v)
    {
    case Verdict::NoResource:
        if (action == ActionKind::DrinkPotion)
            return "does not carry that potion";
        return spell ? "does not know that spell" : "no potion";

    case Verdict::EffectActive:
        if (action == ActionKind::EquipSpell)
            return "that spell is already in hand or still running";
        return spell ? "that spell is still running" : "previous dose still active";

    default:
        return ToString(v);
    }
}

const char *ToString(Verdict v) noexcept
{
    switch (v)
    {
    case Verdict::Fired:
        return "fired";
    case Verdict::Disabled:
        return "disabled";
    case Verdict::ConditionFalse:
        return "condition false";
    case Verdict::ActionCooldown:
        return "action used too recently";
    case Verdict::NoTarget:
        return "no target";
    case Verdict::NoResource:
        return "no potion";
    case Verdict::CannotAfford:
        return "not enough magicka";
    case Verdict::EffectActive:
        return "previous dose still active";
    case Verdict::Unsupported:
        return "unsupported";
    case Verdict::Busy:
        return "busy, skipped this evaluation";
    case Verdict::InvalidCondition:
        return "invalid condition";
    case Verdict::NotReached:
        return "not reached";
    }
    return "?";
}

} // namespace ft
