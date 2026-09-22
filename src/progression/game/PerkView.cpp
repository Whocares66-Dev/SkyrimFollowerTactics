#include "progression/game/PerkView.h"

#include "game/Addresses.h"
#include "progression/game/FastIds.h"
#include "progression/game/Forms.h"
#include "progression/game/Log.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

namespace fp::game::perkview
{
namespace
{

// What ForEachPerk visits for a managed actor. Read on any thread -- the
// engine asks HasPerk from combat and animation jobs -- so the whole map is
// immutable once published and swapped as one pointer.
struct View
{
    std::vector<RE::BGSPerk *> removed;
    std::vector<RE::PerkRankData *> added; // stable: from the pool below
};
using Views = std::unordered_map<RE::FormID, View>;

std::atomic<std::shared_ptr<const Views>> g_views;

// Which actors have a view: checked before the pointer above is loaded,
// since every NPC's HasPerk passes through the hook (progression/game/FastIds.h).
FastIds g_fast;

bool MaybeManaged(RE::FormID id) noexcept
{
    return g_fast.Maybe(id);
}

// One {perk, rank 1} per added perk, made once and never freed. A visitor
// may keep the pointer it was handed past the walk -- the engine's HasPerk
// finder reads the rank through it after ForEachPerk returns -- so what we
// hand out has to outlive any walk. The same entry serves every actor.
std::mutex g_poolMutex;
std::unordered_map<RE::BGSPerk *, RE::PerkRankData *> g_pool;

RE::PerkRankData *Pooled(RE::BGSPerk *perk)
{
    std::scoped_lock lock(g_poolMutex);
    auto &slot = g_pool[perk];
    if (!slot)
        slot = new RE::PerkRankData(perk, 1);
    return slot;
}

// What has been registered on each managed actor's process: set when our
// ApplyPerksFromBase builds it, moved by Reconcile. An actor absent from it
// has what the engine's own ApplyPerksFromBase gave it -- the base record.
// The lock is never held across a call into the engine.
using Ranks = std::vector<std::pair<RE::BGSPerk *, std::uint8_t>>;
std::mutex g_appliedMutex;
std::unordered_map<RE::FormID, Ranks> g_applied;
std::atomic<bool> g_anyApplied{false};

using ForEachPerkFn = void (*)(RE::Actor *, void *);
using ApplyPerksFromBaseFn = void (*)(RE::Actor *);
ForEachPerkFn g_forEachPerk = nullptr;
ApplyPerksFromBaseFn g_applyPerksFromBase = nullptr;

const View *Find(const Views *views, RE::FormID id)
{
    if (!views)
        return nullptr;
    const auto it = views->find(id);
    return it == views->end() ? nullptr : &it->second;
}

bool Removed(const View &view, const RE::BGSPerk *perk)
{
    return std::find(view.removed.begin(), view.removed.end(), perk) != view.removed.end();
}

// The engine's own queued rank change (task 0x5F). Its handler (23822 on AE)
// removes every entry of the old rank and applies every entry of the new
// one, so 0 -> rank puts a perk on and rank -> 0 takes it off, both in the
// queue's order behind anything ApplyPerksFromBase queued. ApplyPerksFromBase
// and the player's AddPerk call the same (dev/ENGINE_PERKS.md).
void QueueRankChange(RE::Actor *actor, RE::BGSPerk *perk, std::uint8_t from, std::uint8_t to)
{
    using Fn = void (*)(RE::TaskQueueInterface *, RE::Actor *, RE::BGSPerk *, std::uint8_t, std::uint8_t);
    static REL::Relocation<Fn> queue{addr::kQueuePerkRankChange};
    auto *tasks = RE::TaskQueueInterface::GetSingleton();
    if (!tasks || !actor || !perk || from == to)
        return;
    queue(tasks, actor, perk, from, to);
}

Ranks RecordRanks(RE::Actor *actor, const View *view)
{
    Ranks out;
    auto *npc = actor ? actor->GetActorBase() : nullptr;
    if (npc && npc->perks)
        for (std::uint32_t i = 0; i < npc->perkCount; ++i)
        {
            const RE::PerkRankData &d = npc->perks[i];
            if (d.perk && !(view && Removed(*view, d.perk)))
                out.emplace_back(d.perk, static_cast<std::uint8_t>(d.currentRank));
        }
    return out;
}

Ranks EffectiveRanks(RE::Actor *actor, const View *view)
{
    Ranks out = RecordRanks(actor, view);
    if (view)
        for (const RE::PerkRankData *d : view->added)
            if (std::none_of(out.begin(), out.end(), [&](const auto &r) { return r.first == d->perk; }))
                out.emplace_back(d->perk, static_cast<std::uint8_t>(d->currentRank));
    return out;
}

bool HasProcess(RE::Actor *actor)
{
    auto *process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
    return process && process->middleHigh;
}

// Queue what takes an actor's registered perks from `current` to `desired`.
// Worked out by the caller under g_appliedMutex; queued here, outside it.
void Move(RE::Actor *actor, const Ranks &current, const Ranks &desired)
{
    const auto find = [](const Ranks &in, RE::BGSPerk *perk) {
        return std::find_if(in.begin(), in.end(), [&](const auto &r) { return r.first == perk; });
    };
    std::size_t on = 0;
    std::size_t off = 0;
    for (const auto &[perk, rank] : desired)
        if (const auto it = find(current, perk); it == current.end() || it->second != rank)
        {
            QueueRankChange(actor, perk, it == current.end() ? 0 : it->second, rank);
            ++on;
        }
    for (const auto &[perk, rank] : current)
        if (find(desired, perk) == desired.end())
        {
            QueueRankChange(actor, perk, rank, 0);
            ++off;
        }
    if (on + off > 0)
        log::perks.debug("{}: {} perk(s) queued on, {} off", NameOf(actor), on, off);
}

// Character::ForEachPerk: the record's perks, for everyone but a managed
// companion; for them the record's less the set-aside, then the added. The
// visitor's first slot takes a PerkRankData and answers 1 to go on, 0 to
// stop, as the engine's own walk (ID 14320 on AE) reads it.
void ForEachPerkHook(RE::Actor *self, void *visitor)
{
    if (!self || !MaybeManaged(self->GetFormID()))
    {
        g_forEachPerk(self, visitor);
        return;
    }
    const auto views = g_views.load(std::memory_order_acquire);
    const View *view = Find(views.get(), self->GetFormID());
    if (!view)
    {
        g_forEachPerk(self, visitor);
        return;
    }
    using Visit = std::uint32_t (*)(void *, RE::PerkRankData *);
    const Visit visit = (*static_cast<Visit **>(visitor))[0];
    auto *npc = self->GetActorBase();
    if (npc && npc->perks)
        for (std::uint32_t i = 0; i < npc->perkCount; ++i)
        {
            RE::PerkRankData *d = &npc->perks[i];
            if (!d->perk || Removed(*view, d->perk))
                continue;
            if (visit(visitor, d) != 1)
                return;
        }
    for (RE::PerkRankData *d : view->added)
        if (visit(visitor, d) != 1)
            return;
}

// Character::ApplyPerksFromBase: when an actor's process is built, the
// engine queues each record perk's rank change (0 -> rank). For a managed
// companion the same, over the view.
void ApplyPerksFromBaseHook(RE::Actor *self)
{
    const bool maybe = self && MaybeManaged(self->GetFormID());
    const auto views = maybe ? g_views.load(std::memory_order_acquire) : nullptr;
    const View *view = maybe ? Find(views.get(), self->GetFormID()) : nullptr;
    if (!view)
    {
        if (self && g_anyApplied.load(std::memory_order_relaxed))
        {
            // Built as the record has it: what we remembered is stale.
            std::scoped_lock lock(g_appliedMutex);
            g_applied.erase(self->GetFormID());
        }
        g_applyPerksFromBase(self);
        return;
    }
    if (!HasProcess(self))
        return; // the engine's own check: nothing is registered without one
    const Ranks ranks = EffectiveRanks(self, view);
    {
        std::scoped_lock lock(g_appliedMutex);
        g_applied[self->GetFormID()] = ranks;
        g_anyApplied.store(true, std::memory_order_relaxed);
    }
    for (const auto &[perk, rank] : ranks)
        QueueRankChange(self, perk, 0, rank);
}

} // namespace

void Install()
{
    if (REL::Module::IsVR())
    {
        log::perks.warn("Skyrim VR: the perk hooks are not installed (their slots were read on SE and AE only); "
                        "companions keep their records' perks");
        return;
    }
    g_views.store(std::make_shared<const Views>());
    REL::Relocation<std::uintptr_t> table{RE::VTABLE_Character[0]};
    const auto slot = [&](std::size_t index) {
        return *reinterpret_cast<std::uintptr_t *>(table.address() + index * sizeof(std::uintptr_t));
    };
    // The originals first, so a call landing between reading and writing a
    // slot finds one (Follower Tactics' Pins.cpp does the same).
    g_forEachPerk = reinterpret_cast<ForEachPerkFn>(slot(addr::kForEachPerkSlot));
    g_applyPerksFromBase = reinterpret_cast<ApplyPerksFromBaseFn>(slot(addr::kApplyPerksFromBaseSlot));
    table.write_vfunc(addr::kForEachPerkSlot, &ForEachPerkHook);
    table.write_vfunc(addr::kApplyPerksFromBaseSlot, &ApplyPerksFromBaseHook);
    log::perks.info("Character's ForEachPerk and ApplyPerksFromBase replaced: companions' perks are a view over their "
                    "records, which are left alone");
}

bool Installed() noexcept
{
    return g_forEachPerk != nullptr;
}

void Publish(std::unordered_map<RE::FormID, Diff> diffs)
{
    Views views;
    for (auto &[id, diff] : diffs)
    {
        if (diff.removed.empty() && diff.added.empty())
            continue; // as the record has it: nothing to answer differently
        View view;
        view.removed = std::move(diff.removed);
        for (RE::BGSPerk *perk : diff.added)
            if (perk)
                view.added.push_back(Pooled(perk));
        views.emplace(id, std::move(view));
    }
    g_fast.Set(views);
    g_views.store(std::make_shared<const Views>(std::move(views)), std::memory_order_release);
}

void Reconcile(RE::Actor *actor)
{
    if (!HasProcess(actor))
        return;
    const auto views = g_views.load(std::memory_order_acquire);
    const View *view = Find(views.get(), actor->GetFormID());
    const Ranks desired = EffectiveRanks(actor, view);
    Ranks current;
    {
        std::scoped_lock lock(g_appliedMutex);
        const auto it = g_applied.find(actor->GetFormID());
        if (it == g_applied.end() && !view)
            return; // as the record has it, and registered that way
        current = it != g_applied.end() ? std::move(it->second) : RecordRanks(actor, nullptr);
        if (view)
        {
            g_applied[actor->GetFormID()] = desired;
            g_anyApplied.store(true, std::memory_order_relaxed);
        }
        else
            g_applied.erase(actor->GetFormID());
    }
    Move(actor, current, desired);
}

void Forget()
{
    g_fast.Clear();
    g_views.store(std::make_shared<const Views>(), std::memory_order_release);
    std::scoped_lock lock(g_appliedMutex);
    g_applied.clear();
}

std::vector<RE::BGSPerk *> Effective(RE::Actor *actor)
{
    const auto views = g_views.load(std::memory_order_acquire);
    std::vector<RE::BGSPerk *> out;
    for (const auto &[perk, rank] : EffectiveRanks(actor, Find(views.get(), actor ? actor->GetFormID() : 0)))
        out.push_back(perk);
    return out;
}

} // namespace fp::game::perkview
