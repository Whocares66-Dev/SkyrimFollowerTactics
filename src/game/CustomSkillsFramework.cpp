#include "game/CustomSkillsFramework.h"

#include "core/CustomSkills.h"
#include "game/Log.h"

#include <algorithm>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ft::game
{
namespace
{

RE::TESForm *Resolve(const FormRef &ref)
{
    auto *handler = RE::TESDataHandler::GetSingleton();
    return handler ? handler->LookupForm(ref.id, ref.plugin) : nullptr;
}

// A name given as a translation key, "$CampingPlusPlus_Name", in the game's
// language, from the strings SKSE loads from Interface/Translations; the name
// as written where there is no key or no string for it.
std::string Translated(const std::string &name)
{
    std::string out;
    return SKSE::Translation::Translate(name, out) ? out : name;
}

// Wide throughout: a file name the narrow conversion cannot hold would throw.
bool IsJsonFile(const std::filesystem::path &path)
{
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return extension == L".json";
}

std::string FileLabel(const std::filesystem::path &path)
{
    try
    {
        return path.filename().string();
    }
    catch (const std::exception &)
    {
        return "(a file whose name does not convert)";
    }
}

CustomSkillTree TreeOf(const CustomSkill &skill, const std::string &file)
{
    CustomSkillTree tree;
    tree.name = Translated(skill.name.empty() ? skill.id : skill.name);
    if (auto *form = skill.level ? Resolve(*skill.level) : nullptr)
        tree.level = form->As<RE::TESGlobal>();
    for (const std::size_t i : TreeOrder(PlacesOf(skill)))
    {
        const CustomSkillNode &node = skill.nodes[i];
        auto *form = node.perk ? Resolve(*node.perk) : nullptr;
        auto *perk = form ? form->As<RE::BGSPerk>() : nullptr;
        if (!perk)
        {
            log::customskills.debug("{}: {} node {} names no perk in the load order", file, tree.name, node.id);
            continue;
        }
        // A node names the first rank; the rest chain through nextPerk,
        // bounded as the vanilla trees' walk bounds it.
        std::vector<RE::BGSPerk *> chain;
        for (auto *rank = perk; rank && chain.size() < 16; rank = rank->nextPerk)
            chain.push_back(rank);
        for (std::size_t r = 0; r < chain.size(); ++r)
            tree.perks.push_back({chain[r], static_cast<int>(r) + 1, static_cast<int>(chain.size())});
    }
    return tree;
}

// Every tree the files add. The files are another mod's, so nothing in them
// may take the game down: a file that will not read, or a skill that will not
// resolve, is logged and left out, and the rest load.
std::vector<CustomSkillTree> Load()
{
    namespace fs = std::filesystem;
    std::vector<CustomSkillTree> out;
    const fs::path directory{"Data/SKSE/Plugins/CustomSkills"};
    std::error_code error;
    if (!fs::is_directory(directory, error))
        return out;

    // The non-throwing increment: a range-for's would throw on an error.
    std::vector<fs::path> files;
    fs::directory_iterator it(directory, error);
    for (; !error && it != fs::directory_iterator(); it.increment(error))
    {
        std::error_code ignored;
        if (it->is_regular_file(ignored) && IsJsonFile(it->path()))
            files.push_back(it->path());
    }
    // By name: the file system's own order is no order at all.
    std::sort(files.begin(), files.end());

    for (const fs::path &file : files)
    {
        const std::string label = FileLabel(file);
        std::vector<CustomSkill> skills;
        try
        {
            std::ifstream in(file, std::ios::binary);
            std::stringstream text;
            text << in.rdbuf();
            std::string parseError;
            skills = ParseCustomSkills(text.str(), &parseError);
            if (!parseError.empty())
            {
                log::customskills.warn("{}: left out, {}", label, parseError);
                continue;
            }
        }
        catch (const std::exception &e)
        {
            log::customskills.warn("{}: left out, {}", label, e.what());
            continue;
        }
        for (const CustomSkill &skill : skills)
        {
            try
            {
                CustomSkillTree tree = TreeOf(skill, label);
                log::customskills.info("{}: {}, {} perks", label, tree.name, tree.perks.size());
                out.push_back(std::move(tree));
            }
            catch (const std::exception &e)
            {
                log::customskills.warn("{}: skill {} left out, {}", label, skill.id, e.what());
            }
        }
    }
    return out;
}

} // namespace

const std::vector<CustomSkillTree> &CustomSkillTrees()
{
    static const std::vector<CustomSkillTree> trees = Load();
    return trees;
}

} // namespace ft::game
