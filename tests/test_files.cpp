// Which files a folder gives up, and in what order, against a real folder.

#include <catch2/catch_test_macros.hpp>

#include "core/Files.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace ft;

namespace
{

struct Folder
{
    fs::path path;
    Folder() : path(fs::temp_directory_path() / ("ft-files-" + std::to_string(std::random_device{}())))
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

    // Binary: the bytes the file holds are the bytes written, as
    // another mod's file would have them.
    void Write(const std::string &name, const std::string &text) const
    {
        std::ofstream(path / name, std::ios::binary) << text;
    }
};

std::vector<std::string> Names(const std::vector<fs::path> &files)
{
    std::vector<std::string> names;
    for (const fs::path &file : files)
        names.push_back(file.filename().string());
    return names;
}

} // namespace

TEST_CASE("every json file in the folder, by name, whatever the extension's case", "[files]")
{
    const Folder folder;
    folder.Write("zeta.json", "{}");
    folder.Write("Alpha.JSON", "{}");
    folder.Write("middle.Json", "{}");
    // Not files of ours, and not a file at all.
    folder.Write("notes.txt", "");
    folder.Write("tree.json.bak", "");
    fs::create_directories(folder.path / "nested.json");
    folder.Write("nested.json/inside.json", "{}");

    REQUIRE(Names(JsonFilesIn(folder.path)) == std::vector<std::string>{"Alpha.JSON", "middle.Json", "zeta.json"});
}

TEST_CASE("a folder that is not there, or is a file, gives nothing", "[files]")
{
    const Folder folder;
    REQUIRE(JsonFilesIn(folder.path / "missing").empty());
    folder.Write("a-file", "");
    REQUIRE(JsonFilesIn(folder.path / "a-file").empty());
    // An empty folder is not an error either.
    REQUIRE(JsonFilesIn(folder.path).empty());
}

TEST_CASE("a file is read whole, and one that is not there reads as nothing", "[files]")
{
    const Folder folder;
    folder.Write("one.json", "{\"id\":\"a\"}\nsecond line\n");
    REQUIRE(ReadText(folder.path / "one.json") == "{\"id\":\"a\"}\nsecond line\n");
    REQUIRE(ReadText(folder.path / "missing.json").empty());
    REQUIRE(FileLabel(folder.path / "one.json") == "one.json");
}

TEST_CASE("every file name has a label of its own", "[files]")
{
    // Past ASCII: in UTF-8.
    REQUIRE(FileLabel(std::filesystem::path(L"Dr\u00e4chenblut.json")) == "Dr\xC3\xA4"
                                                                          "chenblut.json");
    // Not valid UTF-16, which no conversion takes: the odd unit written out,
    // so two such names are still two labels.
    const std::wstring lone{L'\xD800'};
    const std::wstring other{L'\xDC00'};
    REQUIRE(FileLabel(std::filesystem::path(lone + L".json")) == "\\uD800.json");
    REQUIRE(FileLabel(std::filesystem::path(lone + L".json")) != FileLabel(std::filesystem::path(other + L".json")));
}
