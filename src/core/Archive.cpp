#include "core/Archive.h"

#include <chrono>
#include <ctime>
#include <fstream>

namespace ft::log
{
namespace
{

[[nodiscard]] std::string FirstLine(const std::filesystem::path &file)
{
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    return line;
}

} // namespace

std::optional<UtcTime> WrittenAt(const std::filesystem::path &file)
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

void ArchiveSession(const std::filesystem::path &directory, std::vector<std::string> &notes, std::size_t keep)
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

    const fs::path archive = directory / kArchiveFolder;
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
        notes.push_back(copied ? std::string(name) + " could not be archived (" + moved.message() +
                                     ") -- it is overwritten"
                               : std::string(name) + " could not be moved (" + moved.message() + ") -- copied to " +
                                     target.filename().string() + " instead");
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
    for (const std::string &old : StemsToDelete(std::move(stems), keep))
    {
        for (const auto &[name, ending] : kSessionFiles)
        {
            std::error_code removed;
            fs::remove(archive / (old + std::string(ending)), removed);
        }
    }
}

} // namespace ft::log
