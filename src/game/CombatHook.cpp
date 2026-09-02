#include "game/CombatHook.h"

#include <mutex>
#include <string>
#include <unordered_map>

namespace ft::game
{
namespace
{

bool g_installed = false;

// How long a request stays good.
//
// The combat AI decides when to ask, not us, so a request has to wait -- but
// not forever. Two seconds is long enough to survive a few AI ticks and short
// enough that a request never fires at a moment the rule did not intend, which
// would be indistinguishable from the AI acting on its own.
constexpr double kRequestTTL = 2.0;

struct Request
{
    std::uint32_t spell{0};
    double expiresAt{0.0};
};

// Everything below is touched from the COMBAT AI's thread, not ours.
//
// That is the bug that crashed the first version: g_requests was locked and
// g_lastQuiet was not, and concurrent insertion into an unordered_map is
// undefined behaviour. One mutex now covers both, because "this one is only a
// rate limiter" is not a thread-safety argument.
std::mutex g_mutex;
std::unordered_map<std::uint32_t, Request> g_requests;
std::unordered_map<std::uint32_t, double> g_lastQuiet;
std::unordered_map<std::uint64_t, double> g_lastScore;

// Do nothing but call through and say so, once.
//
// The crash left two suspects and no stack trace: the data race above, or a
// wrong vtable index, which would mean we replaced some other function and the
// game called it with mismatched arguments. Fixing the race does not
// distinguish them, so this build tests the second on its own -- if a hook that
// only calls the original still crashes, the index is wrong and no amount of
// locking helps.
//
// SETTLED, by a run that did not crash and logged:
//     combat-hook: vfunc 6 entered for the first time
//         (self=0x25ca62b4330, controller=0x25ca65f8700)
// Valid this-pointer, valid controller, no crash -- the slot is CheckStartCast
// and the AI does ask, 0.5s after a request was queued. The crash was the data
// race above and nothing else. Kept as a switch because it is the first thing
// to flip if this ever misbehaves again.
constexpr bool kPassthroughOnly = false;

double NowSeconds()
{
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

// Is this worth dereferencing at all?
//
// "Not null" is not the same as "valid", and this hook proved it twice. The
// crash log named the exact line:
//
//     [0] TESForm.h:287  RE::TESForm::GetFormID()
//     [1] CombatHook.cpp:107   <- const auto id = actor->GetFormID();
//     Base Register: rax = 0x0000000000000001
//
// So CombatController::cachedAttacker handed back 0x1. A NiPointer is not a
// guarantee; it is a raw pointer with a refcount protocol, and the AI can be in
// a state where nothing valid is in it. Every pointer that crosses out of game
// memory into our code goes through this now, rather than each one being
// hardened only after it crashes.
//
// This does not make a dereference safe -- a plausible address can still be
// garbage. It rejects what has actually been observed: small integers.
bool Plausible(const void *p)
{
    const auto value = reinterpret_cast<std::uintptr_t>(p);
    return value >= 0x10000 && value <= 0x7FFFFFFFFFFFULL && (value % 8) == 0;
}

// A MagicItem pointer worth dereferencing, or nullptr.
//
// CombatMagicCaster::magicItem is NOT null-or-valid. When the caster is not
// mid-cast it holds small sentinel values, and the crash log is unambiguous
// about it:
//
//     EXCEPTION_ACCESS_VIOLATION ... RE::TESForm::GetFormID()
//     Base Register: rax = 0x0000000000000001 (likely invalid)
//
// A plain `item ? ... : 0` waves 0x1 straight through. This is the same
// plausibility check Packages.cpp already applies before every dereference --
// written there because a bad read in a live game is a crash rather than a
// failed test, and then not carried across to this file. It should have been.
//
// This does not make a dereference SAFE, only much less likely to be insane; a
// plausible-looking pointer can still be garbage. It is worth having anyway,
// because the observed failure is a small integer, not a wild address.
RE::MagicItem *SafeMagic(RE::MagicItem *item)
{
    return Plausible(item) ? item : nullptr;
}

const char *NameOf(RE::MagicItem *item)
{
    auto *safe = SafeMagic(item);
    return (safe && safe->GetName()) ? safe->GetName() : "-";
}

std::uint32_t IdOf(RE::MagicItem *item)
{
    auto *safe = SafeMagic(item);
    return safe ? safe->GetFormID() : 0;
}

// What the AI thinks each option is worth, observed and not touched.
//
// CalculateScore is how the combat AI ranks its options -- it is the answer to
// "which spell", where CheckStartCast only answers "when". Making our spell win
// means returning a dominating score, and a dominating score cannot be chosen
// without knowing the scale the game works in. Returning a wild value into an
// unknown comparison is how you get a follower who stands still forever.
//
// So this build measures and changes nothing. The same discipline as the
// package canary: find out, then act.
//
// Hooked ONLY on the spell variants. The template enumerates every combination
// of (item type x caster category), so
//     ..._CombatInventoryItemMagic_CombatMagicCasterRestore_    a SPELL to heal
//     ..._CombatInventoryItemPotion_CombatMagicCasterRestore_   a POTION to heal
// are different classes with different vtables. Hooking the first cannot touch
// potions -- which is exactly the confusion that made the last run useless,
// designed out rather than guarded against.
// The shared body: report a score, never change it.
float ReportScore(const char *kind, RE::CombatInventoryItemMagic *self, RE::CombatController *controller, float score)
{
    if (!Plausible(self) || !Plausible(controller))
        return score;

    const RE::NiPointer<RE::Actor> attacker = controller->attackerHandle.get();
    auto *actor = attacker.get();
    if (!Plausible(actor) || !actor->IsPlayerTeammate())
        return score;

    auto *magic = SafeMagic(self->GetMagic());
    if (!magic)
        return score;

    const std::uint64_t key = (static_cast<std::uint64_t>(actor->GetFormID()) << 32) | IdOf(magic);
    {
        std::scoped_lock lock(g_mutex);
        auto &last = g_lastScore[key];
        const double now = NowSeconds();
        if (now - last < 2.0)
            return score;
        last = now;
    }

    logger::info("score: {:08X} rates [{}] {} {:08X} at {:.3f}", actor->GetFormID(), kind, NameOf(magic), IdOf(magic),
                 score);
    return score;
}

// A SPELL used to restore.
struct ScoreProbeSpell
{
    static float Thunk(RE::CombatInventoryItemMagic *self, RE::CombatController *controller)
    {
        return ReportScore("spell", self, controller, func(self, controller));
    }
    static inline REL::Relocation<decltype(Thunk)> func;
    static void Install()
    {
        REL::Relocation<std::uintptr_t> vtbl{
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemMagic_CombatMagicCasterRestore_[0]};
        func = vtbl.write_vfunc(0xC, Thunk);
    }
};

// A POTION used to restore.
//
// Observed too, and this is the number that actually matters: the last run
// showed the AI choosing a Potion of Magicka over Fast Healing, so the potion's
// score is what a boosted spell has to beat. Measuring only our own side told
// us the scale and nothing about the contest.
struct ScoreProbePotion
{
    static float Thunk(RE::CombatInventoryItemMagic *self, RE::CombatController *controller)
    {
        return ReportScore("potion", self, controller, func(self, controller));
    }
    static inline REL::Relocation<decltype(Thunk)> func;
    static void Install()
    {
        REL::Relocation<std::uintptr_t> vtbl{
            RE::VTABLE_CombatInventoryItemMagicT_CombatInventoryItemPotion_CombatMagicCasterRestore_[0]};
        func = vtbl.write_vfunc(0xC, Thunk);
    }
};

struct RestoreCastHook
{
    // The combat AI asking itself whether to start a restore cast.
    //
    // Returning true is a suggestion the game is free to act on; it still does
    // the casting. That is the whole point -- we never take the cast over, so
    // animation and interruption remain the game's.
    static bool Thunk(RE::CombatMagicCasterRestore *self, RE::CombatController *controller)
    {
        // Proof of life, exactly once. Its absence was the most informative
        // line in the log that did not have it: requests were queued twice and
        // this never printed, which is what proved the AI does ask and the slot
        // is the right one.
        static std::atomic<bool> announced{false};
        if (!announced.exchange(true))
            logger::info("combat-hook: vfunc 6 entered for the first time (self={}, controller={})",
                         static_cast<const void *>(self), static_cast<const void *>(controller));

        // ALWAYS ask the original first, even when we intend to override.
        //
        // This is the line that makes the experiment mean anything. A follower's
        // AI heals itself unprompted when badly hurt -- that is exactly what
        // confounded the earlier runs, where health jumped from 1 to 114 with no
        // rule of ours involved. Without knowing what the game WOULD have
        // answered, a heal cannot be attributed to us.
        const bool original = func(self, controller);

        if constexpr (kPassthroughOnly)
            return original;

        // Resolve the HANDLE, not the cache.
        //
        // This is what actually crashed, and the guard is the smaller half of
        // the fix. The header says it plainly:
        //
        //     ActorHandle      attackerHandle;   // 28
        //     NiPointer<Actor> cachedAttacker;   // C8 - attackerHandle
        //
        // cachedAttacker is a CACHE OF the handle. Reading it directly skips
        // the handle manager, which is the thing that knows whether the
        // reference is still alive -- so a stale or unset cache hands back
        // whatever was last there, and once that was 0x1. ActorHandle::get()
        // goes through BSPointerHandleManagerInterface and returns an empty
        // NiPointer when the actor is gone, which is the answer we wanted all
        // along.
        //
        // Plausible() stays as belt-and-braces, but it was covering for reading
        // the wrong field.
        if (!Plausible(controller) || !Plausible(self))
            return original;

        const RE::NiPointer<RE::Actor> attacker = controller->attackerHandle.get();
        auto *actor = attacker.get();
        if (!Plausible(actor))
            return original;

        const std::uint32_t id = actor->GetFormID();
        const double now = NowSeconds();

        // WHICH RESTORE CASTER IS ASKING -- checked BEFORE the request is taken.
        //
        // CombatMagicCasterRestore carries `primaryAV`, and the game runs one
        // instance per restorable value. They really are separate triggers; it
        // was this code that collapsed them, by keying a pending request on the
        // ACTOR alone. Whichever caster asked first consumed it, and the magicka
        // one usually asks first for a destruction mage -- which is why it held
        // a Potion of Magicka, why the crash landed in CombatBehaviorDrinkPotion,
        // and why healing never happened.
        //
        // Order matters as much as the check. Taking the request first and
        // filtering afterwards would still eat it on the magicka caster and
        // leave the health caster with nothing -- the same bug wearing a fix.
        const auto av = self->primaryAV;

        // Report every distinct caster that asks, once each.
        //
        // Without this the filter is unfalsifiable: "the health caster never
        // asks" and "primaryAV is not the field I think it is" produce exactly
        // the same silence, and they need opposite fixes. An earlier version
        // logged this and it was lost when the gate moved -- which cost a test
        // run that could only say "nothing happened".
        {
            static std::mutex seenMutex;
            static std::unordered_map<std::uint32_t, bool> seen;
            const auto raw = static_cast<std::uint32_t>(av);
            bool first = false;
            {
                std::scoped_lock lock(seenMutex);
                first = seen.emplace(raw, true).second;
            }
            if (first)
                logger::info("combat-hook: a restore caster asked, primaryAV={} ({})", raw,
                             av == RE::ActorValue::kHealth    ? "health"
                             : av == RE::ActorValue::kMagicka ? "magicka"
                             : av == RE::ActorValue::kStamina ? "stamina"
                                                              : "something else");
        }

        if (av != RE::ActorValue::kHealth)
            return original;

        Request request;
        {
            std::scoped_lock lock(g_mutex);
            const auto it = g_requests.find(id);
            if (it != g_requests.end() && it->second.expiresAt > now)
            {
                request = it->second;
                g_requests.erase(it);
            }
        }

        if (request.spell == 0)
        {
            // Nothing pending. Report the game's own decision, but only
            // occasionally, and only when it says yes -- an unprompted heal is
            // the confound worth seeing, a stream of "no" is noise.
            if (original)
            {
                bool report = false;
                {
                    std::scoped_lock lock(g_mutex);
                    auto &last = g_lastQuiet[id];
                    if (now - last > 1.0)
                    {
                        last = now;
                        report = true;
                    }
                }
                if (report)
                    logger::info("combat-hook: {:08X} AI chose to heal on its own (spell {} {:08X}) -- "
                                 "NOT ours",
                                 id, NameOf(self->magicItem), IdOf(self->magicItem));
            }
            return original;
        }

        // What the caster was going to use, from both places it could come
        // from. If these disagree, that alone explains a failed swap.
        // Same caution as magicItem: this is a virtual call through a pointer
        // the AI owns, and "not null" was already proven insufficient once.
        RE::MagicItem *inventorySpell = nullptr;
        if (auto *item = self->inventoryItem.get(); Plausible(item))
            inventorySpell = SafeMagic(item->GetMagic());
        const auto beforeId = IdOf(self->magicItem);
        const std::string beforeName = NameOf(self->magicItem);

        // DO NOT hijack a decision about a different item.
        //
        // CombatMagicCasterRestore is the "restore my stats" caster, and that
        // covers potions as well as spells: AlchemyItem derives from MagicItem,
        // and CombatInventoryItemPotion derives from CombatInventoryItemMagic.
        // The first run caught it holding a Potion of Magicka, because his
        // magicka was low and the AI had already picked one.
        //
        // Saying yes there does not cast our spell -- the behaviour tree is
        // committed to a potion drink and inventoryItem still points at it --
        // it just makes him drink at a moment the rule never asked for.
        //
        // So we only answer when the AI has ALREADY selected the spell we want.
        // That yields timing control over that spell and nothing else; making
        // the AI *choose* it is a different lever (CombatInventoryItem::
        // CalculateScore, vfunc 0C) and a separate change.
        if (IdOf(inventorySpell) != request.spell)
        {
            logger::info("  selected item is {} {:08X}, not the requested spell -- passing through",
                         NameOf(inventorySpell), IdOf(inventorySpell));
            return original;
        }

        auto *wanted = RE::TESForm::LookupByID<RE::MagicItem>(request.spell);
        bool swapped = false;
        if (wanted)
        {
            self->magicItem = wanted;
            swapped = (IdOf(self->magicItem) == request.spell);
        }

        logger::info("combat-hook: {:08X} REQUEST {} {:08X}", id, NameOf(wanted), request.spell);
        logger::info("  original would have said {} <- if true, a cast proves nothing about us",
                     original ? "YES" : "no");
        logger::info("  magicItem {} {:08X} -> {} {:08X} (write {})", beforeName, beforeId, NameOf(self->magicItem),
                     IdOf(self->magicItem), swapped ? "took" : "FAILED");
        logger::info("  inventoryItem->GetMagic() = {} {:08X} <- if this differs, the game may "
                     "re-derive and undo the swap",
                     NameOf(inventorySpell), IdOf(inventorySpell));

        return true;
    }

    static inline REL::Relocation<decltype(Thunk)> func;

    static void Install()
    {
        REL::Relocation<std::uintptr_t> vtbl{RE::VTABLE_CombatMagicCasterRestore[0]};
        func = vtbl.write_vfunc(0x6, Thunk);
    }
};

} // namespace

// OFF by default, and it should stay off unless someone is actively
// investigating.
//
// These hooks do not work: the health restore caster never asks, so a request
// is never consumed (see docs/MAGIC.md). What they DO have is the largest blast
// radius in the project. Everything else here touches one follower at a time;
// this sits in the combat AI's decision path for every actor in the game and
// filters to followers afterwards. It has crashed twice, and generalising it
// would mean roughly fifteen more of them.
//
// The code stays because the findings are worth keeping and the observation
// hooks are how they were found. Running it by default is not worth the risk it
// carries for the nothing it currently buys.
constexpr bool kEnableCombatHooks = false;

void InstallCombatHook()
{
    if (g_installed)
        return;

    if constexpr (!kEnableCombatHooks)
    {
        logger::info("combat-hook: disabled (kEnableCombatHooks) -- see docs/MAGIC.md");
        return;
    }

    RestoreCastHook::Install();
    ScoreProbeSpell::Install();
    ScoreProbePotion::Install();
    g_installed = true;
    logger::info("combat-hook: CheckStartCast (vfunc 6) and CalculateScore (vfunc 0C) hooked");
    logger::info("combat-hook: score probe is OBSERVE-ONLY -- it changes nothing");
}

bool CombatHookInstalled()
{
    return g_installed;
}

void RequestCombatCast(RE::Actor *actor, std::uint32_t spellFormID)
{
    if (!actor || spellFormID == 0)
        return;

    const std::uint32_t id = actor->GetFormID();
    {
        std::scoped_lock lock(g_mutex);
        g_requests[id] = Request{spellFormID, NowSeconds() + kRequestTTL};
    }

    // Logged here as well as in the hook, because the gap between the two is
    // itself a finding: a request that is never consumed means the AI never
    // asked, which is a completely different problem from a swap that failed.
    logger::info("combat-hook: queued cast {:08X} for {:08X} (valid {:.0f}s)", spellFormID, id, kRequestTTL);
}

} // namespace ft::game
