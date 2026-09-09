#include "Evaluator.h"

#include <algorithm>
#include <optional>

namespace ft
{
namespace
{

// Does one specific enemy satisfy the rule's predicate? Predicates that are not
// answerable about an enemy return false; IsPredicateValidFor rejects those
// pairs before we get here, so this is belt and braces.
// A resistance as the rule's number reads it: the game's percent as a
// fraction, so 50 is 0.5 and a weakness is negative.
float ResistFraction(const ActorTraits &t, DamageKind kind)
{
    return t.Resist(kind) / 100.0f;
}

// The armour and resistance predicates, the same for every subject.
bool ArmourOrResistance(const ActorTraits &t, const Rule &r, bool *held)
{
    switch (r.predicate)
    {
    case PredicateKind::ArmorPctBelow:
        *held = t.armor < r.conditionArg;
        return true;
    case PredicateKind::ArmorPctAbove:
        *held = t.armor > r.conditionArg;
        return true;
    case PredicateKind::ResistancePctBelow:
        *held = ResistFraction(t, r.damageKind) < r.conditionArg;
        return true;
    case PredicateKind::ResistancePctAbove:
        *held = ResistFraction(t, r.damageKind) > r.conditionArg;
        return true;
    case PredicateKind::SummonNone:
        *held = t.summons == 0;
        return true;
    case PredicateKind::SummonActive:
        *held = t.summons > 0;
        return true;
    default:
        return false;
    }
}

// The party member a rule names: 0 is the player, the follower's own id is
// themself, anything else another follower. Their id, and whom they are
// fighting.
ActorId MemberId(const Rule &r)
{
    return r.subjectForm == 0 ? kPlayerFormID : r.subjectForm;
}

ActorId MemberTarget(const Rule &r, const Snapshot &s)
{
    if (r.subjectForm == 0)
        return s.playerTarget;
    if (r.subjectForm == s.self)
        return s.currentTarget;
    for (const auto &a : s.allies)
        if (a.id == r.subjectForm)
            return a.target;
    return 0;
}

bool EnemySatisfies(const EnemyView &e, const Rule &r, const Snapshot &s)
{
    switch (r.predicate)
    {
    case PredicateKind::Targeting:
        return e.attacking != 0 && e.attacking == MemberId(r);
    case PredicateKind::TargetOf: {
        const ActorId target = MemberTarget(r, s);
        return target != 0 && e.id == target;
    }
    case PredicateKind::Any:
        return true;
    case PredicateKind::Using:
        return e.traits.Using(r.damageKind);
    case PredicateKind::AttackedBy:
        return e.traits.AttackedBy(r.damageKind);
    case PredicateKind::HealthPctBelow:
        return e.health.Pct() < r.conditionArg;
    case PredicateKind::HealthPctAbove:
        return e.health.Pct() > r.conditionArg;
    case PredicateKind::MagickaPctBelow:
        return e.magicka.Pct() < r.conditionArg;
    case PredicateKind::MagickaPctAbove:
        return e.magicka.Pct() > r.conditionArg;
    case PredicateKind::StaminaPctBelow:
        return e.stamina.Pct() < r.conditionArg;
    case PredicateKind::StaminaPctAbove:
        return e.stamina.Pct() > r.conditionArg;
    case PredicateKind::Status:
        return e.traits.Has(r.statusKind);
    default: {
        // The group's extreme: everyone qualifies, the selection binds the
        // one. Otherwise armour or a resistance, or nothing.
        if (IsExtreme(r.predicate))
            return true;
        bool held = false;
        return ArmourOrResistance(e.traits, r, &held) && held;
    }
    }
}

bool AllySatisfies(const AllyView &a, const Rule &r)
{
    switch (r.predicate)
    {
    case PredicateKind::Any:
        return true;
    case PredicateKind::Using:
        return a.traits.Using(r.damageKind);
    case PredicateKind::AttackedBy:
        return a.traits.AttackedBy(r.damageKind);
    case PredicateKind::HealthPctBelow:
        return a.health.Pct() < r.conditionArg;
    case PredicateKind::HealthPctAbove:
        return a.health.Pct() > r.conditionArg;
    case PredicateKind::MagickaPctBelow:
        return a.magicka.Pct() < r.conditionArg;
    case PredicateKind::MagickaPctAbove:
        return a.magicka.Pct() > r.conditionArg;
    case PredicateKind::StaminaPctBelow:
        return a.stamina.Pct() < r.conditionArg;
    case PredicateKind::StaminaPctAbove:
        return a.stamina.Pct() > r.conditionArg;
    case PredicateKind::Status:
        return a.traits.Has(r.statusKind);
    default: {
        if (IsExtreme(r.predicate))
            return true;
        bool held = false;
        return ArmourOrResistance(a.traits, r, &held) && held;
    }
    }
}

// When several group members match, which one does the rule bind to? By
// the predicate's own measure: asking about low health hands you the most
// hurt one, about high armour the best armoured, about resistance to fire
// the most or least resistant to fire; anything else binds the nearest.
// That rule matters -- it is what makes "enemy below 30% health" mean the
// WEAKEST such enemy rather than an arbitrary one.
std::optional<float> MeasureOf(const Rule &r, const Stat &health, const Stat &magicka, const Stat &stamina,
                               const ActorTraits &traits)
{
    switch (r.predicate)
    {
    case PredicateKind::HealthPctBelow:
    case PredicateKind::HealthPctAbove:
    case PredicateKind::HealthLowest:
    case PredicateKind::HealthHighest:
        return health.Pct();
    case PredicateKind::MagickaPctBelow:
    case PredicateKind::MagickaPctAbove:
    case PredicateKind::MagickaLowest:
    case PredicateKind::MagickaHighest:
        return magicka.Pct();
    case PredicateKind::StaminaPctBelow:
    case PredicateKind::StaminaPctAbove:
    case PredicateKind::StaminaLowest:
    case PredicateKind::StaminaHighest:
        return stamina.Pct();
    case PredicateKind::ArmorPctBelow:
    case PredicateKind::ArmorPctAbove:
    case PredicateKind::ArmorLowest:
    case PredicateKind::ArmorHighest:
        return traits.armor;
    case PredicateKind::ResistancePctBelow:
    case PredicateKind::ResistancePctAbove:
    case PredicateKind::ResistanceLowest:
    case PredicateKind::ResistanceHighest:
        return ResistFraction(traits, r.damageKind);
    default:
        return std::nullopt;
    }
}

// Does the predicate want the MOST of its measure -- an above, or a
// highest -- rather than the least?
bool WantsMost(PredicateKind p)
{
    switch (p)
    {
    case PredicateKind::HealthHighest:
    case PredicateKind::MagickaHighest:
    case PredicateKind::StaminaHighest:
    case PredicateKind::ArmorHighest:
    case PredicateKind::ResistanceHighest:
    case PredicateKind::LevelHighest:
        return true;
    default:
        return IsAbove(p);
    }
}

// Is the candidate a better binding than the best so far?
bool Better(const Rule &r, std::optional<float> candidate, float candidateDistance, std::optional<float> best,
            float bestDistance)
{
    if (!candidate || !best)
        return candidateDistance < bestDistance;
    return WantsMost(r.predicate) ? *candidate > *best : *candidate < *best;
}

const EnemyView *SelectEnemy(const Snapshot &s, const Rule &r)
{
    const EnemyView *best = nullptr;
    for (const auto &e : s.enemies)
    {
        if (!EnemySatisfies(e, r, s))
            continue;
        if (!best || Better(r, MeasureOf(r, e.health, e.magicka, e.stamina, e.traits), e.distance,
                            MeasureOf(r, best->health, best->magicka, best->stamina, best->traits), best->distance))
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
        if (!best || Better(r, MeasureOf(r, a.health, a.magicka, a.stamina, a.traits), a.distance,
                            MeasureOf(r, best->health, best->magicka, best->stamina, best->traits), best->distance))
            best = &a;
    }
    return best;
}

// The corpses the rule's spell can raise: the first cast action naming a
// spell with a level cap sets the cap; a rule with no such spell -- one
// that conjures, say -- sees every corpse.
int CapFor(const Rule &r, const Snapshot &s)
{
    for (const auto &a : r.actions)
    {
        if (a.kind == ActionKind::CastSpell && a.form != 0)
        {
            if (const int cap = s.spells.CapOf(a.form); cap > 0)
                return cap;
        }
    }
    return 0;
}

bool Raisable(const CorpseView &c, int cap)
{
    return cap == 0 || c.level <= cap;
}

// The corpse the rule binds: the highest or lowest level the spell can
// raise, the nearer of two at the same level.
const CorpseView *SelectCorpse(const Snapshot &s, const Rule &r)
{
    const int cap = CapFor(r, s);
    const CorpseView *best = nullptr;
    for (const auto &c : s.corpses)
    {
        if (!Raisable(c, cap))
            continue;
        if (!best)
        {
            best = &c;
            continue;
        }
        if (c.level == best->level ? c.distance < best->distance
                                   : (WantsMost(r.predicate) ? c.level > best->level : c.level < best->level))
            best = &c;
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
    case PredicateKind::CombatBegins:
        held = s.combatBegan;
        break;
    case PredicateKind::CombatEnds:
        held = s.combatEnded;
        break;
    case PredicateKind::Status:
        held = s.traits.Has(r.statusKind);
        break;
    case PredicateKind::Using:
        held = s.traits.Using(r.damageKind);
        break;
    case PredicateKind::AttackedBy:
        held = s.traits.AttackedBy(r.damageKind);
        break;
    case PredicateKind::WeaponChargeNeeded:
        held = s.AnyWeaponChargeNeeded();
        break;
    case PredicateKind::WeaponPoisonNone:
        held = s.AnyWeaponClean();
        break;
    case PredicateKind::WeaponPoisonActive:
        held = s.AnyWeaponPoisoned();
        break;
    default:
        ArmourOrResistance(s.traits, r, &held);
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
    case PredicateKind::MagickaPctBelow:
        held = s.playerMagicka.Pct() < r.conditionArg;
        break;
    case PredicateKind::MagickaPctAbove:
        held = s.playerMagicka.Pct() > r.conditionArg;
        break;
    case PredicateKind::StaminaPctBelow:
        held = s.playerStamina.Pct() < r.conditionArg;
        break;
    case PredicateKind::StaminaPctAbove:
        held = s.playerStamina.Pct() > r.conditionArg;
        break;
    case PredicateKind::Status:
        held = s.playerTraits.Has(r.statusKind);
        break;
    case PredicateKind::Using:
        held = s.playerTraits.Using(r.damageKind);
        break;
    case PredicateKind::AttackedBy:
        held = s.playerTraits.AttackedBy(r.damageKind);
        break;
    default:
        ArmourOrResistance(s.playerTraits, r, &held);
        break;
    }
    return held ? Match(kPlayerFormID) : NoMatch();
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

    case SubjectKind::Follower: {
        // The one named, if they are with us, and only if they are.
        for (const auto &a : s.allies)
        {
            if (a.id == r.subjectForm)
                return AllySatisfies(a, r) ? Match(a.id) : NoMatch();
        }
        return NoMatch();
    }

    case SubjectKind::Enemy: {
        const auto *e = SelectEnemy(s, r);
        return e ? Match(e->id) : NoMatch();
    }

    case SubjectKind::Corpse: {
        // None: no corpse the spell could raise. Otherwise the one bound.
        const auto *c = SelectCorpse(s, r);
        if (r.predicate == PredicateKind::CorpseNone)
            return c ? NoMatch() : Match(s.self);
        return c ? Match(c->id) : NoMatch();
    }

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
    case ActionTargetKind::Self:
        return yes(s.self);
    case ActionTargetKind::Player:
        return yes(kPlayerFormID);
    case ActionTargetKind::Enemy: {
        // Under an enemy condition, THE enemy it matched. Under any other,
        // whoever the follower is fighting; and with no one, the nearest
        // enemy sensed -- a cast has to go at someone, and nearest is what
        // the follower's own AI would pick.
        if (r.subject == SubjectKind::Enemy)
            return binding.ok ? yes(binding.id) : no();
        if (s.currentTarget != 0)
            return yes(s.currentTarget);
        const auto *nearest = NearestEnemy(s);
        return nearest ? yes(nearest->id) : no();
    }
    case ActionTargetKind::Ally:
    case ActionTargetKind::Corpse:
        // THE ally or corpse the condition matched -- the rule names them
        // once, in the condition. Valid only for a condition about one,
        // which IsActionTargetValidFor holds and Evaluate has checked. A
        // Corpse: None binds the follower, so a cast aimed at "the corpse"
        // under it has no one; the menu does not offer that pairing.
        if (r.actionTarget == ActionTargetKind::Corpse && r.predicate == PredicateKind::CorpseNone)
            return no();
        return binding.ok && IsActionTargetValidFor(r.subject, r.actionTarget) ? yes(binding.id) : no();
    case ActionTargetKind::Follower:
        for (const auto &a : s.allies)
            if (a.id == r.actionTargetForm && a.id != kPlayerFormID)
                return yes(a.id);
        return no();
    case ActionTargetKind::Attacker: {
        // Whoever last hit the actor the condition bound: from that actor's
        // own traits, wherever the snapshot carries them.
        if (!binding.ok)
            return no();
        ActorId attacker = 0;
        if (binding.id == s.self)
            attacker = s.traits.attacker;
        else if (binding.id == kPlayerFormID)
            attacker = s.playerTraits.attacker;
        else if (const auto *e = FindEnemy(s, binding.id))
            attacker = e->traits.attacker;
        else
        {
            for (const auto &a : s.allies)
                if (a.id == binding.id)
                    attacker = a.traits.attacker;
        }
        return attacker ? yes(attacker) : no();
    }
    default:
        return no();
    }
}

std::uint32_t ChosenForm(const Action &a, const PotionStock &stock)
{
    if (!IsPolicy(a.kind))
        return a.form;
    return stock.Choose(ConsumableOf(a.kind), a.effect, IsStrongest(a.kind));
}

// Takes the whole action, not just its kind: a spell action is only
// answerable with the spell in hand, and splitting that across two lookups is
// how the two drift apart.
bool HasResource(const Action &a, const Snapshot &s)
{
    switch (a.kind)
    {
    case ActionKind::DrinkStrongest:
    case ActionKind::DrinkWeakest:
    case ActionKind::EatStrongestFood:
    case ActionKind::EatWeakestFood:
    case ActionKind::EatStrongestIngredient:
    case ActionKind::EatWeakestIngredient:
    case ActionKind::ApplyStrongest:
    case ActionKind::ApplyWeakest:
        return ChosenForm(a, s.potions) != 0;
    case ActionKind::DrinkPotion:
    case ActionKind::EatFood:
    case ActionKind::EatIngredient:
    case ActionKind::ApplyPoison:
        return a.form != 0 && s.potions.CountOf(a.form, ConsumableOf(a.kind)) > 0;
    case ActionKind::ChargeStrongestSoulGem:
    case ActionKind::ChargeWeakestSoulGem:
        return !s.soulGems.empty();
    case ActionKind::ChargeSoulGem:
        return a.form != 0 && std::any_of(s.soulGems.begin(), s.soulGems.end(),
                                          [&](const Snapshot::SoulGemView &g) { return g.form == a.form; });

    case ActionKind::CastSpell:
    case ActionKind::UsePower:
    case ActionKind::Shout:
        // Knowing the spell is the inventory equivalent. Whether she can AFFORD
        // to cast it is a separate question and deliberately not asked here:
        // magicka cost depends on perks and skill, which live on the game side.
        // The action reports that back instead. A power and a shout are in
        // the same known list and cost nothing; the menu keeps the three
        // apart by the record's type.
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
    case ActionKind::DrinkStrongest:
    case ActionKind::DrinkWeakest:
    case ActionKind::EatStrongestFood:
    case ActionKind::EatWeakestFood:
    case ActionKind::EatStrongestIngredient:
    case ActionKind::EatWeakestIngredient:
        return s.potions.IsRunning(a.effect);
    case ActionKind::DrinkPotion:
    case ActionKind::EatFood:
    case ActionKind::EatIngredient:
        // A named consumable could restore anything or nothing; only the
        // per-form cooldown spaces it.
        return false;

    case ActionKind::CastSpell:
    case ActionKind::UsePower:
    case ActionKind::Shout:
        // The sustained-buff case. Oakflesh runs sixty seconds and no cooldown
        // worth picking is that long, so re-casting can only be stopped by
        // seeing the effect still running. Embrace of Shadows runs three
        // minutes, and a greater power is once a day besides.
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

// What goes on cooldown when this action fires. The action, its form and
// its target, so that healing the player does not block healing an ally --
// except Target, which is keyed by the action alone: the point of its
// cooldown is that the follower is not flicked between two enemies on
// consecutive ticks, and per-target keys would allow exactly that.
EvalContext::ActionKey CooldownKey(const Action &a, ActorId target)
{
    if (a.kind == ActionKind::Attack)
        return {a.kind, 0, 0, {}};
    return {a.kind, a.form, target, a.effect};
}

// Can this action be done now? Fired if so; otherwise why not. The equip
// actions add their satisfied pin to `heldAbove`, which is what outranks a
// conflicting equip beneath them.
Verdict Availability(const Action &a, const Snapshot &snap, const EvalContext &ctx, ActionTargetKind aimedAt,
                     ActorId target, std::vector<Pin> &heldAbove)
{
    // An action that makes no sense on its target -- a potion drunk on the
    // player -- is one the menus never offer; from a hand-edited profile it
    // is as unfireable as an action this runtime cannot do.
    if (a.kind == ActionKind::None || !ctx.caps.Supports(a.kind) || !IsActionValidFor(aimedAt, a.kind))
        return Verdict::Unsupported;
    // Only a Reanimate goes at a corpse: the spells with a level cap are
    // exactly those with the Reanimate archetype, which is the record
    // property the engine raises by. The menu offers nothing else there; a
    // hand-edited profile that aims Firebolt at a corpse is as unfireable.
    if (aimedAt == ActionTargetKind::Corpse && a.kind == ActionKind::CastSpell && snap.spells.CapOf(a.form) == 0)
        return Verdict::Unsupported;
    if (ctx.caps.Busy(a.kind))
        return Verdict::Busy;
    if (!HasResource(a, snap))
        return Verdict::NoResource;
    // A poison goes on a weapon: none in hand that takes one, and the rule
    // is not met; one already poisoned, and it waits, as a buff rule waits
    // on the buff.
    if (IsApply(a.kind))
    {
        if (!snap.AnyWeaponTakesPoison())
            return Verdict::NothingToPoison;
        if (!snap.AnyWeaponClean())
            return Verdict::EffectActive;
    }
    // A soul gem goes into an enchanted weapon in hand that cannot pay for
    // its next hit: none enchanted in hand, and the rule is not met; none
    // in need, and it waits.
    if (IsCharge(a.kind))
    {
        if (!snap.AnyWeaponEnchanted())
            return Verdict::NothingToCharge;
        if (!snap.AnyWeaponChargeNeeded())
            return Verdict::EffectActive;
    }

    if (IsEquip(a.kind))
    {
        const Holdable *thing = LetsGo(a) ? nullptr : FindHoldable(snap.loadout, a.form);
        // A pin is a promise the AI will use it. A spell above the
        // follower's skill it never would, so the promise cannot be kept,
        // and the rule says so rather than equipping something that gets
        // swapped straight out.
        if (thing && thing->unusable)
            return Verdict::AboveSkill;
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
        // A spell above the follower's skill is not cast either. The
        // package would make them cast it regardless; it is refused so that
        // cast and equip agree, and the menus offer neither.
        if (a.kind == ActionKind::CastSpell)
        {
            if (const Holdable *spell = FindHoldable(snap.loadout, a.form); spell && spell->unusable)
                return Verdict::AboveSkill;
        }
        if (a.kind == ActionKind::CastSpell && snap.magicka.current < snap.spells.CostOf(a.form))
            return Verdict::CannotAfford;
        // A follower mid-cast on a spell of their own is left to finish it.
        // Firing our package then interrupts the cast in progress -- a
        // Lightning Bolt rule on "magicka above half" cut off every spell
        // the AI began -- so the rule waits, as it does for a busy pool: no
        // cooldown spent, the next rule gets its turn. The risk, stated: an
        // AI that never stops casting never lets the rule through. If that
        // shows in play, the cast cooldown is the next knob (2 s to 4 s).
        // Our own cast in progress is reported Busy above, before this.
        // A power goes through a package too, and would interrupt as well.
        if (IsCast(a.kind) && snap.traits.Has(StatusKind::Casting))
            return Verdict::Casting;
        // The voice recovers between shouts, NPCs included; a shout asked for
        // inside that is one the AI will not make, so the rule waits -- no
        // cooldown spent, the next rule gets its turn. A power's wrapper has
        // a one-second recovery of its own and is gated by the same number.
        if ((a.kind == ActionKind::Shout || a.kind == ActionKind::UsePower) && snap.voiceRecovery > 0.0f)
            return Verdict::Recovering;
        // A target is picked from a fight, and only an enemy can be one:
        // aimed at the player, an ally, or someone who has died or fled
        // since the hit, the rule has no one to point at. Already fighting
        // them is the done state, so the rule falls through -- the
        // availability every state-setting action owes (Rule.h).
        if (a.kind == ActionKind::Attack)
        {
            if (!snap.inCombat)
                return Verdict::NoResource;
            if (target == 0 || !FindEnemy(snap, target))
                return Verdict::NoTarget;
            if (target == snap.currentTarget)
                return Verdict::EffectActive;
        }
        // Exact where the settle time is a guess: on a game whose potions
        // restore over time, the previous dose may still have seconds to run.
        if (EffectAlreadyActive(a, snap))
            return Verdict::EffectActive;
    }

    if (snap.now < ctx.BlockedUntil(CooldownKey(a, target)))
        return Verdict::ActionCooldown;
    return Verdict::Fired;
}

// Cannot be done YET, as opposed to cannot be done: worth waiting for.
bool Transient(Verdict v)
{
    return v == Verdict::Busy || v == Verdict::Casting || v == Verdict::Recovering || v == Verdict::ActionCooldown;
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
bool Run(const std::vector<Action> &actions, std::size_t from, int ruleIndex, ActionTargetKind aimedAt, ActorId target,
         const Snapshot &snap, EvalContext &ctx, Decision &decision, std::vector<Verdict> &verdicts,
         std::vector<Pin> &heldAbove)
{
    verdicts.assign(actions.size(), Verdict::NotReached);
    for (std::size_t i = from; i < actions.size(); ++i)
    {
        const Action &a = actions[i];
        const Verdict v = Availability(a, snap, ctx, aimedAt, target, heldAbove);
        verdicts[i] = v;
        if (v == Verdict::Fired)
        {
            decision.ruleIndex = ruleIndex;
            // The step carries the bottle a policy chose, so the game side
            // has only to consume it.
            Action resolved = a;
            resolved.form = ChosenForm(a, snap.potions);
            decision.steps.push_back({resolved, target});
            // The one cooldown there is: the ACTION goes on cooldown for as
            // long as its effect takes to show, and every rule that uses it
            // reports it. Nothing is keyed by rule or by condition.
            ctx.Block(CooldownKey(a, target), snap.now + MinimumCooldown(a.kind));
            // The rest waits for the next tick, or the list is through.
            ctx.pending = i + 1 < actions.size() ? EvalContext::Sequence{ruleIndex, aimedAt, target, actions, i + 1}
                                                 : EvalContext::Sequence{};
            return false;
        }
        if (!Transient(v))
            continue; // cannot be done at all: skipped
        const bool committed = from > 0;
        if (committed)
        {
            ctx.pending = {ruleIndex, aimedAt, target, actions, i};
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

    // A rule in progress owns the tick while it is doing or waiting. If
    // what remains of its list turns out to be nothing it can do -- no
    // potion left, the cast unaffordable -- the list is through, and the
    // tick goes on to the rules from the top, as it would have without it.
    if (ctx.pending.Active())
    {
        const EvalContext::Sequence seq = ctx.pending;
        std::vector<Pin> none;
        std::vector<Verdict> verdicts;
        const bool waiting =
            Run(seq.actions, seq.next, seq.ruleIndex, seq.aimedAt, seq.target, snap, ctx, decision, verdicts, none);
        const auto i = static_cast<std::size_t>(seq.ruleIndex);
        if (trace && i < trace->size())
            (*trace)[i] = Summary(decision, verdicts);
        if (actionTrace && i < actionTrace->size() && (*actionTrace)[i].size() == verdicts.size())
            (*actionTrace)[i] = verdicts;
        if (decision.Fired() || waiting)
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
        if (!IsPredicateValidFor(r.subject, r.predicate) || !IsActionTargetValidFor(r.subject, r.actionTarget))
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
        const bool waiting =
            Run(r.actions, 0, static_cast<int>(i), r.actionTarget, target, snap, ctx, decision, verdicts, heldAbove);
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
        // The consumables read the way the Consume menu shows them, by count.
        if (IsConsume(action))
            return "none in inventory";
        switch (action)
        {
        case ActionKind::CastSpell:
        case ActionKind::EquipSpell:
            return "does not know that spell";
        case ActionKind::UsePower:
            return "does not know that power";
        case ActionKind::Shout:
            return "does not know that shout";
        case ActionKind::EquipWeapon:
            return "does not carry that weapon";
        case ActionKind::EquipArrows:
            return "does not carry those arrows";
        case ActionKind::EquipArmor:
            return "does not carry that armour";
        case ActionKind::Attack:
            return "not in a fight";
        default:
            return ToString(v);
        }

    case Verdict::NoTarget:
        if (action == ActionKind::Attack)
            return "no enemy to point at";
        return ToString(v);

    case Verdict::NothingToPoison:
        return "no weapon in hand takes a poison";

    case Verdict::NothingToCharge:
        return "no enchanted weapon in hand";

    case Verdict::EffectActive:
        if (IsEquip(action))
            return "already pinned, or nothing of that kind pinned to let go";
        if (action == ActionKind::Attack)
            return "already fighting them";
        if (action == ActionKind::UsePower || action == ActionKind::Shout)
            return "that power is still running";
        if (IsApply(action))
            return "every weapon in hand is already poisoned";
        if (IsCharge(action))
            return "no weapon in hand needs a charge";
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
        return "does not meet condition";
    case Verdict::ActionCooldown:
        return "action used too recently";
    case Verdict::NoTarget:
        return "no target";
    case Verdict::NoResource:
        return "none in inventory";
    case Verdict::NothingToPoison:
        return "no weapon to poison";
    case Verdict::NothingToCharge:
        return "no enchanted weapon";
    case Verdict::CannotAfford:
        return "not enough magicka";
    case Verdict::EffectActive:
        return "previous dose still active";
    case Verdict::AboveSkill:
        return "above the follower's skill";
    case Verdict::Outranked:
        return "a rule above holds that hand or slot";
    case Verdict::Unsupported:
        return "unsupported";
    case Verdict::Busy:
        return "busy, skipped this evaluation";
    case Verdict::Casting:
        return "mid-cast on their own spell, waiting";
    case Verdict::Recovering:
        return "shout on cooldown, waiting";
    case Verdict::InvalidCondition:
        return "invalid condition";
    case Verdict::NotReached:
        return "not reached";
    }
    return "?";
}

std::uint32_t ChooseSoulGem(const std::vector<Snapshot::SoulGemView> &gems, float missing, bool strongest) noexcept
{
    const Snapshot::SoulGemView *smallest = nullptr;
    const Snapshot::SoulGemView *bestFit = nullptr;
    for (const auto &gem : gems)
    {
        if (gem.count <= 0 || gem.charge <= 0.0f)
            continue;
        if (!smallest || gem.charge < smallest->charge)
            smallest = &gem;
        if (gem.charge <= missing && (!bestFit || gem.charge > bestFit->charge))
            bestFit = &gem;
    }
    if (!smallest)
        return 0;
    if (!strongest)
        return smallest->form;
    return bestFit ? bestFit->form : smallest->form;
}

} // namespace ft
