#include "progression/game/Events.h"

#include "progression/game/Log.h"
#include "progression/game/Service.h"
#include "progression/game/UI.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace fp::game
{
namespace
{

// Back at the main menu, the game just left is no longer the panel's to act
// on; the next load or new game starts it again.
class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
  public:
    RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent *event,
                                          RE::BSTEventSource<RE::MenuOpenCloseEvent> *) override
    {
        if (event && event->opening && event->menuName == RE::MainMenu::MENU_NAME)
            OnGameLeft();
        return RE::BSEventNotifyControl::kContinue;
    }
};

// The player's level-up: the only thing that moves a scaling follower's
// engine level.
class LevelSink : public RE::BSTEventSink<RE::LevelIncrease::Event>
{
  public:
    RE::BSEventNotifyControl ProcessEvent(const RE::LevelIncrease::Event *event,
                                          RE::BSTEventSource<RE::LevelIncrease::Event> *) override
    {
        if (event)
        {
            const int level = event->newLevel;
            if (auto *tasks = SKSE::GetTaskInterface())
                tasks->AddTask([level] { OnPlayerLevelUp(level); });
        }
        return RE::BSEventNotifyControl::kContinue;
    }
};

// The tick is paced from its own thread and runs on the game thread, one
// task per second. A task must never queue itself again: SKSE drains its
// queue to empty in one call, and a task that refills it hangs the game
// (Follower Tactics' CLAUDE.md, "SKSE gotchas").
// At most one tick waits in the queue: during a load or any stall of the
// main thread, seconds do not pile up into a burst of ticks run back to
// back (Follower Tactics' g_tickQueued).
std::atomic<bool> g_tickQueued{false};

void Pace()
{
    std::thread([] {
        for (;;)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            auto *tasks = SKSE::GetTaskInterface();
            if (!tasks || g_tickQueued.exchange(true))
                continue;
            tasks->AddTask([] {
                g_tickQueued.store(false);
                Tick();
                ui::SyncEntries();
            });
        }
    }).detach();
}

} // namespace

void InstallEvents()
{
    static MenuSink menus;
    static LevelSink levels;
    if (auto *ui = RE::UI::GetSingleton())
        ui->AddEventSink<RE::MenuOpenCloseEvent>(&menus);
    if (auto *source = RE::LevelIncrease::GetEventSource())
        source->AddEventSink(&levels);
    Pace();
    log::plugin.info("the tick runs once a second");
}

} // namespace fp::game
