#include "game/Hits.h"

#include "game/Util.h"

#include <array>
#include <chrono>
#include <mutex>
#include <unordered_map>

namespace ft::game
{
namespace
{

using Clock = std::chrono::steady_clock;

// How long a hit counts as "being attacked". Long enough that a 500 ms
// tick sees every blow, short enough that a fight that has moved on is
// not still "under fire".
constexpr auto kWindow = std::chrono::seconds(3);

struct Entry
{
    Clock::time_point when{};
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
    entry.when = Clock::now();
    entry.attacker = attacker;
}

// The kind of damage a magic effect does, by what resists it -- the same
// reading as the Status condition's burning, frostbitten and shocked --
// else magic. Physical is the weapon's, never an effect's.
DamageKind KindOfEffect(const RE::EffectSetting *base)
{
    switch (base->data.resistVariable)
    {
    case RE::ActorValue::kResistFire:
        return DamageKind::Fire;
    case RE::ActorValue::kResistFrost:
        return DamageKind::Frost;
    case RE::ActorValue::kResistShock:
        return DamageKind::Shock;
    case RE::ActorValue::kPoisonResist:
        return DamageKind::Poison;
    case RE::ActorValue::kResistDisease:
        return DamageKind::Disease;
    default:
        return DamageKind::Magic;
    }
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
                    Note(target, KindOfEffect(effect->baseEffect), attacker);
            }
            // A poison's effects say poison by their resist value; a poison
            // with none still is one.
            if (magic->IsPoison())
                Note(target, DamageKind::Poison, attacker);
            return RE::BSEventNotifyControl::kContinue;
        }
        // A weapon, a fist, a trap: physical.
        Note(target, DamageKind::Physical, attacker);
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
        Note(IdOf(ev->target), KindOfEffect(base), IdOf(ev->caster));
        return RE::BSEventNotifyControl::kContinue;
    }
};

HitSink g_hitSink;
ApplySink g_applySink;

} // namespace

void WatchHits()
{
    auto *holder = RE::ScriptEventSourceHolder::GetSingleton();
    if (!holder)
    {
        logger::warn("hits: no event source holder -- Attacked by will never be true");
        return;
    }
    holder->AddEventSink<RE::TESHitEvent>(&g_hitSink);
    holder->AddEventSink<RE::TESMagicEffectApplyEvent>(&g_applySink);
    logger::info("hits: watching hit and effect-apply events, {} s window", kWindow.count());
}

Attacked AttackedLately(ft::ActorId target)
{
    Attacked out;
    std::scoped_lock lock(g_mutex);
    const auto it = g_hits.find(target);
    if (it == g_hits.end())
        return out;
    const auto now = Clock::now();
    Clock::time_point latest{};
    for (std::size_t k = 0; k < it->second.size(); ++k)
    {
        const Entry &entry = it->second[k];
        if (entry.when == Clock::time_point{} || now - entry.when > kWindow)
            continue;
        out.kinds |= static_cast<std::uint8_t>(1u << k);
        if (entry.when > latest)
        {
            latest = entry.when;
            out.attacker = entry.attacker;
        }
    }
    return out;
}

} // namespace ft::game
