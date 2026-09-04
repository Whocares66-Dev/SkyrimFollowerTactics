#include "Evaluator.h"

#include <algorithm>

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
    case PredicateKind::Any:
        return true;
    case PredicateKind::HealthPctBelow:
        return e.health.Pct() < r.conditionArg;
    case PredicateKind::HealthPctAbove:
        return e.health.Pct() > r.conditionArg;
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
    case PredicateKind::Any:
        return true;
    case PredicateKind::HealthPctBelow:
        return a.health.Pct() < r.conditionArg;
    case PredicateKind::HealthPctAbove:
        return a.health.Pct() > r.conditionArg;
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
// about low health should hand you the most hurt one, about high health the
// healthiest, about distance the closest one.
bool OrdersByHealth(PredicateKind p)
{
    return p == PredicateKind::HealthPctBelow || p == PredicateKind::MagickaPctBelow ||
           p == PredicateKind::StaminaPctBelow || IsAbove(p);
}

// Is `candidate` a better binding than `best` for this predicate?
bool Better(PredicateKind p, const Stat &candidateHealth, float candidateDistance, const Stat &bestHealth,
            float bestDistance)
{
    if (!OrdersByHealth(p))
        return candidateDistance < bestDistance;
    return IsAbove(p) ? candidateHealth.Pct() > bestHealth.Pct() : candidateHealth.Pct() < bestHealth.Pct();
}

