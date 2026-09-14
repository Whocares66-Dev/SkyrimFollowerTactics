#include "Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <spdlog/sinks/basic_file_sink.h>
#include <string>
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
// Hand-parsed, and deliberately: the whole file is two keys, and a dependency
// (or a settings framework) to read them would cost more than it saves. If a
// third section ever appears this moves out into its own file.

struct Settings
{
    Level level{Level::Info};
    bool events{true};
};

[[nodiscard]] std::string_view Trim(std::string_view text) noexcept
{
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] bool IsTrue(std::string_view text) noexcept
{
    return text == "1" || text == "true" || text == "TRUE" || text == "True" || text == "yes" || text == "on";
}

[[nodiscard]] bool IsFalse(std::string_view text) noexcept
{
    return text == "0" || text == "false" || text == "FALSE" || text == "False" || text == "no" || text == "off";
}

// Data/SKSE/Plugins/FollowerTactics.ini, beside the .dll. Relative to the
// process, which the game runs from its own root; under MO2 the virtual file
// system resolves it to whichever mod supplies it, same as the .dll itself.
//
// An absent file is the normal case, not an error: the defaults below are what
// a fresh install runs on, and this reports nothing when there is nothing to
// report. (It cannot report anyway -- it runs before the log is open.)
[[nodiscard]] Settings ReadSettings(std::vector<std::string> &notes)
{
    Settings settings;

    std::ifstream file("Data/SKSE/Plugins/FollowerTactics.ini");
    if (!file)
        return settings;

    notes.emplace_back("settings read from Data/SKSE/Plugins/FollowerTactics.ini");

    std::string section;
    std::string raw;
    while (std::getline(file, raw))
    {
        std::string_view line = Trim(raw);
        if (line.empty() || line.front() == ';' || line.front() == '#')
            continue;

        if (line.front() == '[')
        {
            const auto close = line.find(']');
            section = close == std::string_view::npos ? std::string{} : std::string(line.substr(1, close - 1));
            continue;
        }

        const auto equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;

        const std::string_view key = Trim(line.substr(0, equals));
        const std::string_view value = Trim(line.substr(equals + 1));

        if (section != "Log")
            continue;

        if (key == "level")
        {
            if (!ParseLevel(value, settings.level))
                notes.push_back(fmt::format("level \"{}\" is not one of error/warn/info/debug -- using info", value));
        }
        else if (key == "events")
        {
            // A word that is neither is said, not read as off: "events =
            // yse" silently losing the sidecar would be found only by its
            // absence.
            if (IsTrue(value))
                settings.events = true;
            else if (IsFalse(value))
                settings.events = false;
            else
                notes.push_back(fmt::format("events \"{}\" is not true or false -- using true", value));
        }
    }

    return settings;
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

} // namespace

bool Enabled(Level level) noexcept
{
    return level >= g_level.load(std::memory_order_relaxed);
}

bool EventsOn() noexcept
{
    return g_events != nullptr;
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

void Write(Level level, std::string_view module, std::string_view text)
{
    // Padded to the longest module name so the messages line up in a column
    // and the eye can skip the bracket entirely when tailing.
    spdlog::log(ToSpdlog(level), "{:<11}{}", fmt::format("[{}]", module), text);
}

void Emit(Level level, std::string_view event, RE::Actor *who, std::span<const Field> fields, std::string_view module,
          std::string_view prose)
{
    if (Enabled(level))
        Write(level, module, prose);

    if (!g_events)
        return;

    // The follower's id and name lead the event's own fields, so every line
    // about a follower is keyed the same way whatever the event.
    std::vector<Field> all;
    all.reserve(fields.size() + 2);
    if (who)
    {
        all.emplace_back("followerId", Id(IdOf(who)));
        all.emplace_back("followerName", NameOf(who));
    }
    for (const auto &field : fields)
        all.push_back(field);

    g_events->log(ToSpdlog(level), "{}", FormatEvent(level, event, FT_VERSION, Timestamp(), all));
}

void Init()
{
    std::vector<std::string> notes;
    const Settings settings = ReadSettings(notes);

    const auto directory = SKSE::log::log_directory();
    if (!directory)
        return;

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
    plugin.info("FollowerTactics v{}", FT_VERSION);

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
