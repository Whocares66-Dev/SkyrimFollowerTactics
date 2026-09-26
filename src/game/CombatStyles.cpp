#include "game/CombatStyles.h"

#include "game/Log.h"
#include "game/Settings.h"
#include "game/Tactics.h"
#include "game/Util.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ft::game
{
namespace
{

// Memory for one follower's copy, as a TESCombatStyle lays out. Made the
// first time they are tuned and kept for the session at one address: a
// fight may point at it after their tuning is gone, and a copy that stays
// is never one freed under it.
struct Copy
{
    alignas(RE::TESCombatStyle) std::array<std::byte, sizeof(RE::TESCombatStyle)> bytes{};

    RE::TESCombatStyle *Style()
    {
        return reinterpret_cast<RE::TESCombatStyle *>(bytes.data());
    }
};

// All game thread: the panel's changes reach here through tasks.
std::unordered_map<ft::ActorId, std::unique_ptr<Copy>> g_copies;
std::unordered_map<ft::ActorId, ft::StyleAdjustments> g_adjustments;

// Each base record pointed at a copy, by its form ID, and the style it had:
// what a load puts back. By ID and not by pointer: a leveled actor's base is
// a temporary record the engine may free while the session goes on.
struct Lent
{
    RE::TESCombatStyle *own{nullptr};
    RE::TESCombatStyle *copy{nullptr};
};
std::unordered_map<RE::FormID, Lent> g_lent;

// A reference to the field, const on a const style.
template <class Style> auto &FieldOf(Style &style, ft::StyleField field)
{
    using F = ft::StyleField;
    auto &g = style.generalData;
    auto &m = style.meleeData;
    auto &c = style.closeRangeData;
    switch (field)
    {
    case F::Offensive:
        return g.offensiveMult;
    case F::Defensive:
        return g.defensiveMult;
    case F::MeleeScore:
        return g.meleeScoreMult;
    case F::MagicScore:
        return g.magicScoreMult;
    case F::RangedScore:
        return g.rangedScoreMult;
    case F::StaffScore:
        return g.staffScoreMult;
    case F::ShoutScore:
        return g.shoutScoreMult;
    case F::UnarmedScore:
        return g.unarmedScoreMult;
    case F::AttackStaggered:
        return m.attackIncapacitatedMult;
    case F::PowerAttackStaggered:
        return m.powerAttackIncapacitatedMult;
    case F::PowerAttackBlocking:
        return m.powerAttackBlockingMult;
    case F::Bash:
        return m.bashMult;
    case F::BashRecoiled:
        return m.bashRecoilMult;
    case F::BashAttacking:
        return m.bashAttackMult;
    case F::BashPowerAttacking:
        return m.bashPowerAttackMult;
    case F::Circle:
        return c.circleMult;
    case F::Fallback:
        return c.fallbackMult;
    case F::FlankDistance:
        return c.flankDistanceMult;
    case F::StalkTime:
        return c.stalkTimeMult;
    case F::Strafe:
    case F::COUNT:
    default:
        return style.longRangeData.strafeMult;
    }
}

// A switch's flag in the style's DATA flags, the ones the engine reads:
// dual wielding in the combat inventory's build (44889), flanking in the
// movement tree (49448).
RE::TESCombatStyle::FLAG FlagOf(ft::StyleSwitch which)
{
    return which == ft::StyleSwitch::Flanking ? RE::TESCombatStyle::FLAG::kFlankingStyle
                                              : RE::TESCombatStyle::FLAG::kAllowDualWielding;
}

bool RecordSwitch(const RE::TESCombatStyle &own, ft::StyleSwitch which)
{
    return own.flags.all(FlagOf(which));
}

bool Managing()
{
    return CurrentSettings().manageCombatStyle;
}

RE::TESCombatStyle *CopyOf(ft::ActorId id)
{
    const auto it = g_copies.find(id);
    return it != g_copies.end() ? it->second->Style() : nullptr;
}

const ft::StyleAdjustments *AdjustmentsOf(ft::ActorId id)
{
    const auto it = g_adjustments.find(id);
    return it != g_adjustments.end() ? &it->second : nullptr;
}

// What a follower's adjustments are after `change`, kept only while they
// change anything.
template <class Change> void Adjust(ft::ActorId id, Change change)
{
    ft::StyleAdjustments &adjustments = g_adjustments[id];
    change(adjustments);
    if (!ft::AnyStyleAdjustment(adjustments))
        g_adjustments.erase(id);
}

// The style their record has under ours: the one it had when a copy went
// on. Any copy, not only this follower's: two followers of one base record
// share it, and one's copy is never the other's own.
RE::TESCombatStyle *OwnStyle(RE::TESNPC *npc)
{
    RE::TESCombatStyle *current = npc->combatStyle;
    if (!IsTunedCopy(current))
        return current;
    const auto it = g_lent.find(npc->GetFormID());
    return it != g_lent.end() ? it->second.own : nullptr;
}

// The record's bytes, then the sums and the switches. The whole object,
// header and all: the engine's reads find a TESCombatStyle, and its FormID
// is the record's. The flags are the DATA ones (FlagOf); the record
// header's dual wield bit is left as it is. Dueling is kept the other of
// Flanking, as the Creation Kit keeps the pair, though nothing in play
// reads it.
void Fill(RE::TESCombatStyle *copy, const RE::TESCombatStyle *own, const ft::StyleAdjustments &adjustments)
{
    std::memcpy(static_cast<void *>(copy), static_cast<const void *>(own), sizeof(RE::TESCombatStyle));
    for (std::size_t i = 0; i < ft::kStyleFields; ++i)
    {
        const auto field = static_cast<ft::StyleField>(i);
        float &value = FieldOf(*copy, field);
        value = ft::AdjustedStyleValue(field, value, adjustments.deltas[i]);
    }
    for (std::size_t i = 0; i < ft::kStyleSwitches; ++i)
    {
        const auto which = static_cast<ft::StyleSwitch>(i);
        const bool on = ft::SwitchOn(adjustments, which, RecordSwitch(*own, which));
        if (on)
            copy->flags.set(FlagOf(which));
        else
            copy->flags.reset(FlagOf(which));
        if (which == ft::StyleSwitch::Flanking && adjustments.switches[i])
        {
            if (on)
                copy->flags.reset(RE::TESCombatStyle::FLAG::kDuelingStyle);
            else
                copy->flags.set(RE::TESCombatStyle::FLAG::kDuelingStyle);
        }
    }
}

// Their style as the tuning and the setting say: the copy with the sums on
// their record and on the fight that fights by the record's, or the
// record's own back.
void Sync(RE::Actor *actor)
{
    auto *npc = actor ? actor->GetActorBase() : nullptr;
    if (!npc || actor->IsPlayerRef())
        return;
    const ft::ActorId id = actor->GetFormID();
    RE::TESCombatStyle *copy = CopyOf(id);
    RE::TESCombatStyle *current = npc->combatStyle;
    // No style under ours to put back, or none at all: the record is left
    // as it is rather than given none.
    RE::TESCombatStyle *own = OwnStyle(npc);
    if (!own)
        return;
    const ft::StyleAdjustments *adjustments = AdjustmentsOf(id);
    const bool tuned = adjustments && ft::AnyStyleAdjustment(*adjustments) && Managing();

    RE::TESCombatStyle *wanted = own;
    if (tuned)
    {
        if (!copy)
            copy = g_copies.emplace(id, std::make_unique<Copy>()).first->second->Style();
        Fill(copy, own, *adjustments);
        wanted = copy;
        g_lent[npc->GetFormID()] = {own, copy};
    }
    else
    {
        g_lent.erase(npc->GetFormID());
    }
    npc->combatStyle = wanted;
    // The fight too, where it fights by the record's style or ours, and not
    // by another -- a package's, say.
    if (auto *controller = actor->GetActorRuntimeData().combatController;
        controller && controller->combatStyle &&
        (controller->combatStyle == current || controller->combatStyle == own ||
         (copy && controller->combatStyle == copy)))
        controller->combatStyle = wanted;
    if (wanted != current)
        log::styles.info("{}: combat style {:08X} {}", Describe(actor), own ? own->GetFormID() : 0,
                         tuned ? "tuned" : "as the record has it");
}

} // namespace

float StyleValue(const RE::TESCombatStyle &style, ft::StyleField field)
{
    return FieldOf(style, field);
}

void RequestStyleDelta(ft::ActorId id, ft::StyleField field, float delta)
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    task->AddTask([id, field, delta] {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        if (!actor || actor->IsPlayerRef())
            return;
        const float snapped = ft::SnapStyleDelta(field, delta);
        Adjust(id, [&](ft::StyleAdjustments &a) { a.deltas[static_cast<std::size_t>(field)] = snapped; });
        log::styles.debug("{}: {} {:+.2f}", Describe(actor), ft::WireName(field), snapped);
        Sync(actor);
        RefreshShownPage();
    });
}

