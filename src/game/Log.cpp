#include "Log.h"

#include "core/Sessions.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
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

// A session's two files, and the ending each keeps in the archive.
constexpr std::array<std::pair<std::string_view, std::string_view>, 2> kSessionFiles{{
    {"FollowerTactics.events.jsonl", ".events.jsonl"},
    {"FollowerTactics.log", ".log"},
}};

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

// --- the archive -----------------------------------------------------------

[[nodiscard]] std::string FirstLine(const std::filesystem::path &file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

// When a file was last written, to the second. Not when it was created: a
// truncated file keeps its creation time, and NTFS gives a file made under a
// name just renamed away the old file's.
[[nodiscard]] std::optional<UtcTime> WrittenAt(const std::filesystem::path &file)
{
    std::error_code error;
    const auto written = std::filesystem::last_write_time(file, error);
    if (error)
        return std::nullopt;
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::clock_cast<std::chrono::system_clock>(written));
    const std::time_t raw = std::chrono::system_clock::to_time_t(system);
    std::tm utc{};
    if (gmtime_s(&utc, &raw) != 0)
        return std::nullopt;
    return UtcTime{utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec};
}

// The last session's pair moves into the archive under its start and end,
// and the archive is pruned to kSessionsKept. Before either file opens, since
// both open truncated; what it has to say goes into `notes`, written once the
// log is open.
void ArchivePrevious(const std::filesystem::path &directory, std::vector<std::string> &notes)
{
    namespace fs = std::filesystem;

    std::optional<UtcTime> start;
    std::optional<UtcTime> end;
    for (const auto &[name, ending] : kSessionFiles)
    {
        const fs::path file = directory / name;
        std::error_code missing;
        if (!fs::exists(file, missing))
            continue;
        if (!start)
            start = SessionStart(FirstLine(file));
        if (const auto written = WrittenAt(file); written && (!end || *end < *written))
            end = written;
    }
    if (!end)
        return;

    const fs::path archive = directory / "FollowerTactics";
    std::error_code made;
    fs::create_directories(archive, made);
    const std::string stem = ArchiveStem(start, *end);
    for (const auto &[name, ending] : kSessionFiles)
    {
        const fs::path file = directory / name;
        std::error_code missing;
        if (!fs::exists(file, missing))
            continue;
        const fs::path target = archive / (stem + std::string(ending));
        std::error_code moved;
        fs::rename(file, target, moved);
        if (!moved)
            continue;
        // Held open by a program that does not share deletion, an editor say:
        // a copy can still be kept, and opening the file truncates it as before.
        std::error_code copied;
        fs::copy_file(file, target, fs::copy_options::none, copied);
        notes.push_back(copied
                            ? fmt::format("{} could not be archived ({}) -- it is overwritten", name, moved.message())
                            : fmt::format("{} could not be moved ({}) -- copied to {} instead", name, moved.message(),
                                          target.filename().string()));
    }

    std::vector<std::string> stems;
    std::error_code listed;
    for (fs::directory_iterator it(archive, listed), done; !listed && it != done; it.increment(listed))
    {
        const std::string file = it->path().filename().string();
        for (const auto &[name, ending] : kSessionFiles)
        {
            if (file.ends_with(ending))
            {
                stems.push_back(file.substr(0, file.size() - ending.size()));
                break;
            }
        }
    }
    for (const std::string &old : StemsToDelete(std::move(stems), kSessionsKept))
    {
        for (const auto &[name, ending] : kSessionFiles)
        {
            std::error_code removed;
            fs::remove(archive / (old + std::string(ending)), removed);
        }
    }
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
    const Settings settings = ReadSettings(notes);

    const auto directory = SKSE::log::log_directory();
    if (!directory)
        return;

    ArchivePrevious(*directory, notes);
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
