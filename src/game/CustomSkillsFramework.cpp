#include "game/CustomSkillsFramework.h"

#include "core/CustomSkills.h"
#include "core/Files.h"
#include "game/Log.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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
        // walked by the same core function the vanilla trees walk with.
        const std::vector<RE::BGSPerk *> chain = ft::RankChain(perk, [](RE::BGSPerk *rank) { return rank->nextPerk; });
        for (std::size_t r = 0; r < chain.size(); ++r)
            tree.perks.push_back({chain[r], static_cast<int>(r) + 1, static_cast<int>(chain.size())});
    }
    return tree;
}

// Every tree the files add. The files are another mod's, so nothing in them
// may take the game down: which files and in what order is core's
// (core/Files.h), and that a file which will not parse, or a skill which
// will not build, costs only itself is core's too (LoadSkillTrees, tested).
// This reads a file and resolves the forms it names.
std::vector<CustomSkillTree> Load()
{
    return ft::LoadSkillTrees(
        ft::JsonFilesIn(std::filesystem::path{"Data/SKSE/Plugins/CustomSkills"}),
        [](const std::filesystem::path &file) { return ft::ReadText(file); },
        [](const CustomSkill &skill, const std::string &label) {
            CustomSkillTree tree = TreeOf(skill, label);
            log::customskills.info("{}: {}, {} perks", label, tree.name, tree.perks.size());
            return tree;
        },
        [](const std::string &label, std::string_view skill, std::string_view why) {
            if (skill.empty())
                log::customskills.warn("{}: left out, {}", label, why);
            else
                log::customskills.warn("{}: skill {} left out, {}", label, skill, why);
        });
}

} // namespace

const std::vector<CustomSkillTree> &CustomSkillTrees()
{
    static const std::vector<CustomSkillTree> trees = Load();
    return trees;
}

} // namespace ft::game
