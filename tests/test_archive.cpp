// Moving a session's pair into the archive, and pruning it, against a
// real folder in the temporary directory. No Skyrim; the game passes the
// folder it logs into.

#include <catch2/catch_test_macros.hpp>

#include "core/Archive.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ft::log;

namespace
{

// A folder of its own per test, removed with it.
struct Folder
{
    fs::path path;
    explicit Folder(const std::string &name)
        : path(fs::temp_directory_path() / ("ft-archive-" + name + "-" + std::to_string(std::random_device{}())))
    {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Folder()
    {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
    Folder(const Folder &) = delete;
    Folder &operator=(const Folder &) = delete;

    [[nodiscard]] fs::path Archive() const
    {
        return path / kArchiveFolder;
    }
    void Write(std::string_view name, const std::string &text) const
    {
        std::ofstream out(path / name);
        out << text;
    }
    [[nodiscard]] std::vector<std::string> Archived() const
    {
        std::vector<std::string> names;
        std::error_code listed;
        for (fs::directory_iterator it(Archive(), listed), done; !listed && it != done; it.increment(listed))
            names.push_back(it->path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }
};

// The prose log's first line, as the banner writes it.
std::string Banner(const std::string &iso)
{
    return "[00:00:00.000] [    1] [I] FollowerTactics v0.1.0 -- " + std::string(kSessionBegan) + iso;
}

} // namespace

TEST_CASE("a session's pair moves into the archive under its start and end", "[archive]")
{
    const Folder folder("pair");
    folder.Write(kSessionFiles[1].first, Banner("2026-09-14T19:53:02.100Z") + "\nmore\n");
    folder.Write(kSessionFiles[0].first, R"({"ts":"2026-09-14T19:53:02.100Z","event":"session.started"})"
                                         "\n");

    std::vector<std::string> notes;
    ArchiveSession(folder.path, notes);

    // Both files are gone from the log folder and are in the archive under
    // one stem, which begins with the session's start.
    REQUIRE_FALSE(fs::exists(folder.path / kSessionFiles[0].first));
    REQUIRE_FALSE(fs::exists(folder.path / kSessionFiles[1].first));
    const auto archived = folder.Archived();
    REQUIRE(archived.size() == 2);
    REQUIRE(archived[0].starts_with("FollowerTactics-2026-09-14-19-53-02_"));
    REQUIRE(archived[0].ends_with(".events.jsonl"));
    REQUIRE(archived[1].ends_with(".log"));
    // The pair shares its stem.
    REQUIRE(archived[0].substr(0, archived[0].size() - std::string(".events.jsonl").size()) ==
            archived[1].substr(0, archived[1].size() - std::string(".log").size()));
    REQUIRE(notes.empty());
}

TEST_CASE("a first run archives nothing, and a half pair is archived alone", "[archive]")
{
    const Folder empty("first-run");
    std::vector<std::string> notes;
    ArchiveSession(empty.path, notes);
    // Nothing was written, so there is not even an archive folder.
    REQUIRE_FALSE(fs::exists(empty.Archive()));
    REQUIRE(notes.empty());

    // Only the prose log: it goes alone, and still under a dated name.
    const Folder half("half");
    half.Write(kSessionFiles[1].first, Banner("2026-09-14T19:53:02.100Z") + "\n");
    ArchiveSession(half.path, notes);
    const auto archived = half.Archived();
    REQUIRE(archived.size() == 1);
    REQUIRE(archived[0].ends_with(".log"));
    REQUIRE(archived[0].starts_with("FollowerTactics-2026-09-14-19-53-02_"));
}

TEST_CASE("a session whose start cannot be read is archived by its end alone", "[archive]")
{
    const Folder folder("no-start");
    folder.Write(kSessionFiles[1].first, "a line from before sessions were dated\n");
    std::vector<std::string> notes;
    ArchiveSession(folder.path, notes);
    const auto archived = folder.Archived();
    REQUIRE(archived.size() == 1);
    REQUIRE(archived[0].starts_with("FollowerTactics-_"));
}

TEST_CASE("the archive keeps the last sessions and prunes the rest, leaving other files alone", "[archive]")
{
    const Folder folder("prune");
    fs::create_directories(folder.Archive());
    // Six sessions already archived, and two files this never made.
    for (int day = 1; day <= 6; ++day)
    {
        const std::string stem = "FollowerTactics-2026-09-0" + std::to_string(day) + "-10-00-00_2026-09-0" +
                                 std::to_string(day) + "-11-00-00";
        for (const auto &[name, ending] : kSessionFiles)
            std::ofstream(folder.Archive() / (stem + std::string(ending))) << "old\n";
    }
    std::ofstream(folder.Archive() / "crash-2026-09-04-06-39-15.log") << "someone else's\n";
    std::ofstream(folder.Archive() / "notes.txt") << "mine\n";

    // This session's pair arrives; three sessions are kept.
    folder.Write(kSessionFiles[1].first, Banner("2026-09-14T19:53:02.100Z") + "\n");
    folder.Write(kSessionFiles[0].first, R"({"ts":"2026-09-14T19:53:02.100Z","event":"session.started"})"
                                         "\n");
    std::vector<std::string> notes;
    ArchiveSession(folder.path, notes, 3);

    const auto archived = folder.Archived();
    // Three sessions of two files, and the two files this did not make.
    REQUIRE(archived.size() == 3 * 2 + 2);
    // The earliest sessions went; the latest and this one stayed.
    for (const std::string &name : archived)
    {
        REQUIRE(name.find("2026-09-01") == std::string::npos);
        REQUIRE(name.find("2026-09-02") == std::string::npos);
        REQUIRE(name.find("2026-09-03") == std::string::npos);
    }
    REQUIRE(std::any_of(archived.begin(), archived.end(),
                        [](const std::string &n) { return n.find("2026-09-06") != std::string::npos; }));
    REQUIRE(std::any_of(archived.begin(), archived.end(),
                        [](const std::string &n) { return n.find("2026-09-14-19-53-02") != std::string::npos; }));
    // Never ours to delete.
    REQUIRE(fs::exists(folder.Archive() / "crash-2026-09-04-06-39-15.log"));
    REQUIRE(fs::exists(folder.Archive() / "notes.txt"));
}

TEST_CASE("an archived pair keeps what the session wrote", "[archive]")
{
    const Folder folder("contents");
    folder.Write(kSessionFiles[1].first, Banner("2026-09-14T19:53:02.100Z") + "\nthe second line\n");
    std::vector<std::string> notes;
    ArchiveSession(folder.path, notes);
    const auto archived = folder.Archived();
    REQUIRE(archived.size() == 1);
    std::ifstream in(folder.Archive() / archived[0]);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(text.find("the second line") != std::string::npos);
}
