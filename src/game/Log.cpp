#include "Log.h"

#include "core/Archive.h"
#include "core/LogSettings.h"
#include "core/Sessions.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
#include <system_error>
#include <vector>

namespace ft::log
{
namespace
{

// The level the prose log is filtered by; the events file takes every game
// event whatever it says. Read once from the ini at Init and never again --
// an atomic only so that a read from the render thread (the panel logs)
// cannot race the write at load.
std::atomic<Level> g_level{Level::Info};

// The sidecar. A logger of its own with the bare "%v" pattern, so what lands
// in the file is exactly the JSON line and nothing spdlog decided to add.
std::shared_ptr<spdlog::logger> g_events;

// What each file has been handed this session, against kCeilingBytes.
// Atomic: the panel logs from the render thread, the animation sink from its
// own.
std::atomic<std::uint64_t> g_proseBytes{0};
std::atomic<std::uint64_t> g_eventBytes{0};

// The last game events, for the panel. Pushed from whichever thread emitted,
// copied out by the render thread: guarded, the lock held only for the push
// or the copy.
std::mutex g_recentMutex;
EventRing g_recent{kRecentEvents};

[[nodiscard]] spdlog::level::level_enum ToSpdlog(Level level) noexcept
{
    switch (level)
    {
    case Level::Debug:
        return spdlog::level::debug;
    case Level::Info:
        return spdlog::level::info;
    case Level::Warn:
        return spdlog::level::warn;
    case Level::Error:
        return spdlog::level::err;
    }
    return spdlog::level::info;
}

// --- the ini ---------------------------------------------------------------
//
// Data/SKSE/Plugins/FollowerTactics.ini, beside the .dll. Relative to the
// process, which the game runs from its own root; under MO2 the virtual file
// system resolves it to whichever mod supplies it, same as the .dll itself.
//
// An absent file is the normal case, not an error: the defaults are what a
// fresh install runs on, and this reports nothing when there is nothing to
// report. (It cannot report anyway -- it runs before the log is open.) What
// the text means is core's (core/LogSettings.h).
[[nodiscard]] IniSettings ReadSettings(std::vector<std::string> &notes)
{
    std::ifstream file("Data/SKSE/Plugins/FollowerTactics.ini");
    if (!file)
        return {};

    notes.emplace_back("settings read from Data/SKSE/Plugins/FollowerTactics.ini");
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return ParseIniSettings(text, notes);
}

// "2026-09-09T14:02:11.400Z". UTC, milliseconds, sortable, and the same shape
// whatever the player's locale -- the point of the sidecar is that a query
// works, and a local-time stamp with no offset does not sort across a DST
// boundary.
[[nodiscard]] std::string Timestamp()
{
    using namespace std::chrono;

    const auto now = system_clock::now();
    const auto seconds = time_point_cast<std::chrono::seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - seconds).count();

    const std::time_t raw = system_clock::to_time_t(seconds);
    std::tm utc{};
    gmtime_s(&utc, &raw);

    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                       utc.tm_hour, utc.tm_min, utc.tm_sec, millis);
}

// The archive is core's (core/Archive.h): the naming and the retention
// were already, and the disk work around them is testable against a
// folder in the temporary directory rather than only in play.
} // namespace

bool Enabled(Level level) noexcept
{
    return level >= g_level.load(std::memory_order_relaxed);
}

bool EventsOn() noexcept
{
    return g_events != nullptr;
}

std::vector<LoggedEvent> RecentEvents(std::uint64_t after)
{
    std::scoped_lock lock(g_recentMutex);
    return g_recent.Since(after);
}

std::uint32_t IdOf(RE::Actor *actor) noexcept
{
    return actor ? actor->GetFormID() : 0;
}

std::string NameOf(RE::Actor *actor)
{
    if (!actor)
        return "<none>";
    const char *name = actor->GetDisplayFullName();
    return (name && *name) ? name : "<unnamed>";
}

std::string NameOf(const RE::TESForm *form)
{
    if (!form)
        return "<none>";
    const char *name = form->GetName();
    return (name && *name) ? name : "<unnamed>";
}

namespace
{
[[nodiscard]] NamedActor NameActor(std::uint32_t actorId)
{
    auto *actor = actorId != 0 ? RE::TESForm::LookupByID<RE::Actor>(actorId) : nullptr;
    const auto *base = actor ? actor->GetTemplateBase() : nullptr;
    return {actorId, base ? base->GetFormID() : 0, actor ? NameOf(actor) : std::string{}};
}
} // namespace

void AppendActor(std::vector<Field> &fields, std::string_view idKey, std::string_view baseKey, std::string_view nameKey,
                 std::uint32_t actorId)
{
    NamedActor named = NameActor(actorId);
    fields.emplace_back(idKey, Id(named.formId));
    fields.emplace_back(baseKey, Id(named.baseFormId));
    fields.emplace_back(nameKey, std::move(named.name));
}

std::vector<NamedActor> Actors(const std::vector<std::uint32_t> &actorIds)
{
    std::vector<NamedActor> out;
    out.reserve(actorIds.size());
    for (const std::uint32_t id : actorIds)
        out.push_back(NameActor(id));
    return out;
}

void AppendForm(std::vector<Field> &fields, std::string_view idKey, std::string_view nameKey, std::uint32_t formId)
{
    const auto *form = formId != 0 ? RE::TESForm::LookupByID(formId) : nullptr;
    fields.emplace_back(idKey, Id(formId));
    fields.emplace_back(nameKey, form ? NameOf(form) : std::string{});
}

