#include "core/Files.h"

#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace ft
{
namespace
{

// Wide throughout: a file name the narrow conversion cannot hold would
// throw.
[[nodiscard]] bool IsJsonFile(const std::filesystem::path &path)
{
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".json";
}

} // namespace

std::vector<std::filesystem::path> JsonFilesIn(const std::filesystem::path &directory)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    std::error_code error;
    if (!fs::is_directory(directory, error))
        return files;

    // The non-throwing increment: a range-for's would throw on an error.
    fs::directory_iterator it(directory, error);
    for (; !error && it != fs::directory_iterator(); it.increment(error))
    {
        std::error_code ignored;
        if (it->is_regular_file(ignored) && IsJsonFile(it->path()))
            files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string ReadText(const std::filesystem::path &file)
{
    try
    {
        std::ifstream in(file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return text.str();
    }
    catch (const std::exception &)
    {
        return {};
    }
}

std::string FileLabel(const std::filesystem::path &file)
{
    try
    {
        const std::u8string name = file.filename().u8string();
        return {name.begin(), name.end()};
    }
    catch (const std::exception &)
    {
        // Not valid UTF-16 -- an unpaired surrogate -- so written out a unit
        // at a time, which keeps it apart from every other name.
        std::string out;
        for (const wchar_t unit : file.filename().wstring())
        {
            if (unit < 0x80)
            {
                out += static_cast<char>(unit);
                continue;
            }
            char hex[8];
            std::snprintf(hex, sizeof(hex), "\\u%04X", static_cast<unsigned>(unit));
            out += hex;
        }
        return out;
    }
}

} // namespace ft