void RequestStyleReset(ft::ActorId id)
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    task->AddTask([id] {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        g_adjustments.erase(id);
        if (!actor)
            return;
        Sync(actor);
        RefreshShownPage();
    });
}

void RequestStyleSwitch(ft::ActorId id, ft::StyleSwitch which)
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    task->AddTask([id, which] {
        auto *actor = RE::TESForm::LookupByID<RE::Actor>(id);
        auto *npc = actor && !actor->IsPlayerRef() ? actor->GetActorBase() : nullptr;
        const RE::TESCombatStyle *own = npc ? OwnStyle(npc) : nullptr;
        if (!own)
            return;
        const bool record = RecordSwitch(*own, which);
        const auto at = static_cast<std::size_t>(which);
        Adjust(id, [&](ft::StyleAdjustments &a) { a.switches[at] = ft::ToggledSwitch(a, which, record); });
        const ft::StyleAdjustments *now = AdjustmentsOf(id);
        log::styles.debug("{}: {} {}", Describe(actor), ft::WireName(which),
                          ft::SwitchOn(now ? *now : ft::StyleAdjustments{}, which, record) ? "on" : "off");
        Sync(actor);
        RefreshShownPage();
    });
}

ft::StyleAdjustments StyleAdjustmentsOf(ft::ActorId id)
{
    const ft::StyleAdjustments *adjustments = AdjustmentsOf(id);
    return adjustments ? *adjustments : ft::StyleAdjustments{};
}

