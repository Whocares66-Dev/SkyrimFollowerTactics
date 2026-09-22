#include "progression/game/SpellView.h"

#include "progression/game/Addresses.h"
#include "progression/game/FastIds.h"
#include "progression/game/Forms.h"
#include "progression/game/Log.h"

#include <algorithm>
#include <atomic>
#include <memory>

// Detours needs the Windows API declared first, and asks for it by name.
#include <Windows.h>

#include <detours/detours.h>

namespace fp::game::spellview
{
namespace
{

using Result = RE::BSContainer::ForEachResult;

// At most this many taught spells per companion are handed over: the walk
// notes which it met in one 64-bit mask.
constexpr std::size_t kMaxAdded = 64;

// What the hooks answer for a managed actor. Read on any thread -- the
// combat AI gathers its inventory off the main one -- so immutable once
// published and swapped as one pointer, as the perk view is.
struct View
{
    std::vector<RE::SpellItem *> removed;   // hidden from every walk
    std::vector<RE::SpellItem *> added;     // handed over after the walk
    std::vector<RE::SpellItem *> withdrawn; // no longer known, still in a fight's inventory
};
using Views = std::unordered_map<RE::FormID, View>;

std::atomic<std::shared_ptr<const Views>> g_views;
FastIds g_fast;

// The game thread's own copies, from which the published map is built: the
// ledger's diffs as last published, and the taught spells taken back since
// that a fight may still reach (Withdraw).
std::unordered_map<RE::FormID, Diff> g_diffs;
std::unordered_map<RE::FormID, std::vector<RE::SpellItem *>> g_withdrawn;

using VisitSpellsFn = void (*)(RE::Actor *, RE::Actor::ForEachSpellVisitor *);
using CheckCastFn = bool (*)(RE::Actor *, RE::MagicItem *, bool, RE::MagicSystem::CannotCastReason *);
VisitSpellsFn g_visitSpells = nullptr;
CheckCastFn g_checkCast = nullptr;
bool g_installed = false;
// The combat AI's gathering visitor (GatherSpellsFunctor), told apart from
// our own HasSpell calls by its vtable, so the counter shows the AI.
std::uintptr_t g_gatherVtable = 0;

std::atomic<std::uint64_t> g_visitsManaged{0};
std::atomic<std::uint64_t> g_gathersManaged{0};
std::atomic<std::uint64_t> g_castsRefused{0};
// Set while SelfCheck asks CheckCast itself, so its questions are not
// counted as the engine's.
thread_local bool t_checking = false;

const View *Find(const Views *views, RE::FormID id)
{
    if (!views)
        return nullptr;
    const auto it = views->find(id);
    return it == views->end() ? nullptr : &it->second;
}

bool In(const std::vector<RE::SpellItem *> &list, const RE::MagicItem *item)
{
    return std::any_of(list.begin(), list.end(),
                       [&](const RE::SpellItem *s) { return static_cast<const RE::MagicItem *>(s) == item; });
}

bool Removed(const View &view, const RE::MagicItem *item)
{
    return In(view.removed, item);
}

// Stands in front of the engine's visitor for one walk: a spell set aside is
// passed over, from whichever list it comes -- added, record, race, leveled
// -- and a taught one met on the way is noted, so the walk's end does not
// hand it over twice.
class Filter final : public RE::Actor::ForEachSpellVisitor
{
  public:
    Filter(RE::Actor::ForEachSpellVisitor &inner, const View &view) : inner_(inner), view_(view)
    {
    }

    Result Visit(RE::SpellItem *spell) override
    {
        if (spell && Removed(view_, spell))
            return Result::kContinue;
        for (std::size_t i = 0; i < view_.added.size(); ++i)
            if (view_.added[i] == spell)
                seen_ |= std::uint64_t{1} << i;
        const Result result = inner_.Visit(spell);
        stopped_ = result != Result::kContinue;
        return result;
    }

    [[nodiscard]] bool Stopped() const noexcept
    {
        return stopped_;
    }
    [[nodiscard]] bool Seen(std::size_t i) const noexcept
    {
        return (seen_ >> i & 1) != 0;
    }

