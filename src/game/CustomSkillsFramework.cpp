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
    // Each node's perk, or none where the load order has none: such a node
    // is left out, and the links to it with it (KeptTree).
    std::vector<RE::BGSPerk *> perks;
    for (const CustomSkillNode &node : skill.nodes)
    {
        auto *form = node.perk ? Resolve(*node.perk) : nullptr;
        perks.push_back(form ? form->As<RE::BGSPerk>() : nullptr);
        if (!perks.back())
            log::customskills.debug("{}: {} node {} names no perk in the load order", file, tree.name, node.id);
    }
    for (const KeptNode &kept : KeptTree(PlacesOf(skill), [&perks](std::size_t i) { return perks[i] != nullptr; }))
    {
        const CustomSkillNode &node = skill.nodes[kept.source];
        // A node names the first rank; the rest chain through nextPerk,
        // walked by the same core function the vanilla trees walk with.
        tree.nodes.push_back({ft::RankChain(perks[kept.source], [](RE::BGSPerk *rank) { return rank->nextPerk; }),
                              node.x, node.y, kept.children});
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
            log::customskills.info("{}: {}, {} perks", label, tree.name, tree.nodes.size());
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
