#include "Sessions.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <utility>

namespace ft::log
{
namespace
{

constexpr std::string_view kStemPrefix = "FollowerTactics-";

// "YYYY?MM?DD?HH?MM?SS", the length both spellings of a time share.
constexpr std::size_t kTimeLength = 19;

[[nodiscard]] std::optional<int> Digits(std::string_view text, std::size_t at, std::size_t count) noexcept
{
    if (at + count > text.size())
        return std::nullopt;
    int value = 0;
    for (std::size_t i = at; i < at + count; ++i)
    {
        const char c = text[i];
        if (c < '0' || c > '9')
            return std::nullopt;
        value = value * 10 + (c - '0');
    }
    return value;
}

// The first kTimeLength characters as a time, with `date` between the date's
// parts, `middle` between date and time, and `time` between the time's.
[[nodiscard]] std::optional<UtcTime> ParseTime(std::string_view text, char date, char middle, char time) noexcept
{
    if (text.size() < kTimeLength || text[4] != date || text[7] != date || text[10] != middle || text[13] != time ||
        text[16] != time)
        return std::nullopt;
    const auto year = Digits(text, 0, 4);
    const auto month = Digits(text, 5, 2);
    const auto day = Digits(text, 8, 2);
    const auto hour = Digits(text, 11, 2);
    const auto minute = Digits(text, 14, 2);
    const auto second = Digits(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second)
        return std::nullopt;
    const UtcTime parsed{*year, *month, *day, *hour, *minute, *second};
    if (parsed.month < 1 || parsed.month > 12 || parsed.day < 1 || parsed.day > 31 || parsed.hour > 23 ||
        parsed.minute > 59 || parsed.second > 60)
        return std::nullopt;
    return parsed;
}

[[nodiscard]] std::optional<UtcTime> ParseStamp(std::string_view text) noexcept
{
    return text.size() == kTimeLength ? ParseTime(text, '-', '-', '-') : std::nullopt;
}

} // namespace

std::optional<UtcTime> ParseIsoTime(std::string_view text) noexcept
{
    const auto parsed = ParseTime(text, '-', 'T', ':');
    if (!parsed)
        return std::nullopt;
    std::string_view rest = text.substr(kTimeLength);
    if (!rest.empty() && rest.front() == '.')
    {
        rest.remove_prefix(1);
        while (!rest.empty() && rest.front() >= '0' && rest.front() <= '9')
            rest.remove_prefix(1);
    }
    if (!rest.empty() && rest.front() == 'Z')
        rest.remove_prefix(1);
    return rest.empty() ? parsed : std::nullopt;
}

std::optional<UtcTime> SessionStart(std::string_view firstLine)
{
    // The sink ends a line with \r\n on Windows, and getline leaves the \r.
    while (!firstLine.empty() && (firstLine.back() == '\r' || firstLine.back() == '\n' || firstLine.back() == ' '))
        firstLine.remove_suffix(1);

    if (!firstLine.empty() && firstLine.front() == '{')
    {
        const auto line = nlohmann::json::parse(firstLine.begin(), firstLine.end(), nullptr, false);
        if (!line.is_object())
            return std::nullopt;
        const auto event = line.find("event");
        const auto ts = line.find("ts");
        if (event == line.end() || !event->is_string() || event->get_ref<const std::string &>() != "session.started" ||
            ts == line.end() || !ts->is_string())
            return std::nullopt;
        return ParseIsoTime(ts->get_ref<const std::string &>());
    }

    const auto at = firstLine.find(kSessionBegan);
    if (at == std::string_view::npos)
        return std::nullopt;
    return ParseIsoTime(firstLine.substr(at + kSessionBegan.size()));
}

std::string Stamp(const UtcTime &time)
{
    std::array<char, 80> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%04d-%02d-%02d-%02d-%02d-%02d", time.year,
                                      time.month, time.day, time.hour, time.minute, time.second);
    if (written <= 0)
        return {};
    return {buffer.data(), std::min(static_cast<std::size_t>(written), buffer.size() - 1)};
}

std::string ArchiveStem(const std::optional<UtcTime> &start, const UtcTime &end)
{
    return std::string(kStemPrefix) + (start ? Stamp(*start) : std::string{}) + '_' + Stamp(end);
}

std::optional<UtcTime> ArchiveEnd(std::string_view stem) noexcept
{
    if (!stem.starts_with(kStemPrefix))
        return std::nullopt;
    const auto underscore = stem.rfind('_');
    if (underscore == std::string_view::npos)
        return std::nullopt;
    const std::string_view start = stem.substr(kStemPrefix.size(), underscore - kStemPrefix.size());
    if (!start.empty() && !ParseStamp(start))
        return std::nullopt;
    return ParseStamp(stem.substr(underscore + 1));
}

std::vector<std::string> StemsToDelete(std::vector<std::string> stems, std::size_t keep)
{
    std::erase_if(stems, [](const std::string &stem) { return !ArchiveEnd(stem); });
    std::sort(stems.begin(), stems.end(), [](const std::string &a, const std::string &b) {
        return std::pair(*ArchiveEnd(a), std::string_view(a)) < std::pair(*ArchiveEnd(b), std::string_view(b));
    });
    stems.erase(std::unique(stems.begin(), stems.end()), stems.end());
    if (stems.size() <= keep)
        return {};
    stems.resize(stems.size() - keep);
    return stems;
}

Room RoomFor(std::uint64_t before, std::uint64_t bytes, std::uint64_t ceiling) noexcept
{
    if (before + bytes <= ceiling)
        return Room::Write;
    return before <= ceiling ? Room::Last : Room::None;
}

} // namespace ft::log
