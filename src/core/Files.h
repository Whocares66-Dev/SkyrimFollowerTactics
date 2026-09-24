#pragma once
// Finding the files another mod leaves for us to read, and reading one.
// The walk is here rather than beside the records it feeds (the custom
// skills loader, game/CustomSkillsFramework.cpp) so the order it hands
// files over in, and what it leaves out, can be tested against a real
// folder. No Skyrim.

#include <filesystem>
#include <string>
#include <vector>

namespace ft
{

// Every .json file directly in the folder, by name -- the file system's
// own order is no order at all, and a mod's trees would otherwise load
// differently on two machines. The extension is matched whatever its
// case; a folder, and anything below it, is not a file. A folder that is
// not there gives nothing, as does one that cannot be walked: these are
// another mod's files, and nothing in them may take the game down.
[[nodiscard]] std::vector<std::filesystem::path> JsonFilesIn(const std::filesystem::path &directory);

// The whole of a file, as bytes; empty when it cannot be read.
[[nodiscard]] std::string ReadText(const std::filesystem::path &file);

// A file's name, in UTF-8: for the log, and, one folder's files having one
// name each, what tells them apart. A name that is not valid UTF-16 is
// written out unit by unit (\uD800), so no two files share one.
[[nodiscard]] std::string FileLabel(const std::filesystem::path &file);

} // namespace ft