void AdoptCombatStyle(RE::Actor *actor, const ft::StyleAdjustments &adjustments)
{
    if (!actor || !ft::AnyStyleAdjustment(adjustments))
        return;
    g_adjustments[actor->GetFormID()] = adjustments;
    Sync(actor);
}

void KeepCombatStyle(RE::Actor *actor)
{
    if (!actor || !AdjustmentsOf(actor->GetFormID()) || !Managing())
        return;
    auto *npc = actor->GetActorBase();
    if (!npc)
        return;
    // Any copy on the record will do: of two followers of one base record,
    // the one tuned last has it, and taking it back each tick would trade
    // it between them for ever. A new fight takes the record's style, which
    // is a copy; one that took the style under it instead is put right.
    auto *controller = actor->GetActorRuntimeData().combatController;
    if (!IsTunedCopy(npc->combatStyle) || (controller && controller->combatStyle == OwnStyle(npc)))
        Sync(actor);
}

void RequestCombatStylesSynced()
{
    auto *task = SKSE::GetTaskInterface();
    if (!task)
        return;
    task->AddTask([] {
        // Every follower ever tuned this session, tuned now or not: the
        // setting turned off takes a copy off one whose tuning is kept.
        std::vector<ft::ActorId> ids;
        ids.reserve(g_copies.size());
        for (const auto &[id, copy] : g_copies)
            ids.push_back(id);
        for (const ft::ActorId id : ids)
            Sync(RE::TESForm::LookupByID<RE::Actor>(id));
        RefreshShownPage();
    });
}

std::optional<ft::StyleTuning> StyleTuningOf(RE::Actor *actor)
{
    auto *npc = actor ? actor->GetActorBase() : nullptr;
    if (!npc || actor->IsPlayerRef() || !Managing())
        return std::nullopt;
    const RE::TESCombatStyle *own = OwnStyle(npc);
    if (!own)
        return std::nullopt;
    ft::StyleTuning tuning;
    for (std::size_t i = 0; i < ft::kStyleFields; ++i)
        tuning.base[i] = StyleValue(*own, static_cast<ft::StyleField>(i));
    for (std::size_t i = 0; i < ft::kStyleSwitches; ++i)
        tuning.baseSwitches[i] = RecordSwitch(*own, static_cast<ft::StyleSwitch>(i));
    tuning.adjustments = StyleAdjustmentsOf(actor->GetFormID());
    return tuning;
}

bool IsTunedCopy(const RE::TESCombatStyle *style)
{
    if (!style)
        return false;
    for (const auto &[id, copy] : g_copies)
        if (copy->Style() == style)
            return true;
    return false;
}

void ForgetCombatStyles()
{
    // A record put back only while it still points at the copy: a script's
    // own SetCombatStyle since then is its business, and a leveled actor's
    // freed base may have left its ID to another record.
    for (const auto &[base, lent] : g_lent)
    {
        auto *npc = RE::TESForm::LookupByID<RE::TESNPC>(base);
        if (npc && npc->combatStyle == lent.copy)
            npc->combatStyle = lent.own;
    }
    g_lent.clear();
    g_adjustments.clear();
}

} // namespace ft::game
