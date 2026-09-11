#include "game/Hits.h"

#include "game/Log.h"
#include "game/Util.h"

#include <array>
#include <mutex>
#include <unordered_map>

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

struct Entry
{
    double when{-1.0}; // -1: never hit with this kind
    ft::ActorId attacker{0};
};

// Per target, per kind. Written by the sinks on whatever thread the engine
// sends them from, read by the tick: locked.
std::mutex g_mutex;
std::unordered_map<ft::ActorId, std::array<Entry, static_cast<std::size_t>(DamageKind::COUNT)>> g_hits;

void Note(ft::ActorId target, DamageKind kind, ft::ActorId attacker)
{
    std::scoped_lock lock(g_mutex);
    auto &entry = g_hits[target][static_cast<std::size_t>(kind)];
    entry.when = TacticsSeconds();
    entry.attacker = attacker;
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
        auto *source = RE::TESForm::LookupByID(ev->source);
        if (auto *magic = source ? source->As<RE::MagicItem>() : nullptr)
        {
            for (const auto *effect : magic->effects)
            {
                if (effect && effect->baseEffect && effect->baseEffect->IsDetrimental())
                {
                    Note(target, DamageKind::Magic, attacker);
                    Note(target, KindOfEffect(effect->baseEffect), attacker);
                }
            }
            // A poison's effects say poison by their resist value; a poison
            // with none still is one.
            if (magic->IsPoison())
                Note(target, DamageKind::Poison, attacker);
            return RE::BSEventNotifyControl::kContinue;
        }
        // A weapon, a fist, a trap. One that arrived as a projectile -- an
        // arrow, a bolt -- is ranged; anything else is a blow. A rule can
        // answer the archer, or the one in the follower's face, in
        // particular.
        Note(target, ev->projectile != 0 ? DamageKind::Ranged : DamageKind::Melee, attacker);
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

DamageKind KindOfEffect(const RE::EffectSetting *base)
{
    for (const auto kind : {DamageKind::Fire, DamageKind::Frost, DamageKind::Shock, DamageKind::Poison})
        if (base->data.resistVariable == ResistValueOf(kind))
            return kind;
    return DamageKind::Magic;
}

void WatchHits()
{
    auto *holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder)
    {
        log::hits.event(log::Level::Warn, "install.failed",
                        {{"what", "hit events"}, {"reason", "no script event source holder"}},
                        "no event source holder -- Attacked by will never be true");
        return;
    }
    holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);
    holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_applySink);
    log::hits.info("watching hit and effect-apply events, {} s window", kWindow);
}

Attacked AttackedLately(ft::ActorId target)
{
    Attacked out;
    std::scoped_lock lock(g_mutex);
    const auto it = g_hits.find(target);
    if (it == g_hits.end())
        return out;
    const double now = TacticsSeconds();
    double latest = -1.0;
    for (std::size_t k = 0; k < it->second.size(); ++k)
    {
        const Entry &entry = it->second[k];
        if (entry.when < 0.0 || now - entry.when > kWindow)
            continue;
        out.kinds |= Bit(static_cast<DamageKind>(k));
        if (entry.when > latest)
        {
            latest = entry.when;
            out.attacker = entry.attacker;
        }
    }
    return out;
}

} // namespace ft::game