void Write(Level level, std::string_view module, std::string_view text)
{
    // The pattern's time, thread and level, the module column, the line end.
    constexpr std::uint64_t kPrefixBytes = 40;
    const std::uint64_t bytes = text.size() + kPrefixBytes;
    switch (RoomFor(g_proseBytes.fetch_add(bytes), bytes, kCeilingBytes))
    {
    case Room::Write:
        // Padded to the longest module name so the messages line up in a
        // column and the eye can skip the bracket entirely when tailing.
        spdlog::log(ToSpdlog(level), "{:<11}{}", fmt::format("[{}]", module), text);
        return;
    case Room::Last:
        spdlog::log(spdlog::level::warn,
                    "{:<11}FollowerTactics.log has taken its {} MB for this session -- nothing more is written to it",
                    "[plugin]", kCeilingBytes >> 20);
        return;
    case Room::None:
        return;
    }
}

void Emit(Level level, std::string_view event, RE::Actor *who, std::span<const Field> fields, std::string_view module,
          std::string_view prose)
{
    if (Enabled(level))
        Write(level, module, prose);

    const std::string stamp = Timestamp();
    const std::uint32_t followerId = IdOf(who);
    const std::string followerName = who ? NameOf(who) : std::string{};
    {
        LoggedEvent kept{0,
                         stamp,
                         level,
                         std::string(event),
                         followerId,
                         followerName,
                         std::string(prose),
                         {fields.begin(), fields.end()}};
        std::scoped_lock lock(g_recentMutex);
        g_recent.Push(std::move(kept));
    }

    if (!g_events)
        return;

    // The follower's id and name lead the event's own fields, so every line
    // about a follower is keyed the same way whatever the event.
    std::vector<Field> all;
    all.reserve(fields.size() + 2);
    if (who)
    {
        all.emplace_back("followerId", Id(followerId));
        all.emplace_back("followerName", followerName);
    }
    for (const auto &field : fields)
        all.push_back(field);

    const std::string line = FormatEvent(level, event, FT_VERSION, stamp, all);
    const std::uint64_t bytes = line.size() + 2;
    switch (RoomFor(g_eventBytes.fetch_add(bytes), bytes, kCeilingBytes))
    {
    case Room::Write:
        g_events->log(ToSpdlog(level), "{}", line);
        return;
    case Room::Last:
        g_events->log(
            spdlog::level::warn, "{}",
            FormatEvent(Level::Warn, "session.ceiling", FT_VERSION, Timestamp(), {{"megabytes", kCeilingBytes >> 20}}));
        return;
    case Room::None:
        return;
    }
}

void Init()
{
    std::vector<std::string> notes;
    const IniSettings settings = ReadSettings(notes);

    const auto directory = SKSE::log::log_directory();
    if (!directory)
        return;

    ArchiveSession(*directory, notes);
    // One start for both files, read back at the next launch to name this
    // session's pair in the archive.
    const std::string began = Timestamp();

    // The prose log opens at debug and is tightened at the bottom of this
    // function, so the banner saying WHICH level was read is written before
    // the filter that would hide it. At `level = error` an otherwise empty log
    // would leave no way to tell a quiet setting from a mod that never loaded.
    // The events file is never tightened: the level is not its filter.
    {
        auto path = *directory / "FollowerTactics.log";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        auto prose = std::make_shared<spdlog::logger>("global", std::move(sink));
        prose->set_level(spdlog::level::debug);
        // Flush at every level that is written at all: a crash mid-session is
        // exactly when the last line matters most, and this log is nowhere
        // near hot enough for the buffering to be worth its risk.
        prose->flush_on(spdlog::level::debug);
        spdlog::set_default_logger(std::move(prose));
        // The shape CommonLibSSE-NG's logger gave the file while it was the
        // one writing it: time, thread, the level's letter. Kept, so the
        // log reads as it always has.
        spdlog::set_pattern("[%T.%e] [%=5t] [%L] %v");
    }
    plugin.info("FollowerTactics v{} -- {}{}", FT_VERSION, kSessionBegan, began);

    if (settings.events)
    {
        auto path = *directory / "FollowerTactics.events.jsonl";
        auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), true);
        g_events = std::make_shared<spdlog::logger>("events", std::move(sink));
        g_events->set_level(spdlog::level::debug);
        // Buffered, unlike the prose log: flushed on a warning and otherwise
        // once a second. A crash loses at most that second, and the prose
        // log, which carries the same events, still flushes every line. The
        // flusher reaches registered loggers only; the prose logger is one,
        // as the default.
        g_events->flush_on(spdlog::level::warn);
        g_events->set_pattern("%v");
        g_events->log(spdlog::level::info, "{}", FormatEvent(Level::Info, "session.started", FT_VERSION, began, {}));
        spdlog::register_logger(g_events);
        spdlog::flush_every(std::chrono::seconds(1));
    }

    plugin.info("log level {}, events {}", ToString(settings.level),
                settings.events ? "to FollowerTactics.events.jsonl" : "off");
    for (const auto &note : notes)
        plugin.info("{}", note);

    g_level.store(settings.level, std::memory_order_relaxed);
    spdlog::default_logger()->set_level(ToSpdlog(settings.level));
    spdlog::default_logger()->flush_on(ToSpdlog(settings.level));
}

} // namespace ft::log