  private:
    RE::Actor::ForEachSpellVisitor &inner_;
    const View &view_;
    std::uint64_t seen_{0};
    bool stopped_{false};
};

// Actor::VisitSpells: added, record, race, the record's leveled lists as the
// process resolved them (38781 on AE, 37827 on SE), each to the visitor's
// slot 1 while it answers 1. For a managed companion, the same walk through
// the filter, then what was taught here. The snapshot is held for the whole
// walk, so the filter's view outlives it.
void VisitSpellsHook(RE::Actor *self, RE::Actor::ForEachSpellVisitor *visitor)
{
    if (!self || !visitor || !g_fast.Maybe(self->GetFormID()))
    {
        g_visitSpells(self, visitor);
        return;
    }
    const auto views = g_views.load(std::memory_order_acquire);
    const View *view = Find(views.get(), self->GetFormID());
    if (!view)
    {
        g_visitSpells(self, visitor);
        return;
    }
    g_visitsManaged.fetch_add(1, std::memory_order_relaxed);
    if (*reinterpret_cast<const std::uintptr_t *>(visitor) == g_gatherVtable)
        g_gathersManaged.fetch_add(1, std::memory_order_relaxed);
    Filter filter(*visitor, *view);
    g_visitSpells(self, &filter);
    if (filter.Stopped())
        return;
    for (std::size_t i = 0; i < view->added.size(); ++i)
        if (!filter.Seen(i) && visitor->Visit(view->added[i]) != Result::kContinue)
            return;
}

// Character::CheckCast: asked of each of the actor's casters in turn by the
// original (38758 on AE, 37809 on SE). For a spell a managed companion has
// set aside, or has been taught and lost, no. The reason is left as the
// original leaves it with no caster to ask.
bool CheckCastHook(RE::Actor *self, RE::MagicItem *item, bool dual, RE::MagicSystem::CannotCastReason *reason)
{
    if (self && item && g_fast.Maybe(self->GetFormID()))
    {
        const auto views = g_views.load(std::memory_order_acquire);
        if (const View *view = Find(views.get(), self->GetFormID());
            view && (Removed(*view, item) || In(view->withdrawn, item)))
        {
            if (!t_checking)
                g_castsRefused.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    return g_checkCast(self, item, dual, reason);
}

bool Selected(RE::Actor *actor, const RE::SpellItem *spell)
{
    const auto &data = actor->GetActorRuntimeData();
    return std::any_of(std::begin(data.selectedSpells), std::end(data.selectedSpells),
                       [&](const RE::MagicItem *m) { return m == spell; }) ||
           data.selectedPower == spell;
}

// The ledger's diffs and what has been withdrawn, as one map, published.
void Rebuild()
{
    Views views;
    for (const auto &[id, diff] : g_diffs)
        views[id] = View{diff.removed, diff.added, {}};
    for (const auto &[id, spells] : g_withdrawn)
    {
        View &view = views[id];
        // Taught again since: known, not withdrawn.
        for (RE::SpellItem *spell : spells)
            if (!In(view.added, spell))
                view.withdrawn.push_back(spell);
    }
    g_fast.Set(views);
    g_views.store(std::make_shared<const Views>(std::move(views)), std::memory_order_release);
}

// A spell the actor is not to use: out of their hands and voice, and out of
// a UseMagic package's choice, which the procedure keeps when nothing else
// qualifies (29346 reads it back whatever 38727 found).
void Disarm(RE::Actor *actor, RE::SpellItem *spell, std::string_view why)
{
    if (Selected(actor, spell))
    {
        actor->DeselectSpell(spell);
        log::spells.debug("{}: {} {}, taken out of hand", NameOf(actor), NameOf(spell), why);
    }
    auto *process = actor->GetActorRuntimeData().currentProcess;
    if (process && process->middleHigh && process->middleHigh->currentPackageSpell == spell)
    {
        process->middleHigh->currentPackageSpell = nullptr;
        log::spells.debug("{}: {} {}, dropped from the package's choice", NameOf(actor), NameOf(spell), why);
    }
}

} // namespace

void Install()
{
    if (REL::Module::IsVR())
    {
        log::spells.warn("Skyrim VR: the spell hooks are not installed; spells are taught onto the actor, and a "
                         "companion's own cannot be set aside");
        return;
    }
    g_views.store(std::make_shared<const Views>());
    g_gatherVtable = REL::Relocation<std::uintptr_t>{RE::VTABLE_GatherSpellsFunctor[0]}.address();

    // VisitSpells first: without it there is nothing for CheckCast to agree
    // with, so a failure leaves both alone. Installed at data load, before
    // anything walks an actor's spells: Detours writes the jump a moment
    // before it hands back the original, as it does for Follower Tactics.
    const REL::Relocation<std::uintptr_t> target{addr::kVisitSpells};
    g_visitSpells = reinterpret_cast<VisitSpellsFn>(target.address());
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID &>(g_visitSpells), reinterpret_cast<PVOID>(&VisitSpellsHook));
    if (const LONG result = DetourTransactionCommit(); result != NO_ERROR)
    {
        log::spells.error("could not detour VisitSpells (Detours error {}): spells are taught onto the actor, and a "
                          "companion's own cannot be set aside",
                          result);
        g_visitSpells = nullptr;
        return;
    }

    REL::Relocation<std::uintptr_t> table{RE::VTABLE_Character[0]};
    g_checkCast = reinterpret_cast<CheckCastFn>(
        *reinterpret_cast<std::uintptr_t *>(table.address() + addr::kCheckCastSlot * sizeof(std::uintptr_t)));
    table.write_vfunc(addr::kCheckCastSlot, &CheckCastHook);
    g_installed = true;
    log::spells.info("VisitSpells detoured and Character's CheckCast replaced: companions' spells are a view over "
                     "what the engine gives them");
}

bool Installed() noexcept
{
    return g_installed;
}

void Publish(std::unordered_map<RE::FormID, Diff> diffs)
{
    g_diffs.clear();
    for (auto &[id, diff] : diffs)
    {
        std::erase(diff.removed, nullptr);
        std::erase(diff.added, nullptr);
        // Set aside and taught at once is a ledger the core refuses to make;
        // from a damaged co-save, set aside wins.
        std::erase_if(diff.added, [&](RE::SpellItem *s) { return In(diff.removed, s); });
        if (diff.added.size() > kMaxAdded)
        {
            log::spells.warn("{:08X}: {} spells taught; the first {} are known", id, diff.added.size(), kMaxAdded);
            diff.added.resize(kMaxAdded);
        }
        if (!diff.removed.empty() || !diff.added.empty())
            g_diffs.emplace(id, std::move(diff));
    }
    Rebuild();
}

void Withdraw(RE::Actor *actor, RE::SpellItem *spell)
{
    if (!actor || !spell || !g_installed)
        return;
    auto &list = g_withdrawn[actor->GetFormID()];
    if (std::find(list.begin(), list.end(), spell) == list.end())
        list.push_back(spell);
    Rebuild();
    Disarm(actor, spell, "no longer known");
}

void Reconcile(RE::Actor *actor)
{
    if (!actor || !g_installed)
        return;
    const auto views = g_views.load(std::memory_order_acquire);
    const View *view = Find(views.get(), actor->GetFormID());
    if (!view)
        return;
    for (RE::SpellItem *spell : view->removed)
        Disarm(actor, spell, "set aside");
    for (RE::SpellItem *spell : view->withdrawn)
        Disarm(actor, spell, "no longer known");
    // Out of the fight, the inventory that listed a withdrawn spell is gone:
    // the next one is gathered through the view.
    if (!view->withdrawn.empty() && !actor->IsInCombat())
    {
        g_withdrawn.erase(actor->GetFormID());
        Rebuild();
    }
}

void Forget()
{
    g_diffs.clear();
    g_withdrawn.clear();
    g_fast.Clear();
    g_views.store(std::make_shared<const Views>(), std::memory_order_release);
}

Counters Count() noexcept
{
    return {g_visitsManaged.load(std::memory_order_relaxed), g_gathersManaged.load(std::memory_order_relaxed),
            g_castsRefused.load(std::memory_order_relaxed)};
}

std::string SelfCheck(RE::Actor *actor)
{
    if (!actor)
        return "no actor";
    if (!g_installed)
        return fmt::format("{}: the spell hooks are not installed", NameOf(actor));
    const auto views = g_views.load(std::memory_order_acquire);
    const View *view = Find(views.get(), actor->GetFormID());
    std::vector<RE::SpellItem *> asked;
    if (auto *npc = actor->GetActorBase())
        if (auto *list = npc->GetSpellList(); list && list->spells)
            for (std::uint32_t i = 0; i < list->numSpells; ++i)
                if (RE::SpellItem *spell = list->spells[i];
                    spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell)
                    asked.push_back(spell);
    if (view)
        for (RE::SpellItem *spell : view->added)
            if (std::find(asked.begin(), asked.end(), spell) == asked.end())
                asked.push_back(spell);
    std::size_t agree = 0;
    std::string wrong;
    for (RE::SpellItem *spell : asked)
    {
        const bool expected = !(view && Removed(*view, spell));
        const bool engine = actor->HasSpell(spell);
        if (expected == engine)
            ++agree;
        else
            wrong += fmt::format("{}{} ({} expected, engine says {})", wrong.empty() ? "" : "; ", NameOf(spell),
                                 expected ? "known" : "not known", engine ? "known" : "not known");
        if (!expected)
        {
            RE::MagicSystem::CannotCastReason reason{};
            t_checking = true;
            const bool allowed = actor->CheckCast(spell, false, &reason);
            t_checking = false;
            if (allowed)
                wrong +=
                    fmt::format("{}{} set aside, but CheckCast allows it", wrong.empty() ? "" : "; ", NameOf(spell));
        }
    }
    return fmt::format("{}: {} of {} spells as the view has them{}{}", NameOf(actor), agree, asked.size(),
                       view ? "" : " (no view: nothing set aside or taught)",
                       wrong.empty() ? std::string() : ": " + wrong);
}

} // namespace fp::game::spellview
