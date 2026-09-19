#include "game/Hits.h"

#include "core/HitKinds.h"
#include "game/Log.h"
#include "game/Util.h"

#include <mutex>

namespace ft::game
{
namespace
{

// How long a hit counts as "being attacked". Long enough that a 500 ms
// tick sees every blow, short enough that a fight that has moved on is
// not still "under fire". On the tactics clock, game time, like every
// other timer here: on the wall clock a hit aged out while the panel held
// the world still.
constexpr double kWindow = 3.0;

// Written by the sinks on whatever thread the engine sends them from, read
// by the tick: locked.
std::mutex g_mutex;
ft::HitTable g_hits;

} // namespace

ft::Resist ResistOf(const RE::EffectSetting *base);

namespace
{

void Note(ft::ActorId target, DamageKind kind, ft::ActorId attacker)
{
    const double now = TacticsSeconds();
    std::scoped_lock lock(g_mutex);
    g_hits.Note(target, kind, attacker, now);
}

ft::ActorId IdOf(const RE::NiPointer<RE::TESObjectREFR> &ref)
{
    return ref ? ref->GetFormID() : 0;
}

// A blow or a spell landing. The source is the weapon or the magic item;
// a magic item is noted once per effect it carries, so a fire-and-frost
// spell is both.
class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
{
    RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent *ev, RE::BSTEventSource<RE::TESHitEvent> *) override
    {
        if (!ev || !ev->target)
            return RE::BSEventNotifyControl::kContinue;
        const ft::ActorId target = IdOf(ev->target);
        const ft::ActorId attacker = IdOf(ev->cause);
        // What the hit counts as is core's (core/HitKinds.h, tested); this
        // reads the event.
        ft::HitSeen hit;
        hit.projectile = ev->projectile != 0;
        auto *source = RE::TESForm::LookupByID(ev->source);
        if (auto *magic = source ? source->As<RE::MagicItem>() : nullptr)
        {
            hit.magic = true;
            for (const auto *effect : ResolvedEffects(*magic))
                if (effect->baseEffect->IsDetrimental())
                    hit.detrimental.push_back(ResistOf(effect->baseEffect));
            hit.poison = magic->IsPoison();
        }
        for (const DamageKind kind : ft::KindsOfHit(hit))
            Note(target, kind, attacker);
        return RE::BSEventNotifyControl::kContinue;
    }
};

// An effect taking hold, hit event or none: a cloak's burn, a hazard's
// frost, a poisoned blade's poison.
class ApplySink : public RE::BSTEventSink<RE::TESMagicEffectApplyEvent>
{
    RE::BSEventNotifyControl ProcessEvent(const RE::TESMagicEffectApplyEvent *ev,
                                          RE::BSTEventSource<RE::TESMagicEffectApplyEvent> *) override
    {
        if (!ev || !ev->target)
            return RE::BSEventNotifyControl::kContinue;
        auto *base = RE::TESForm::LookupByID<RE::EffectSetting>(ev->magicEffect);
        if (!base || !base->IsDetrimental())
            return RE::BSEventNotifyControl::kContinue;
        Note(IdOf(ev->target), DamageKind::Magic, IdOf(ev->caster));
        Note(IdOf(ev->target), KindOfEffect(base), IdOf(ev->caster));
        return RE::BSEventNotifyControl::kContinue;
    }
};

HitSink g_hitSink;
ApplySink g_applySink;

} // namespace

// By what resists it -- the same reading as the Status condition's
// burning, frostbitten and shocked -- else magic. Every hit's effect is
// noted as Magic as well, so "attacked by magic" is any spell and
// "attacked by fire" the fire in particular. Melee and Ranged are the
// weapon's, never an effect's.
RE::ActorValue ResistValueOf(DamageKind kind)
{
    switch (kind)
    {
    case DamageKind::Magic:
        return RE::ActorValue::kResistMagic;
    case DamageKind::Fire:
        return RE::ActorValue::kResistFire;
    case DamageKind::Frost:
        return RE::ActorValue::kResistFrost;
    case DamageKind::Shock:
        return RE::ActorValue::kResistShock;
    case DamageKind::Poison:
        return RE::ActorValue::kPoisonResist;
    default:
        return RE::ActorValue::kNone;
    }
}

ft::Resist ResistOf(const RE::EffectSetting *base)
{
    switch (base->data.resistVariable)
    {
    case RE::ActorValue::kResistFire:
        return ft::Resist::Fire;
    case RE::ActorValue::kResistFrost:
        return ft::Resist::Frost;
    case RE::ActorValue::kResistShock:
        return ft::Resist::Shock;
    case RE::ActorValue::kPoisonResist:
        return ft::Resist::Poison;
    default:
        return ft::Resist::Other;
    }
}

DamageKind KindOfEffect(const RE::EffectSetting *base)
{
    return ft::KindOfResist(ResistOf(base));
}

void WatchHits()
{
    auto *holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder)
    {
        log::hits.warn("no event source holder -- Attacked by will never be true");
        return;
    }
    holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);
    holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_applySink);
    log::hits.info("watching hit and effect-apply events, {} s window", kWindow);
}

Attacked AttackedLately(ft::ActorId target)
{
    const double now = TacticsSeconds();
    std::scoped_lock lock(g_mutex);
    return g_hits.Lately(target, now, kWindow);
}

} // namespace ft::game