const EnemyView *SelectEnemy(const Snapshot &s, const Rule &r)
{
    const EnemyView *best = nullptr;
    for (const auto &e : s.enemies)
    {
        if (!EnemySatisfies(e, r))
            continue;
        if (!best || Better(r.predicate, e.health, e.distance, best->health, best->distance))
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
        if (!best || Better(r.predicate, a.health, a.distance, best->health, best->distance))
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

// The hands an equip action asks for: its own for a weapon or a spell,
// none for armour and ammunition, which have no hand.
Hand HandsWanted(const Action &a)
{
    return (a.kind == ActionKind::EquipWeapon || a.kind == ActionKind::EquipSpell) ? a.hand : Hand::None;
}

// A "none" action: an equip naming nothing, which lets go of every pin of
// its kind.
bool LetsGo(const Action &a)
{
    return IsEquip(a.kind) && a.form == 0;
}

bool AnyPinOf(const std::vector<Pin> &pins, Kind kind)
{
    return std::any_of(pins.begin(), pins.end(), [kind](const Pin &p) { return p.thing.kind == kind; });
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
    case PredicateKind::Any:
        held = true;
        break;
    case PredicateKind::HealthPctBelow:
        held = s.health.Pct() < r.conditionArg;
        break;
    case PredicateKind::HealthPctAbove:
        held = s.health.Pct() > r.conditionArg;
        break;
    case PredicateKind::MagickaPctBelow:
        held = s.magicka.Pct() < r.conditionArg;
        break;
    case PredicateKind::MagickaPctAbove:
        held = s.magicka.Pct() > r.conditionArg;
        break;
    case PredicateKind::StaminaPctBelow:
        held = s.stamina.Pct() < r.conditionArg;
        break;
    case PredicateKind::StaminaPctAbove:
        held = s.stamina.Pct() > r.conditionArg;
        break;
    case PredicateKind::InBleedout:
        held = s.inBleedout;
        break;
    case PredicateKind::InCombat:
        held = s.inCombat;
        break;
    case PredicateKind::CombatBegins:
        held = s.combatBegan;
        break;
    case PredicateKind::CombatEnds:
        held = s.combatEnded;
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
    case PredicateKind::Any:
        held = true;
        break;
    case PredicateKind::HealthPctBelow:
        held = s.playerHealth.Pct() < r.conditionArg;
        break;
    case PredicateKind::HealthPctAbove:
        held = s.playerHealth.Pct() > r.conditionArg;
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
    if (r.predicate == PredicateKind::Any)
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

    // The farewell pass after a fight: the fight is over, and the only thing
    // true of this moment is that it has ended.
    if (s.combatEnded && r.predicate != PredicateKind::CombatEnds)
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

// Takes the whole action, not just its kind: a spell action is only
// answerable with the spell in hand, and splitting that across two lookups is
// how the two drift apart.
bool HasResource(const Action &a, const Snapshot &s)
{
    switch (a.kind)
    {
    case ActionKind::DrinkHealthPotion:
        return s.potions.healthCount > 0;
    case ActionKind::DrinkMagickaPotion:
        return s.potions.magickaCount > 0;
    case ActionKind::DrinkStaminaPotion:
        return s.potions.staminaCount > 0;
    case ActionKind::DrinkPotion:
        return a.form != 0 && s.potions.CountOf(a.form) > 0;

    case ActionKind::CastSpell:
        // Knowing the spell is the inventory equivalent. Whether she can AFFORD
        // to cast it is a separate question and deliberately not asked here:
        // magicka cost depends on perks and skill, which live on the game side.
        // The action reports that back instead.
        return a.form != 0 && s.spells.Knows(a.form);

    case ActionKind::EquipWeapon:
    case ActionKind::EquipSpell:
    case ActionKind::EquipArrows:
    case ActionKind::EquipArmor: {
        // "None" needs nothing. A named thing must be hers, and of the kind
        // the action says: a hand-edited profile could put a spell under
        // equip-weapon, and that is a rule that can never work.
        if (LetsGo(a))
            return true;
        const Holdable *thing = FindHoldable(s.loadout, a.form);
        return thing && thing->kind == KindOf(a.kind);
    }

    default:
        return true; // most actions cost nothing from inventory
    }
}

bool EffectAlreadyActive(const Action &a, const Snapshot &s)
{
    // Reported separately from "no potion" because the fix is different: the
    // follower has plenty, she is simply still absorbing the last one.
    switch (a.kind)
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
        return a.form != 0 && s.spells.IsActive(a.form);

    case ActionKind::EquipWeapon:
    case ActionKind::EquipSpell:
    case ActionKind::EquipArrows:
    case ActionKind::EquipArmor: {
        // Availability, the mechanism the note in Rule.h says every
        // state-setting action owes: without it a rule that pins what is
        // already pinned wins every evaluation and starves every rule below
        // it -- first-match-wins makes that a monopoly, not a nuisance.
        // Pinned, not merely equipped: the rule's promise is the pin, and a
        // thing the AI happens to be holding is not yet kept. "None" is done
        // when there is nothing of its kind to let go of.
        if (LetsGo(a))
            return !AnyPinOf(s.pins, KindOf(a.kind));
        const Pin *pin = FindPin(s.pins, a.form);
        return pin && Covers(pin->hands, HandsWanted(a));
    }

    default:
        return false;
    }
}

namespace
{

// Can this action be done now? Fired if so; otherwise why not. The equip
// actions add their satisfied pin to `heldAbove`, which is what outranks a
// conflicting equip beneath them.
Verdict Availability(const Action &a, const Snapshot &snap, const EvalContext &ctx, ActorId target,
                     std::vector<Pin> &heldAbove)
{
    if (a.kind == ActionKind::None || !ctx.caps.Supports(a.kind))
        return Verdict::Unsupported;
    if (ctx.caps.Busy(a.kind))
        return Verdict::Busy;
    if (!HasResource(a, snap))
        return Verdict::NoResource;

    if (IsEquip(a.kind))
    {
        const Holdable *thing = LetsGo(a) ? nullptr : FindHoldable(snap.loadout, a.form);
        // A pin is a promise the AI will use it. A spell above her skill it
        // never would, so the promise cannot be kept, and the rule says so
        // rather than equipping something that gets swapped straight out.
        if (thing && thing->unusable)
            return Verdict::CannotHold;
        const Hand hands = HandsWanted(a);
        const bool outranked = std::any_of(heldAbove.begin(), heldAbove.end(), [&](const Pin &held) {
            return thing ? Conflicts(*thing, hands, held.thing, held.hands) : held.thing.kind == KindOf(a.kind);
        });
        if (outranked)
            return Verdict::Outranked;
        if (EffectAlreadyActive(a, snap))
        {
            if (const Pin *pin = thing ? FindPin(snap.pins, thing->form) : nullptr)
                heldAbove.push_back(*pin);
            return Verdict::EffectActive;
        }
    }
    else
    {
        // A cast she cannot pay for is not a cast. The AI would decline the
        // package and the rule would have spent its cooldown on nothing --
        // the 12:20 run fired four heals at empty magicka.
        if (a.kind == ActionKind::CastSpell && snap.magicka.current < snap.spells.CostOf(a.form))
            return Verdict::CannotAfford;
        // Exact where the settle time is a guess: on a game whose potions
        // restore over time, the previous dose may still have seconds to run.
        if (EffectAlreadyActive(a, snap))
            return Verdict::EffectActive;
    }

    const EvalContext::ActionKey key{a.kind, a.form, target};
    if (snap.now < ctx.BlockedUntil(key))
        return Verdict::ActionCooldown;
    return Verdict::Fired;
}

// Cannot be done YET, as opposed to cannot be done: worth waiting for.
bool Transient(Verdict v)
{
    return v == Verdict::Busy || v == Verdict::ActionCooldown;
}

// Do the NEXT action of a list, from `from`, in order: one per tick, like
// nested rules firing on successive ticks. Not as many as can be done at
// once -- ten casts on one tick would want ten UseMagic records and an AI
// that could run them, and a half-second stagger is the cadence of
// everything else here. The action done goes into the decision and onto
// cooldown, and the list waits at the one after it for the next tick.
// One that cannot be done at all is skipped. One that cannot be done YET
// stops the run: it is left for the next tick -- IF the rule has
// committed, which it has once any of its actions is done. A rule whose
// very first action is only blocked for the moment has not begun, and
// yields to the rules beneath it, as a single-action rule always did.
// Returns whether the run stopped on a wait.
bool Run(const std::vector<Action> &actions, std::size_t from, int ruleIndex, ActorId target, const Snapshot &snap,
         EvalContext &ctx, Decision &decision, std::vector<Verdict> &verdicts, std::vector<Pin> &heldAbove)
{
    verdicts.assign(actions.size(), Verdict::NotReached);
    for (std::size_t i = from; i < actions.size(); ++i)
    {
        const Action &a = actions[i];
        const Verdict v = Availability(a, snap, ctx, target, heldAbove);
        verdicts[i] = v;
        if (v == Verdict::Fired)
        {
            decision.ruleIndex = ruleIndex;
            decision.steps.push_back({a, target});
            // The one cooldown there is: the ACTION goes on cooldown for as
            // long as its effect takes to show, and every rule that uses it
            // reports it. Nothing is keyed by rule or by condition.
            ctx.Block({a.kind, a.form, target}, snap.now + MinimumCooldown(a.kind));
            // The rest waits for the next tick, or the list is through.
            ctx.pending = i + 1 < actions.size() ? EvalContext::Sequence{ruleIndex, target, actions, i + 1}
                                                 : EvalContext::Sequence{};
            return false;
        }
        if (!Transient(v))
            continue; // cannot be done at all: skipped
        const bool committed = from > 0;
        if (committed)
        {
            ctx.pending = {ruleIndex, target, actions, i};
            return true;
        }
        // Not begun: the rest of this rule is not reached this tick either.
        return false;
    }
    ctx.pending = {};
    return false;
}

// The word for the whole rule: fired if anything was done, else the first
// action's reason -- which for a single-action rule is the reason.
Verdict Summary(const Decision &decision, const std::vector<Verdict> &verdicts)
{
    if (decision.Fired())
        return Verdict::Fired;
    for (const Verdict v : verdicts)
        if (v != Verdict::NotReached)
            return v;
    return Verdict::Unsupported;
}

} // namespace

Decision Evaluate(const RuleSet &rs, const Snapshot &snap, EvalContext &ctx, Trace *trace, ActionTrace *actionTrace)
{
    if (trace)
        trace->assign(rs.rules.size(), Verdict::NotReached);
    if (actionTrace)
    {
        actionTrace->resize(rs.rules.size());
        for (std::size_t i = 0; i < rs.rules.size(); ++i)
            (*actionTrace)[i].assign(rs.rules[i].actions.size(), Verdict::NotReached);
    }

    Decision decision;

    // The fight is over: what a rule was in the middle of is dropped.
    if (snap.combatEnded)
        ctx.pending = {};

    // A rule in progress owns the tick until its list is through. Nothing
    // else is evaluated, and the status column says so.
    if (ctx.pending.Active())
    {
        const EvalContext::Sequence seq = ctx.pending;
        std::vector<Pin> none;
        std::vector<Verdict> verdicts;
        Run(seq.actions, seq.next, seq.ruleIndex, seq.target, snap, ctx, decision, verdicts, none);
        const auto i = static_cast<std::size_t>(seq.ruleIndex);
        if (trace && i < trace->size())
            (*trace)[i] = Summary(decision, verdicts);
        if (actionTrace && i < actionTrace->size() && (*actionTrace)[i].size() == verdicts.size())
            (*actionTrace)[i] = verdicts;
        return decision;
    }

    // What the satisfied equip rules above hold. An equip rule whose
    // condition holds and whose thing is pinned is DONE, and falls through
    // so a complementary rule beneath it -- the helm after the shield --
    // gets its turn; but a rule beneath it that would take the same hand or
    // slot must not, or the two trade places every tick: the sword rule
    // pins the sword, falls through as done, the bow rule takes the hands,
    // the sword rule is undone and fires again. Priority means the rule
    // above keeps what it holds for as long as its condition holds.
    std::vector<Pin> heldAbove;

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
        // Nothing this runtime can do: said before the condition is looked
        // at, because the answer does not depend on it.
        const bool anySupported = std::any_of(r.actions.begin(), r.actions.end(), [&](const Action &a) {
            return a.kind != ActionKind::None && ctx.caps.Supports(a.kind);
        });
        if (!anySupported)
        {
            put(Verdict::Unsupported);
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

        bool targetOk = false;
        const ActorId target = ResolveActionTarget(r, snap, binding, &targetOk);
        if (!targetOk)
        {
            put(Verdict::NoTarget);
            continue;
        }

        std::vector<Verdict> verdicts;
        const bool waiting = Run(r.actions, 0, static_cast<int>(i), target, snap, ctx, decision, verdicts, heldAbove);
        put(Summary(decision, verdicts));
        if (actionTrace)
            (*actionTrace)[i] = verdicts;
        if (decision.Fired() || waiting)
            break;
        // Nothing of this rule could be done: the next gets its turn, in the
        // same tick, exactly as for "no potion in the bag".
    }

    return decision;
}

const char *Explain(Verdict v, ActionKind action) noexcept
{
    switch (v)
    {
    case Verdict::NoResource:
        switch (action)
        {
        case ActionKind::DrinkPotion:
            return "does not carry that potion";
        case ActionKind::CastSpell:
        case ActionKind::EquipSpell:
            return "does not know that spell";
        case ActionKind::EquipWeapon:
            return "does not carry that weapon";
        case ActionKind::EquipArrows:
            return "does not carry those arrows";
        case ActionKind::EquipArmor:
            return "does not carry that armour";
        default:
            return "no potion";
        }

    case Verdict::EffectActive:
        if (IsEquip(action))
            return "already pinned, or nothing of that kind pinned to let go";
        return action == ActionKind::CastSpell ? "that spell is still running" : "previous dose still active";

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
    case Verdict::CannotHold:
        return "cannot be pinned: above the follower's skill, so the AI would never choose it";
    case Verdict::Outranked:
        return "a rule above holds that hand or slot";
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
