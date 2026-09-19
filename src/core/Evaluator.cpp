#include "Evaluator.h"

#include <algorithm>
#include <optional>

namespace ft
{
namespace
{

// A resistance as the rule's number reads it: the game's percent as a
// fraction, so 50 is 0.5 and a weakness is negative.
float ResistFraction(const ActorTraits &t, DamageKind kind)
{
    return t.Resist(kind) / 100.0f;
}

// What a condition reads of an actor: the three bars and the traits. The
// follower's own are the snapshot's; anyone else's are their view's.
struct Facts
{
    const Stat &health;
    const Stat &magicka;
    const Stat &stamina;
    const ActorTraits &traits;
};

Facts FactsOf(const ActorView &v)
{
    return {v.health, v.magicka, v.stamina, v.traits};
}

Facts FactsOf(const Snapshot &s)
{
    return {s.health, s.magicka, s.stamina, s.traits};
}

// The predicate's measure, read off the actor: what the grid's threshold
// is compared with, and what a group's extreme is chosen by. Nothing for
// a predicate off the grid.
std::optional<float> MeasureOf(const Rule &r, const Facts &f)
{
    switch (GridOf(r.predicate).measure)
    {
    case Measure::Health:
        return f.health.Pct();
    case Measure::Magicka:
        return f.magicka.Pct();
    case Measure::Stamina:
        return f.stamina.Pct();
    case Measure::Armor:
        return f.traits.armor;
    case Measure::Resistance:
        return ResistFraction(f.traits, r.damageKind);
    case Measure::None:
        return std::nullopt;
    }
    return std::nullopt;
}

// The predicates asked alike of everyone -- the follower, the player, an
// ally, an enemy: the grid, the hits, a status, the summons, and Any.
// Nothing for a predicate that is one subject's own (the fight's edges,
// the follower's weapons, an enemy's mark); the callers add those.
std::optional<bool> Common(const Rule &r, const Facts &f)
{
    switch (r.predicate)
    {
    case PredicateKind::Any:
        return true;
    case PredicateKind::HitType:
        return f.traits.Using(r.damageKind);
    case PredicateKind::HitBy:
        return f.traits.HitBy(r.damageKind);
    case PredicateKind::Status:
        return f.traits.Has(r.statusKind);
    case PredicateKind::Type:
        return f.traits.Is(r.typeKind);
    case PredicateKind::SummonNone:
        return f.traits.summons == 0;
    case PredicateKind::SummonActive:
        return f.traits.summons > 0;
    default:
        break;
    }
    // The group's extreme: everyone qualifies, the selection binds the
    // one. A threshold: the measure against the rule's number, on the
    // side the predicate names.
    if (IsExtreme(r.predicate))
        return true;
    if (const auto measure = MeasureOf(r, f))
        return IsAbove(r.predicate) ? *measure > r.conditionArg : *measure < r.conditionArg;
    return std::nullopt;
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
    if (r.subjectForm == s.self)
        return s.currentTarget;
    const ActorView *member = s.Ally(MemberId(r));
    return member ? member->target : 0;
}

// Does one member of a group -- an ally, the player among them, or an
// enemy -- satisfy the rule's condition? The common questions, and the
// enemy's own two; IsPredicateValidFor keeps an ally from being asked
// those. The rule's Not is asked here, of each one, so that a negated
// condition holds of a member the plain one does not: "NOT Enemy: Undead"
// is an enemy that is not undead.
bool MemberSatisfies(const ActorView &v, const Rule &r, const Snapshot &s)
{
    bool held = false;
    switch (r.predicate)
    {
    case PredicateKind::Attacking:
        held = v.target != 0 && v.target == MemberId(r);
        break;
    case PredicateKind::AttackedBy: {
        const ActorId target = MemberTarget(r, s);
        held = target != 0 && v.id == target;
        break;
    }
    default:
        held = Common(r, FactsOf(v)).value_or(false);
        break;
    }
    return held != r.negated;
}

// Does the predicate want the MOST of its measure -- an above, or a
// highest -- rather than the least?
bool WantsMost(PredicateKind p)
{
    const Side side = GridOf(p).side;
    return side == Side::Above || side == Side::Highest || p == PredicateKind::LevelHighest;
}

// When several group members match, which one does the rule bind to? By
// the predicate's own measure: asking about low health hands you the most
// hurt one, about high armour the best armoured, about resistance to fire
// the most or least resistant to fire; anything else binds the nearest.
// That rule matters -- it is what makes "enemy below 30% health" mean the
// WEAKEST such enemy rather than an arbitrary one. Negated, the measure
// ranks nobody -- "not below 30%" names no direction -- so the nearest.
bool Better(const Rule &r, const ActorView &candidate, const ActorView &best)
{
    if (r.negated)
        return candidate.distance < best.distance;
    const auto c = MeasureOf(r, FactsOf(candidate));
    const auto b = MeasureOf(r, FactsOf(best));
    if (!c || !b)
        return candidate.distance < best.distance;
    return WantsMost(r.predicate) ? *c > *b : *c < *b;
}

const ActorView *Select(const std::vector<ActorView> &group, const Rule &r, const Snapshot &s)
{
    const ActorView *best = nullptr;
    for (const auto &v : group)
    {
        if (!MemberSatisfies(v, r, s))
            continue;
        if (!best || Better(r, v, *best))
            best = &v;
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

const ActorView *NearestEnemy(const Snapshot &s)
{
    const ActorView *best = nullptr;
    for (const auto &e : s.enemies)
    {
        if (!best || e.distance < best->distance)
            best = &e;
    }
    return best;
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
    return TakesHand(a.kind) ? a.hand : Hand::None;
}

// A "none" action: an equip naming nothing, which lets go of every pin of
// its kind, in the hand it names for a weapon or a spell. An arrow policy
// names nothing in the rule and chooses at evaluation; it is not a none.
bool LetsGo(const Action &a)
{
    return IsEquip(a.kind) && !IsArrowsPolicy(a.kind) && a.form == 0;
}

// A pin of the kind, in those hands when hands are named: a none for the
// right hand is done when no weapon pin holds the right.
bool AnyPinOf(const std::vector<Pin> &pins, Kind kind, Hand hands = Hand::None)
{
    return std::any_of(pins.begin(), pins.end(), [kind, hands](const Pin &p) {
        return p.thing.kind == kind && (hands == Hand::None || Overlap(p.hands, hands));
    });
}

// The arrows a policy takes: of the ammunition carried, the hardest-hitting
// or the weakest by the record's damage, the first of a tie in the bag's
// order. The form, whichever variant: arrows are not tempered or renamed,
// and the loadout's form-level entry is the one the pin goes on.
std::uint32_t ChooseArrows(const std::vector<Holdable> &loadout, bool strongest)
{
    const Holdable *pick = nullptr;
    for (const Holdable &thing : loadout)
    {
        if (!thing.IsAmmo() || thing.variant || thing.count <= 0)
            continue;
        if (!pick || (strongest ? thing.damage > pick->damage : thing.damage < pick->damage))
            pick = &thing;
    }
    return pick ? pick->form : 0;
}

// The follower themself: the common questions, the fight's edges, and
// the weapons in their hands.
Binding EvaluateSelf(const Snapshot &s, const Rule &r)
{
    bool held = false;
    switch (r.predicate)
    {
    case PredicateKind::CombatBegins:
        held = s.combatBegan;
        break;
    case PredicateKind::CombatEnds:
        held = s.combatEnded;
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
        held = Common(r, FactsOf(s)).value_or(false);
        break;
    }
    return held != r.negated ? Match(s.self) : NoMatch();
}

// The player: the ally with their id, asked as any ally is. Not in the
// snapshot -- dead, or not yet loaded -- and nothing about them holds.
Binding EvaluatePlayer(const Snapshot &s, const Rule &r)
{
    const ActorView *player = s.Ally(kPlayerFormID);
    return player && MemberSatisfies(*player, r, s) ? Match(kPlayerFormID) : NoMatch();
}

} // namespace

// Who the condition matches, the rule's Not included.
Binding MatchSubject(const Rule &r, const Snapshot &s)
{
    switch (r.subject)
    {
    case SubjectKind::Self:
        return EvaluateSelf(s, r);

    case SubjectKind::Player:
        return EvaluatePlayer(s, r);

    case SubjectKind::Ally: {
        const auto *a = Select(s.allies, r, s);
        return a ? Match(a->id) : NoMatch();
    }

    case SubjectKind::Follower: {
        // The one named, if they are with us, and only if they are.
        const ActorView *a = s.Ally(r.subjectForm);
        return a && MemberSatisfies(*a, r, s) ? Match(a->id) : NoMatch();
    }

    case SubjectKind::Enemy: {
        const auto *e = Select(s.enemies, r, s);
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

Binding EvaluateCondition(const Rule &r, const Snapshot &s)
{
    // The two guards are about the QUESTION, not its answer: a pair that
    // cannot be asked stays unanswered, and the farewell pass after a fight
    // stays the moment it is. The rule's Not is asked of each actor inside
    // the match (MemberSatisfies, EvaluateSelf), never of these.
    if (!IsPredicateValidFor(r.subject, r.predicate) || !IsDamageKindValidFor(r.predicate, r.damageKind))
        return NoMatch();
    if (s.combatEnded && r.predicate != PredicateKind::CombatEnds)
        return NoMatch();
    return MatchSubject(r, s);
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
        // Whoever last hit the actor the condition bound. That actor is one
        // of the party -- the follower, the player, or an ally -- because
        // IsActionTargetValidFor refuses this target under an enemy or a
        // corpse condition: an enemy's attacker is one of us, and no rule
        // means to aim at that.
        if (!binding.ok)
            return no();
        ActorId attacker = 0;
        if (binding.id == s.self)
            attacker = s.traits.attacker;
        else if (const ActorView *a = s.Ally(binding.id))
            attacker = a->traits.attacker;
        return attacker ? yes(attacker) : no();
    }
    default:
        return no();
    }
}

namespace
{

const std::vector<RunningEffect> kNothingInForce;

// What is in force where a consumable's effects would land: a poison's on
// whoever is struck, the follower's current target being the best guess
// there is; everything else on the follower.
const std::vector<RunningEffect> &InForceFor(ConsumableKind kind, const Snapshot &s)
{
    return kind == ConsumableKind::Poison ? s.targetRunning : s.potions.running;
}

// The thing an action chooses, against what is in force. Asked with
// nothing in force it answers "is there anything in the bag this rule could
// ever use"; asked with what is, "anything worth taking now". The two apart
// are what tell NoResource from EffectActive.
std::uint32_t ChooseAgainst(const Action &a, const Snapshot &snap, const std::vector<RunningEffect> &inForce)
{
    if (IsCharge(a.kind) && a.kind != ActionKind::ChargeSoulGem)
    {
        // The gem for the hand in need, the right before the left.
        const Snapshot::HandWeapon &weapon = snap.rightWeapon.ChargeNeeded() ? snap.rightWeapon : snap.leftWeapon;
        return ChooseSoulGem(snap.soulGems, weapon.Missing(), a.kind == ActionKind::ChargeStrongestSoulGem);
    }
    if (IsArrowsPolicy(a.kind))
        return ChooseArrows(snap.loadout, a.kind == ActionKind::EquipStrongestArrows);
    // An "any" rolls the thing itself; a policy with no effect named rolls
    // the EFFECT and then picks by it, since magnitudes do not compare
    // across effects. Both index with the snapshot's roll, so this answers
    // the same for every call within one evaluation.
    if (IsAny(a.kind))
        return snap.potions.AnyForm(ConsumableOf(a.kind), snap.roll, inForce);
    if (!IsPolicy(a.kind))
        return a.form;
    const ConsumableKind kind = ConsumableOf(a.kind);
    const std::string effect = a.effect.empty() ? snap.potions.AnyEffect(kind, snap.roll, inForce) : a.effect;
    return snap.potions.Choose(kind, effect, IsStrongest(a.kind), inForce);
}

} // namespace

std::uint32_t ChosenForm(const Action &a, const Snapshot &snap)
{
    if (!IsConsume(a.kind) && !IsApply(a.kind))
        return ChooseAgainst(a, snap, kNothingInForce);
    const ConsumableKind kind = ConsumableOf(a.kind);
    const std::uint32_t worthIt = ChooseAgainst(a, snap, InForceFor(kind, snap));
    // A poison already on the target is second best, not useless: the dose
    // lands when a blow does, perhaps after the one in force has worn off,
    // perhaps on someone else. So a poison falls back to the plain choice;
    // a potion drunk for an effect already in force is simply a potion gone,
    // and does not.
    if (worthIt != 0 || kind != ConsumableKind::Poison)
        return worthIt;
    return ChooseAgainst(a, snap, kNothingInForce);
}

namespace
{

// Takes the whole action, not just its kind: a spell action is only
// answerable with the spell in hand, and splitting that across two lookups is
// how the two drift apart.
bool HasResource(const Action &a, const Snapshot &s)
{
    // An action that works out its own thing has what it chooses, or
    // nothing -- asked with nothing in force, so that a bag of buffs all
    // already up reads as "already up" further on, not as an empty bag.
    if (ChoosesForm(a.kind))
        return ChooseAgainst(a, s, kNothingInForce) != 0;
    if (a.kind == ActionKind::ChargeSoulGem)
        return a.form != 0 && std::any_of(s.soulGems.begin(), s.soulGems.end(),
                                          [&](const Snapshot::SoulGemView &g) { return g.form == a.form; });
    if (NamesConsumable(a.kind))
        return a.form != 0 && s.potions.CountOf(a.form, ConsumableOf(a.kind)) > 0;
    // Knowing the spell is the inventory equivalent. Whether the follower
    // can AFFORD to cast it is a separate question and deliberately not
    // asked here: magicka cost depends on perks and skill, which live on
    // the game side. The action reports that back instead. A power and a
    // shout are in the same known list and cost nothing; the menu keeps the
    // three apart by the record's type.
    if (IsCast(a.kind))
        return a.form != 0 && s.spells.Knows(a.form);
    if (IsEquip(a.kind))
    {
        // "None" needs nothing. A named thing must be the follower's, and
        // of the kind the action says: a hand-edited profile could put a
        // spell under equip-weapon, and that is a rule that can never work.
        if (LetsGo(a))
            return true;
        const Holdable *thing = FindHoldable(s.loadout, a.form, a.variant);
        return thing && thing->kind == KindOf(a.kind);
    }
    return true; // Attack and the blows cost nothing from inventory
}

// Is the action already in effect, so that the rule falls through to the
// next: the availability every state-setting action owes (Rule.h). One
// place for every "already done": a dose still running, a buff still up,
// the thing already pinned, every weapon in hand already poisoned, none
// needing a charge, the enemy already the target.
bool EffectAlreadyActive(const Action &a, const Snapshot &s, ActorId target)
{
    // Reported separately from "no potion" because the fix is different:
    // the follower has plenty, and is simply still absorbing the last one.
    // A named consumable could restore anything or nothing; only the
    // per-form cooldown spaces it.
    //
    // Asked of what is in force against what the bag holds, so it is
    // exact: nothing left that would beat an effect already up -- the same
    // dose still working, or every buff carried already on at least as
    // strongly. A stronger bottle over a weaker dose is a gain and is not
    // this. HasResource has already said the bag is not empty.
    if (IsConsume(a.kind) && ChoosesForm(a.kind))
        return ChosenForm(a, s) == 0;
    // A poison goes on a clean weapon; a gem into one that cannot pay for
    // its next hit. None such in hand, and the rule waits, as a buff rule
    // waits on the buff.
    if (IsApply(a.kind))
        return !s.AnyWeaponClean();
    if (IsCharge(a.kind))
        return !s.AnyWeaponChargeNeeded();
    // Already fighting them is the done state.
    if (a.kind == ActionKind::Attack)
        return target != 0 && target == s.currentTarget;
    // The sustained-buff case. Oakflesh runs sixty seconds and no cooldown
    // worth picking is that long, so re-casting can only be stopped by
    // seeing the effect still running. Embrace of Shadows runs three
    // minutes, and a greater power is once a day besides.
    if (IsCast(a.kind))
        return a.form != 0 && s.spells.IsActive(a.form);
    if (IsEquip(a.kind))
    {
        // Availability, the mechanism the note in Rule.h says every
        // state-setting action owes: without it a rule that pins what is
        // already pinned wins every evaluation and starves every rule below
        // it -- first-match-wins makes that a monopoly, not a nuisance.
        // Pinned, not merely equipped: the rule's promise is the pin, and a
        // thing the AI happens to be holding is not yet kept. "None" is done
        // when there is nothing of its kind to let go of.
        if (LetsGo(a))
            return !AnyPinOf(s.pins, KindOf(a.kind), TakesHand(a.kind) ? HandsWanted(a) : Hand::None);
        // An arrow policy is done while the arrows it would choose are the
        // ones pinned: with those gone, the next kind is a new pin.
        const std::uint32_t form = IsArrowsPolicy(a.kind) ? ChosenForm(a, s) : a.form;
        const Pin *pin = FindPin(s.pins, form, a.variant);
        return pin && Covers(pin->hands, HandsWanted(a));
    }
    return false;
}

// The equips' own gates: a thing above the follower's skill, and a hand
// or slot a rule above holds. A pin is a promise the AI will use it; a
// spell above the follower's skill it never would, so the promise cannot
// be kept, and the rule says so rather than equipping something that gets
// swapped straight out.
Verdict EquipAvailability(const Action &a, const Snapshot &snap, const std::vector<Pin> &heldAbove)
{
    const std::uint32_t form = IsArrowsPolicy(a.kind) ? ChosenForm(a, snap) : a.form;
    const Holdable *thing = LetsGo(a) ? nullptr : FindHoldable(snap.loadout, form, a.variant);
    if (thing && thing->unusable)
        return Verdict::AboveSkill;
    const Hand hands = HandsWanted(a);
    const bool outranked = std::any_of(heldAbove.begin(), heldAbove.end(), [&](const Pin &held) {
        return thing ? Conflicts(*thing, hands, held.thing, held.hands) : held.thing.kind == KindOf(a.kind);
    });
    return outranked ? Verdict::Outranked : Verdict::Fired;
}

// The casts' own gates. A cast they cannot pay for is not a cast: the AI
// would decline the package and the rule would have spent its cooldown
// on nothing -- the 12:20 run fired four heals at empty magicka. A spell
// above the follower's skill is not cast either: the package would make
// them cast it regardless; it is refused so that cast and equip agree,
// and the menus offer neither. Whether a dual cast is possible, and what it
// costs, the snapshot has judged. A follower mid-cast on a spell of their own is left to
// finish it: firing our package then interrupts the cast in progress -- a
// Lightning Bolt rule on "magicka above half" cut off every spell the AI
// began -- so the rule waits, as a busy action does: no cooldown
// spent, the next rule gets its turn. (The risk, stated: an AI that never
// stops casting never lets the rule through. If that shows in play, the
// cast cooldown is the next knob, 2 s to 4 s. Our own cast in progress is
// reported Busy before this. A power goes through a package too, and
// would interrupt as well.) The voice recovers between shouts, NPCs
// included; a shout asked for inside that is one the AI will not make, so
// the rule waits; a power's wrapper has a one-second recovery of its own
// and is gated by the same number.
Verdict CastAvailability(const Action &a, const Snapshot &snap)
{
    if (a.kind == ActionKind::CastSpell)
    {
        if (const Holdable *spell = FindHoldable(snap.loadout, a.form); spell && spell->unusable)
            return Verdict::AboveSkill;
        if (a.dual && !snap.spells.CanDualCast(a.form))
            return Verdict::CannotDualCast;
        if (snap.magicka.current < (a.dual ? snap.spells.DualCostOf(a.form) : snap.spells.CostOf(a.form)))
            return Verdict::CannotAfford;
    }
    if (a.kind == ActionKind::UsePower && snap.spells.UsedToday(a.form))
        return Verdict::PowerUsed;
    if (snap.traits.Has(StatusKind::Casting))
        return Verdict::Casting;
    if ((a.kind == ActionKind::Shout || a.kind == ActionKind::UsePower) && snap.voiceRecovery > 0.0f)
        return Verdict::Recovering;
    return Verdict::Fired;
}

// A target is picked from a fight, and only an enemy can be one: aimed at
// the player, an ally, or someone who has died or fled since the hit, the
// rule has no one to point at.
Verdict AttackAvailability(const Snapshot &snap, ActorId target)
{
    if (!snap.inCombat)
        return Verdict::NotInCombat;
    if (target == 0 || !snap.Enemy(target))
        return Verdict::NoTarget;
    return Verdict::Fired;
}

// A blow -- a power attack, a bash, a power bash: in a fight, at one who
// is still an enemy, with something in hand for it, the stamina it costs,
// and within its reach.
Verdict BlowAvailability(const Action &a, const Snapshot &snap, ActorId target)
{
    const Snapshot::Blow &blow = snap.BlowFor(a.kind);
    if (!snap.inCombat)
        return Verdict::NotInCombat;
    // What the player asked to be required of it, before what is in the
    // hands: a perk the follower will never have this fight is the plainer
    // reason.
    if (!blow.perk)
        return Verdict::NoPerk;
    if (!blow.possible)
        return Verdict::NoMeleeWeapon;
    const ActorView *enemy = target != 0 ? snap.Enemy(target) : nullptr;
    if (!enemy)
        return Verdict::NoTarget;
    if (snap.stamina.current < blow.stamina)
        return Verdict::NoStamina;
    if (enemy->reachDistance > blow.reach)
        return Verdict::OutOfReach;
    return Verdict::Fired;
}

// What goes on cooldown when this action fires. The action, its form and
// its target, so that healing the player does not block healing an ally --
// except Attack and the blows, which are keyed by the action alone: the
// point of their cooldown is that the follower is not flicked between two
// enemies on consecutive ticks, and per-target keys would allow exactly
// that.
EvalContext::ActionKey CooldownKey(const Action &a, ActorId target)
{
    if (a.kind == ActionKind::Attack || IsBlow(a.kind))
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
    // A poison goes on a weapon, a gem into an enchanted one: none in hand
    // that takes it, and the rule is not met.
    if (IsApply(a.kind) && !snap.AnyWeaponTakesPoison())
        return Verdict::NothingToPoison;
    if (IsCharge(a.kind) && !snap.AnyWeaponEnchanted())
        return Verdict::NothingToCharge;

    // The family's own gates.
    Verdict gate = Verdict::Fired;
    if (IsEquip(a.kind))
        gate = EquipAvailability(a, snap, heldAbove);
    else if (IsCast(a.kind))
        gate = CastAvailability(a, snap);
    else if (a.kind == ActionKind::Attack)
        gate = AttackAvailability(snap, target);
    else if (IsBlow(a.kind))
        gate = BlowAvailability(a, snap, target);
    if (gate != Verdict::Fired)
        return gate;

    // Already in effect: the rule falls through. Exact where the settle
    // time is a guess -- on a game whose potions restore over time, the
    // previous dose may still have seconds to run -- and for an equip, the
    // pin it holds outranks a conflicting equip beneath it.
    if (EffectAlreadyActive(a, snap, target))
    {
        if (IsEquip(a.kind) && !LetsGo(a))
            if (const Pin *pin = FindPin(snap.pins, a.form, a.variant))
                heldAbove.push_back(*pin);
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
// (A power used today is not transient: the day turns in hours, and a
// list waiting on it would wait the fight out.)

// Do the NEXT action of a list, from `from`, in order: one per tick, like
// nested rules firing on successive ticks. Not as many as can be done at
// once -- ten casts on one tick would want ten UseMagic records and an AI
// that could run them, and a half-second stagger is the cadence of
// everything else here. The action done goes into the decision and onto
// cooldown, and the list waits at the one after it for the next tick.
// One that cannot be done at all is skipped. One that cannot be done YET
// stops the run: it is left for the next tick -- IF the rule has
// `committed`, which it has once any of its actions is done, and which an
// edge rule's list has from the start (the edge holds for one evaluation,
// so a list that yielded on it would be lost). A rule whose very first
// action is only blocked for the moment has otherwise not begun, and
// yields to the rules beneath it, as a single-action rule always did.
// Returns whether the run stopped on a wait.
bool Run(const Rule &rule, std::size_t from, int ruleIndex, ActorId subject, ActorId target, bool committed,
         const Snapshot &snap, EvalContext &ctx, Decision &decision, std::vector<Verdict> &verdicts,
         std::vector<Pin> &heldAbove)
{
    const std::vector<Action> &actions = rule.actions;
    verdicts.assign(actions.size(), Verdict::NotReached);
    for (std::size_t i = from; i < actions.size(); ++i)
    {
        const Action &a = actions[i];
        const Verdict v = Availability(a, snap, ctx, rule.actionTarget, target, heldAbove);
        verdicts[i] = v;
        if (v == Verdict::Fired)
        {
            decision.ruleIndex = ruleIndex;
            decision.rule = rule;
            // The step carries the bottle or gem a policy chose, so the
            // game side has only to consume it.
            Action resolved = a;
            resolved.form = ChosenForm(a, snap);
            decision.step = Decision::Step{resolved, target, subject};
            // The one cooldown there is: the ACTION goes on cooldown for as
            // long as its effect takes to show, and every rule that uses it
            // reports it. Nothing is keyed by rule or by condition.
            ctx.Block(CooldownKey(a, target), snap.now + MinimumCooldown(a.kind));
            // The rest waits for the next tick, or the list is through.
            ctx.pending = i + 1 < actions.size() ? EvalContext::Sequence{ruleIndex, rule, subject, target, i + 1}
                                                 : EvalContext::Sequence{};
            return false;
        }
        if (!Transient(v))
            continue; // cannot be done at all: skipped
        if (committed)
        {
            ctx.pending = {ruleIndex, rule, subject, target, i};
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

// Whether a rule gets as far as its actions this tick, whom its condition
// bound, and against whom it acts.
// Fired here means admitted; anything else is the verdict that stopped it,
// and stands for the rule in the trace.
Verdict Admit(const Rule &r, Moment moment, const Snapshot &snap, const EvalContext &ctx, ActorId &subject,
              ActorId &target)
{
    if (!r.enabled)
        return Verdict::Disabled;
    // Nothing this runtime can do: said before the condition is looked at,
    // because the answer does not depend on it.
    const bool anySupported = std::any_of(r.actions.begin(), r.actions.end(), [&](const Action &a) {
        return a.kind != ActionKind::None && ctx.caps.Supports(a.kind);
    });
    if (!anySupported)
        return Verdict::Unsupported;
    // Reported separately from ConditionFalse on purpose: a pair that can
    // never be answered is an authoring mistake, not a condition that
    // happens to be untrue right now, and the log must not send
    // someone off to investigate a follower's health for nothing.
    // The same of a rule its list has no use for -- an enemy in the idle
    // list -- which a hand-edited profile can hold and the editor never
    // offers.
    if (!IsPredicateValidFor(r.subject, r.predicate) || !IsActionTargetValidFor(r.subject, r.actionTarget) ||
        !IsDamageKindValidFor(r.predicate, r.damageKind) || (r.negated && !CanNegate(r.predicate)) ||
        !IsSubjectValidIn(moment, r.subject) || !IsPredicateValidIn(moment, r.predicate) ||
        (r.predicate == PredicateKind::Status && !IsStatusValidIn(moment, r.statusKind)) ||
        !IsActionTargetValidIn(moment, r.actionTarget))
        return Verdict::InvalidCondition;
    const Binding binding = EvaluateCondition(r, snap);
    if (!binding)
        return Verdict::ConditionFalse;
    subject = r.predicate == PredicateKind::CorpseNone ? 0 : binding.id;
    bool targetOk = false;
    target = ResolveActionTarget(r, snap, binding, &targetOk);
    return targetOk ? Verdict::Fired : Verdict::NoTarget;
}

} // namespace

void RestartCooldown(EvalContext &ctx, const Action &a, ActorId target, double now)
{
    ctx.Block(CooldownKey(a, target), now + MinimumCooldown(a.kind));
}

ActionTrace ProbeAvailability(const RuleSet &rs, const Snapshot &snap, const EvalContext &ctx)
{
    ActionTrace out(rs.rules.size());
    std::vector<Pin> heldAbove;
    for (std::size_t i = 0; i < rs.rules.size(); ++i)
    {
        const Rule &r = rs.rules[i];
        ActorId subject = 0;
        ActorId target = 0;
        // The target only: what the condition says is not the question here.
        static_cast<void>(Admit(r, rs.moment, snap, ctx, subject, target));
        out[i].reserve(r.actions.size());
        for (const Action &a : r.actions)
            out[i].push_back(Availability(a, snap, ctx, r.actionTarget, target, heldAbove));
    }
    return out;
}

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
    const auto put = [&](std::size_t i, Verdict v) {
        if (trace && i < trace->size())
            (*trace)[i] = v;
    };

    Decision decision;

    // The idle list decides nothing in a fight, and a list of its own in
    // progress is dropped with the combat list's on the fight's first edge
    // (below): the tick hands a fight to the combat list, so this is a
    // guard, not a path.
    const bool idle = rs.moment == Moment::Idle;
    if (idle && snap.inCombat)
        return decision;

    // The edge of a fight. What a rule was in the middle of is dropped: the
    // fight it was for is over, or a new one has begun. Then every rule on
    // this edge is looked at first, wherever it sits in the list, and the
    // lists of those that hold are queued in order, one after another, to
    // run one action per tick from here -- the edge holds for this one
    // evaluation, and a rule not begun on it would never be. The rest of
    // the list gets a turn only if the edge rules can do nothing. The
    // edges are the combat list's: the idle list is never handed one.
    const bool edge = !idle && (snap.combatBegan || snap.combatEnded);
    const PredicateKind at = snap.combatBegan ? PredicateKind::CombatBegins : PredicateKind::CombatEnds;
    if (edge)
    {
        ctx.pending = {};
        ctx.queued.clear();
        // A new fight starts with no cooldowns. A cooldown exists to stop a
        // rule thrashing WITHIN a fight; carried into the next one it would
        // silently suppress that fight's first heal. Here, on the edge the
        // tick reports, rather than on a gap in the tick's own clock: a
        // bleedout longer than the gap mid-fight was read as a new fight.
        if (snap.combatBegan)
            ctx.blocked.clear();
        for (std::size_t i = 0; i < rs.rules.size(); ++i)
        {
            const Rule &r = rs.rules[i];
            if (r.predicate != at)
                continue;
            ActorId subject = 0;
            ActorId target = 0;
            const Verdict admitted = Admit(r, rs.moment, snap, ctx, subject, target);
            if (admitted != Verdict::Fired)
            {
                put(i, admitted);
                continue;
            }
            ctx.queued.push_back({static_cast<int>(i), r, subject, target, 0});
        }
    }

    // A list in progress owns the tick while it is doing or waiting, and
    // the lists queued behind it wait their turn. If what remains of one
    // turns out to be nothing it can do -- no potion left, the cast
    // unaffordable -- it is through, and the next queued list, or the
    // rules from the top, get the same tick, as they would have without it.
    for (;;)
    {
        if (!ctx.pending.Active())
        {
            if (ctx.queued.empty())
                break;
            ctx.pending = std::move(ctx.queued.front());
            ctx.queued.erase(ctx.queued.begin());
        }
        const EvalContext::Sequence seq = ctx.pending;
        std::vector<Pin> none;
        std::vector<Verdict> verdicts;
        const bool waiting =
            Run(seq.rule, seq.next, seq.ruleIndex, seq.subject, seq.target, true, snap, ctx, decision, verdicts, none);
        const auto i = static_cast<std::size_t>(seq.ruleIndex);
        put(i, Summary(decision, verdicts));
        if (actionTrace && i < actionTrace->size() && (*actionTrace)[i].size() == verdicts.size())
            (*actionTrace)[i] = verdicts;
        for (const EvalContext::Sequence &later : ctx.queued)
            put(static_cast<std::size_t>(later.ruleIndex), Verdict::Queued);
        if (decision.Fired() || waiting)
            return decision;
    }

    // Out of a fight only the Combat end lists of the combat list run, and
    // they have: the farewell evaluation itself goes on, so the trace can
    // say the standing rules are false on it, but the ticks after it decide
    // nothing. The idle list's standing rules are for exactly those ticks.
    if (!idle && !snap.inCombat && !snap.combatEnded)
        return decision;

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
        // The edge's own rules were looked at above, first; their word
        // stands.
        if (edge && r.predicate == at)
            continue;

        ActorId subject = 0;
        ActorId target = 0;
        const Verdict admitted = Admit(r, rs.moment, snap, ctx, subject, target);
        if (admitted != Verdict::Fired)
        {
            put(i, admitted);
            continue;
        }

        std::vector<Verdict> verdicts;
        const bool waiting =
            Run(r, 0, static_cast<int>(i), subject, target, false, snap, ctx, decision, verdicts, heldAbove);
        put(i, Summary(decision, verdicts));
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
        // An "any" is narrower than the bag: a follower with six health
        // potions and no Fortify carries plenty and still has nothing this
        // rule can drink, so it says which.
        if (IsConsume(action))
            return IsAny(action) ? "carries nothing that buffs" : "none in inventory";
        switch (action)
        {
        case ActionKind::ApplyAny:
            return "carries no poison";
        case ActionKind::CastSpell:
        case ActionKind::EquipSpell:
            return "does not know that spell";
        case ActionKind::UsePower:
            return "does not know that power";
        case ActionKind::Shout:
            return "does not know that shout";
        case ActionKind::UseScroll:
            return "does not carry that scroll";
        case ActionKind::EquipWeapon:
            return "does not carry that weapon";
        case ActionKind::EquipArrows:
            return "does not carry those arrows";
        case ActionKind::EquipStrongestArrows:
        case ActionKind::EquipWeakestArrows:
            return "carries no arrows";
        case ActionKind::EquipArmor:
            return "does not carry that armour";
        default:
            return ToString(v);
        }

    case Verdict::NoTarget:
        if (action == ActionKind::Attack || IsBlow(action))
            return "no enemy to point at";
        return ToString(v);

    case Verdict::NoMeleeWeapon:
        return action == ActionKind::PowerAttack ? "nothing to power attack with" : "nothing to bash with";

    case Verdict::NothingToPoison:
        return "no weapon in hand can be poisoned";

    case Verdict::NothingToCharge:
        return "no enchanted weapon in hand";

    case Verdict::EffectActive:
        if (IsEquip(action))
            return "already pinned, or nothing of that kind pinned to let go";
        if (action == ActionKind::Attack)
            return "already fighting them";
        if (action == ActionKind::UsePower)
            return "that power is still running";
        if (action == ActionKind::Shout)
            return "that shout is still running";
        if (IsApply(action))
            return "every weapon in hand is already poisoned";
        if (IsCharge(action))
            return "no weapon in hand needs a charge";
        // Only Drink reaches here; Apply is answered above. The roll only
        // lands on a buff that would gain something, so reaching this means
        // none carried would.
        if (IsAny(action))
            return "every buff carried is already up";
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
    case Verdict::NotInCombat:
        return "not in a fight";
    case Verdict::NothingToPoison:
        return "no weapon to poison";
    case Verdict::NothingToCharge:
        return "no enchanted weapon";
    case Verdict::CannotAfford:
        return "not enough magicka";
    case Verdict::CannotDualCast:
        return "cannot dual cast that spell";
    case Verdict::NoMeleeWeapon:
        return "nothing in hand for that blow";
    case Verdict::NoPerk:
        return "lacks the perk Settings requires for it";
    case Verdict::NoStamina:
        return "not enough stamina";
    case Verdict::OutOfReach:
        return "out of reach";
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
    case Verdict::PowerUsed:
        return "power already used today";
    case Verdict::InvalidCondition:
        return "invalid condition";
    case Verdict::Queued:
        return "waiting its turn behind the rule above on the same edge";
    case Verdict::NotReached:
        return "not reached";
    }
    return "?";
}

const char *WireName(Verdict v) noexcept
{
    switch (v)
    {
    case Verdict::Fired:
        return "fired";
    case Verdict::Disabled:
        return "disabled";
    case Verdict::ConditionFalse:
        return "condition-false";
    case Verdict::ActionCooldown:
        return "cooldown";
    case Verdict::NoTarget:
        return "no-target";
    case Verdict::NoResource:
        return "no-resource";
    case Verdict::NotInCombat:
        return "not-in-combat";
    case Verdict::NothingToPoison:
        return "nothing-to-poison";
    case Verdict::NothingToCharge:
        return "nothing-to-charge";
    case Verdict::CannotAfford:
        return "cannot-afford";
    case Verdict::CannotDualCast:
        return "cannot-dual-cast";
    case Verdict::NoMeleeWeapon:
        return "no-melee-weapon";
    case Verdict::NoPerk:
        return "no-perk";
    case Verdict::NoStamina:
        return "no-stamina";
    case Verdict::OutOfReach:
        return "out-of-reach";
    case Verdict::EffectActive:
        return "effect-active";
    case Verdict::AboveSkill:
        return "above-skill";
    case Verdict::Outranked:
        return "outranked";
    case Verdict::Unsupported:
        return "unsupported";
    case Verdict::Busy:
        return "busy";
    case Verdict::Casting:
        return "casting";
    case Verdict::Recovering:
        return "recovering";
    case Verdict::PowerUsed:
        return "power-used";
    case Verdict::InvalidCondition:
        return "invalid-condition";
    case Verdict::Queued:
        return "queued";
    case Verdict::NotReached:
        return "not-reached";
    }
    return "?";
}

std::vector<std::size_t> VerdictChanges(Trace &reported, const Trace &now)
{
    if (reported.size() != now.size())
        reported.assign(now.size(), Verdict::NotReached);
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < now.size(); ++i)
    {
        const Verdict v = now[i];
        if (v == Verdict::NotReached || v == reported[i])
            continue;
        reported[i] = v;
        if (v != Verdict::Fired)
            changed.push_back(i);
    }
    return changed;
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
