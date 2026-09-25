#include "game/Graph.h"

#include "game/Log.h"
#include "game/Tactics.h"
#include "game/Util.h"

namespace ft::game
{
namespace
{

// Never destroyed: a request's watch closes itself as it goes, and one held
// in another file's static would otherwise close into a destroyed list at
// exit.
ft::GraphWatches &Watches()
{
    static auto *watches = new ft::GraphWatches;
    return *watches;
}

// What fired, read as the event comes: the spell selected in the firing
// hand -- the caster's own currentSpell is already null by then, and the
// UseMagic procedure equips what it casts -- or the shout being shouted.
const RE::TESForm *FiredForm(RE::Actor *actor, ft::GraphTag tag)
{
    if (!actor)
        return nullptr;
    auto &runtime = actor->GetActorRuntimeData();
    if (tag == ft::GraphTag::SpellFireVoice)
    {
        const auto *process = runtime.currentProcess;
        const auto *high = process ? process->high : nullptr;
        return high ? high->currentShout : nullptr;
    }
    return runtime.selectedSpells[tag == ft::GraphTag::SpellFireRight ? RE::Actor::SlotTypes::kRightHand
                                                                      : RE::Actor::SlotTypes::kLeftHand];
}

// A fire, with what went off: at info where it was a request's own, since a
// cast that lands nothing is read from these. The voice's says what the
// engine chose of the shout's words and what the voice holds (2026-09-04).
void LogFire(RE::Actor *actor, std::uint32_t id, ft::GraphTag tag, const RE::TESForm *fired, bool own)
{
    const std::uint32_t form = fired ? fired->GetFormID() : 0;
    const log::Level level = own ? log::Level::Info : log::Level::Debug;
    const char *whose = own ? "ours" : "not ours";
    if (tag != ft::GraphTag::SpellFireVoice)
    {
        log::graph.at(level, "anim {:08X}: the {} hand fired {:08X} \"{}\" -- {}", id,
                      tag == ft::GraphTag::SpellFireRight ? "right" : "left", form, log::NameOf(fired), whose);
        return;
    }
    const auto *process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
    const auto *high = process ? process->high : nullptr;
    const auto *voiceItem =
        actor ? actor->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kPowerOrShout] : nullptr;
    const auto *caster =
        actor ? actor->GetActorRuntimeData().magicCasters[RE::Actor::SlotTypes::kPowerOrShout] : nullptr;
    log::graph.at(level,
                  "anim {:08X}: the voice fired: shout {:08X} variation {} level {}, voice slot holds {:08X}, caster "
                  "spell {:08X} -- {}",
                  id, form, high ? static_cast<std::int32_t>(high->currentShoutVariation) : -99,
                  actor ? actor->GetCurrentShoutLevel() : -99, voiceItem ? voiceItem->GetFormID() : 0,
                  caster && caster->currentSpell ? caster->currentSpell->GetFormID() : 0, whose);
}

class GraphSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
{
  public:
    RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent *ev,
                                          RE::BSTEventSource<RE::BSAnimationGraphEvent> *) override
    {
        // Every event of every graph the sink was ever put on comes here,
        // footsteps and sounds among them: nothing is read while no watch
        // is open, and nothing of an actor no watch is on.
        if (!ev || !ev->holder || ev->tag.empty() || !Watches().AnyOpen())
            return RE::BSEventNotifyControl::kContinue;
        const std::uint32_t id = ev->holder->GetFormID();
        if (!Watches().Watching(id))
            return RE::BSEventNotifyControl::kContinue;
        const char *name = ev->tag.c_str();
        log::graph.debug("anim {:08X}: {}", id, name);
        const auto tag = ft::GraphTagOf(name);
        if (!tag)
            return RE::BSEventNotifyControl::kContinue;
        auto *actor = const_cast<RE::TESObjectREFR *>(ev->holder)->As<RE::Actor>();
        const RE::TESForm *fired = ft::IsFire(*tag) ? FiredForm(actor, *tag) : nullptr;
        const ft::Recorded recorded = Watches().Record(id, *tag, fired ? fired->GetFormID() : 0);
        if (ft::IsFire(*tag))
            LogFire(actor, id, *tag, fired, recorded.ownFire);
        if (recorded.wakes)
            if (auto *tasks = SKSE::GetTaskInterface())
                tasks->AddTask([] { StepInFlightNow(); });
        return RE::BSEventNotifyControl::kContinue;
    }
};
GraphSink g_sink;

} // namespace

ft::GraphWatch WatchGraph(RE::Actor *actor, ft::GraphTags wakes, std::span<const ft::OwnFire> fires)
{
    if (!actor)
        return {};
    // The library's AddAnimationGraphEventSink looks for the sink first and
    // adds it only where it is missing (RE/A/Actor.cpp).
    if (actor->AddAnimationGraphEventSink(&g_sink))
        log::graph.debug("{}: the sink is on their graph", Describe(actor));
    return Watches().Open(actor->GetFormID(), wakes, fires);
}

} // namespace ft::game
