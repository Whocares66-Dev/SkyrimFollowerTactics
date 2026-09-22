#include "core/LogSettings.h"

namespace ft::log
{
namespace
{

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

} // namespace

IniSettings ParseIniSettings(std::string_view text, std::vector<std::string> &notes)
{
    IniSettings settings;
    std::string section;
    while (!text.empty())
    {
        const auto newline = text.find('\n');
        const std::string_view raw = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);

        const std::string_view line = Trim(raw);
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

        if (section == "Interface" && key == "language")
        {
            settings.language = value == "auto" ? std::string{} : std::string(value);
            continue;
        }
        if (section != "Log")
            continue;

        if (key == "level")
        {
            if (!ParseLevel(value, settings.level))
                notes.push_back("level \"" + std::string(value) + "\" is not one of error/warn/info/debug -- using " +
                                ToString(settings.level));
        }
        else if (key == "events")
        {
            if (IsTrue(value))
                settings.events = true;
            else if (IsFalse(value))
                settings.events = false;
            else
                notes.push_back("events \"" + std::string(value) + "\" is not true or false -- using " +
                                (settings.events ? "true" : "false"));
        }
    }
    return settings;
}

} // namespace ft::log
