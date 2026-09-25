#include "game/PerkSheet.h"

#include "game/Sensors.h"

#include "game/Sheet.h"

#include "core/Blows.h"
#include "core/CustomSkills.h"
#include "core/Effects.h"
#include "core/I18n.h"
#include "core/Party.h"
#include "core/Reach.h"
#include "core/Spells.h"
#include "core/Vocabulary.h"

#include "game/CustomSkillsFramework.h"
#include "game/EffectRows.h"
#include "game/Effects.h"
#include "game/Hits.h"
#include "game/Inventory.h"
#include "game/Log.h"
#include "game/Magic.h"
#include "game/Packages.h"
#include "game/Pins.h"
#include "game/Settings.h"
#include "game/Toggles.h"
#include "game/Util.h"
#include "game/Values.h"
#include "progression/game/Service.h"
#include "progression/game/ValueView.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

using ft::i18n::Tr;
using ft::i18n::TrFormat;

namespace ft::game
{

namespace
{

// --- perks -------------------------------------------------------------------

struct TreePerk
{
    RE::BGSPerk *perk;
    int rank;          // 1-based position in the perk's rank chain
    int ranks;         // length of that chain
    float requirement; // the skill level the perk asks for; 0 if it asks nothing
    std::string description;
    int order{0}; // its node's place in the tree's own order (ft::TreeOrder)
};

// The skill level a perk requires, read from its own conditions. It is not a
// field on the record: the perk menu's "requires Archery 20" is a condition,
// GetBaseActorValue(Archery) >= 20, and the first perk in a tree has none at
// all. The highest such bound is what the menu shows, so that is what this
// returns.
float SkillRequirement(const RE::BGSPerk *perk, RE::ActorValue skill)
{
    float best = 0.0f;
    for (const auto *item = perk->perkConditions.head; item; item = item->next)
    {
        const auto &d = item->data;
        if (d.functionData.function != RE::FUNCTION_DATA::FunctionID::kGetBaseActorValue)
            continue;
        if (static_cast<RE::ActorValue>(reinterpret_cast<std::uintptr_t>(d.functionData.params[0])) != skill)
            continue;
        if (d.flags.global) // compared against a global, not a number
            continue;
        using Op = RE::CONDITION_ITEM_DATA::OpCode;
        if (d.flags.opCode != Op::kGreaterThanOrEqualTo && d.flags.opCode != Op::kGreaterThan)
            continue;
        best = (std::max)(best, d.comparisonValue.f);
    }
    return best;
}

// Every perk in a skill's tree, ranks included, found by walking the same
// tree the perk menu draws. Walked once per skill and kept, description and
// requirement included: all of it is static data, and this is called from
// the tick, where reading a description every half second for every perk of
// every follower would be the only real cost on the sheet.
const std::vector<TreePerk> &TreePerks(RE::ActorValue skill)
{
    static std::unordered_map<RE::ActorValue, std::vector<TreePerk>> cache;
    if (const auto it = cache.find(skill); it != cache.end())
        return it->second;

    std::vector<TreePerk> out;
    auto *list = RE::ActorValueList::GetSingleton();
    auto *info = list ? list->GetActorValueInfo(skill) : nullptr;
    if (info && info->perkTree)
    {
        std::vector<RE::BGSSkillPerkTreeNode *> nodes;
        std::unordered_map<const RE::BGSSkillPerkTreeNode *, std::size_t> index;
        std::vector<RE::BGSSkillPerkTreeNode *> stack{info->perkTree};
        while (!stack.empty())
        {
            auto *node = stack.back();
            stack.pop_back();
            if (!node || index.contains(node))
                continue;
            index.emplace(node, nodes.size());
            nodes.push_back(node);
            for (auto *child : node->children)
                stack.push_back(child);
        }

        // Where the menu draws each node across -- its grid column plus its
        // offset within it -- and whom it leads to, for the tree's order.
        // The first node of a tree is a root with no perk and nonsense in its
        // grid; it lists nothing, so its place is harmless.
        std::vector<ft::TreeNodePlace> places(nodes.size());
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            places[i].x = static_cast<double>(nodes[i]->perkGridX) + nodes[i]->horizontalPosition;
            for (auto *child : nodes[i]->children)
            {
                if (const auto it = index.find(child); it != index.end())
                    places[i].children.push_back(it->second);
            }
        }
        const std::vector<std::size_t> order = ft::TreeOrder(places);

        for (std::size_t place = 0; place < order.size(); ++place)
        {
            const auto *node = nodes[order[place]];
            // A node names the first rank; the rest chain through nextPerk,
            // walked and bounded by core (ft::RankChain, tested).
            const std::vector<RE::BGSPerk *> chain =
                ft::RankChain(node->perk, [](RE::BGSPerk *rank) { return rank->nextPerk; });
            for (std::size_t i = 0; i < chain.size(); ++i)
            {
                RE::BSString text;
                chain[i]->GetDescription(text, chain[i]);
                out.push_back({chain[i], static_cast<int>(i) + 1, static_cast<int>(chain.size()),
                               SkillRequirement(chain[i], skill), text.c_str() ? text.c_str() : "",
                               static_cast<int>(place)});
            }
        }
    }

    // Least demanding first: the requirement is the game's own statement of
    // how strong a perk is, so the list reads weakest to strongest. Perks
    // asking the same read as the tree does, by depth and then left to right
    // as the menu draws them (ft::TreeOrder); a chain's ranks in turn.
    std::sort(out.begin(), out.end(), [](const TreePerk &a, const TreePerk &b) {
        if (a.requirement != b.requirement)
            return a.requirement < b.requirement;
        if (a.order != b.order)
            return a.order < b.order;
        return a.rank < b.rank;
    });
    return cache.emplace(skill, std::move(out)).first->second;
}

// Whether a perk the actor holds does something for them right now.
// A perk has no on-off switch. The conditions on its record are what the
// skill tree asks before the player may take it -- the perk before it in
// the chain, a skill level -- and gate nothing once it is held; Augmented
// Frost given without Elementalist works. What gates the effect is the
// conditions on each entry, whose first tab is the perk's owner: a mod
// that hands every NPC its perks and gates them on a power writes those to
// fail until the power is taken. So a perk is active when any of its
// entries could fire: an entry-point entry with no owner conditions or
// with them met, or an ability or quest entry; and a perk with no entries
// at all, a marker for conditions elsewhere, counts as active.
const std::vector<std::string> &PerkReaders(const RE::BGSPerk *perk); // below

bool PerkActive(RE::Actor *actor, RE::BGSPerk *perk)
{
    if (!actor || !perk)
        return false;
    bool anyEntry = false;
    for (const auto *entry : perk->perkEntries)
    {
        if (!entry)
            continue;
        anyEntry = true;
        if (entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint)
            return true;
        const auto *point = static_cast<const RE::BGSEntryPointPerkEntry *>(entry);
        if (point->conditions.size() == 0 || !point->conditions[0] || point->conditions[0].IsTrue(actor, actor))
            return true;
    }
    // A perk with no entries is a marker for conditions elsewhere -- Skald,
    // which the Bard's perks and spells ask for -- or a stub nothing asks
    // for: Adamant leaves the vanilla third rank of Armsman with its name
    // and its "60% more damage" and no entries, and an NPC authored with
    // it holds a perk that does nothing (Teldryn Sero, 2026-09-13). The
    // load order tells the two apart.
    return !anyEntry && !PerkReaders(perk).empty();
}

// Why a perk held is set aside, for the row's grey: nothing, "Inactive"
// for one whose entries' conditions fail, "Does nothing" for one with no
// entries that nothing reads.
const char *PerkAside(RE::Actor *actor, RE::BGSPerk *perk)
{
    if (PerkActive(actor, perk))
        return nullptr;
    return perk && perk->perkEntries.empty() ? Tr("Does nothing") : Tr("Inactive");
}

} // namespace

std::string PerkName(const RE::BGSPerk *perk)
{
    const std::string raw = NameOr(perk, "");
    const auto first = raw.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return raw.substr(first, raw.find_last_not_of(' ') - first + 1);
}

namespace
{

// The records whose conditions ask HasPerk of a perk -- other perks'
// entries on any tab, spells' effects, magic effects -- by name, so a
// perk with no entries can say who reads it, or that nobody does. The
// whole load order once, on first use: a few thousand records, a few
// milliseconds, and the answer does not change while the game runs.
const std::vector<std::string> &PerkReaders(const RE::BGSPerk *perk)
{
    static const std::unordered_map<RE::FormID, std::vector<std::string>> readers = [] {
        std::unordered_map<RE::FormID, std::vector<std::string>> out;
        auto *handler = RE::TESDataHandler::GetSingleton();
        if (!handler)
            return out;
        const auto note = [&out](const RE::TESCondition &condition, const std::string &who) {
            for (const auto *item = condition.head; item; item = item->next)
            {
                if (item->data.functionData.function.get() != RE::FUNCTION_DATA::FunctionID::kHasPerk)
                    continue;
                const auto *asked = static_cast<const RE::BGSPerk *>(item->data.functionData.params[0]);
                if (!asked || who.empty())
                    continue;
                auto &names = out[asked->GetFormID()];
                if (std::find(names.begin(), names.end(), who) == names.end())
                    names.push_back(who);
            }
        };
        for (const auto *other : handler->GetFormArray<RE::BGSPerk>())
        {
            if (!other)
                continue;
            for (const auto *entry : other->perkEntries)
            {
                if (!entry || entry->GetType() != RE::PERK_ENTRY_TYPE::kEntryPoint)
                    continue;
                const auto *point = static_cast<const RE::BGSEntryPointPerkEntry *>(entry);
                for (std::uint32_t tab = 0; tab < point->conditions.size(); ++tab)
                    note(point->conditions[tab], PerkName(other));
            }
        }
        for (const auto *spell : handler->GetFormArray<RE::SpellItem>())
        {
            if (!spell)
                continue;
            // Not ResolvedEffects: the conditions are the effect's own and
            // need no base, so an unresolved effect's still count.
            for (const auto *effect : spell->effects)
                if (effect)
                    note(effect->conditions, NameOr(spell, ""));
        }
        for (const auto *effect : handler->GetFormArray<RE::EffectSetting>())
            if (effect)
                note(effect->conditions, NameOr(effect, ""));
        return out;
    }();
    static const std::vector<std::string> none;
    if (!perk)
        return none;
    const auto found = readers.find(perk->GetFormID());
    return found == readers.end() ? none : found->second;
}

// Held, and the rank a held chain shows: no higher rank held, which would
// get the row instead.
bool TopRankHeld(RE::Actor *actor, RE::BGSPerk *perk)
{
    return actor->HasPerk(perk) && !(perk->nextPerk && actor->HasPerk(perk->nextPerk));
}

SheetRow PerkRow(RE::Actor *actor, RE::BGSPerk *perk, int rank, int ranks, std::string description)
{
    std::string label = PerkName(perk);
    if (label.empty())
        label = "?";
    SheetRow row = Row(std::move(label), ranks > 1 ? std::to_string(rank) + "/" + std::to_string(ranks) : "");
    row.modifiers = std::move(description);
    row.form = perk->GetFormID(); // the name opens the perk's page
    if (const char *aside = PerkAside(actor, perk))
        row.aside = aside;
    return row;
}

// The perks this follower holds in one skill's tree, one row per perk at
// the highest rank held. Asked of the engine with HasPerk rather than read
// off their record, so a perk a mod granted at runtime counts the same as one
// they were authored with. Ordered by the skill level each perk asks for,
// weakest first; the modifiers column carries its own in-game description.
std::vector<SheetRow> OwnedPerks(RE::Actor *actor, RE::ActorValue skill)
{
    std::vector<SheetRow> rows;
    for (const TreePerk &entry : TreePerks(skill))
    {
        if (TopRankHeld(actor, entry.perk))
            rows.push_back(PerkRow(actor, entry.perk, entry.rank, entry.ranks, entry.description));
    }
    return rows;
}

// The same for a Custom Skills Framework tree, in the tree's order.
std::vector<SheetRow> OwnedPerks(RE::Actor *actor, const CustomSkillTree &tree)
{
    std::vector<SheetRow> rows;
    for (const CustomTreeNode &node : tree.nodes)
        for (std::size_t r = 0; r < node.ranks.size(); ++r)
        {
            RE::BGSPerk *perk = node.ranks[r];
            if (!TopRankHeld(actor, perk))
                continue;
            RE::BSString text;
            perk->GetDescription(text, perk);
            rows.push_back(PerkRow(actor, perk, static_cast<int>(r) + 1, static_cast<int>(node.ranks.size()),
                                   text.c_str() ? text.c_str() : ""));
        }
    return rows;
}

} // namespace

constexpr std::array<const char *, 92> kEntryPointNames{{
    "Calculate Weapon Damage",
    "Calculate My Critical Hit Chance",
    "Calculate My Critical Hit Damage",
    "Calculate Mine Explode Chance",
    "Adjust Limb Damage",
    "Adjust Book Skill Points",
    "Mod Recovered Health",
    "Get Should Attack",
    "Mod Buy Prices",
    "Add Leveled List On Death",
    "Get Max Carry Weight",
    "Mod Addiction Chance",
    "Mod Addiction Duration",
    "Mod Positive Chem Duration",
    "Activate",
    "Ignore Running During Detection",
    "Ignore Broken Lock",
    "Mod Enemy Critical Hit Chance",
    "Mod Sneak Attack Mult",
    "Mod Max Placeable Mines",
    "Mod Bow Zoom",
    "Mod Recover Arrow Chance",
    "Mod Skill Use",
    "Mod Telekinesis Distance",
    "Mod Telekinesis Damage Mult",
    "Mod Telekinesis Damage",
    "Mod Bashing Damage",
    "Mod Power Attack Stamina",
    "Mod Power Attack Damage",
    "Mod Spell Magnitude",
    "Mod Spell Duration",
    "Mod Secondary Value Weight",
    "Mod Armor Weight",
    "Mod Incoming Stagger",
    "Mod Target Stagger",
    "Mod Attack Damage",
    "Mod Incoming Damage",
    "Mod Target Damage Resistance",
    "Mod Spell Cost",
    "Mod Percent Blocked",
    "Mod Shield Deflect Arrow Chance",
    "Mod Incoming Spell Magnitude",
    "Mod Incoming Spell Duration",
    "Mod Player Intimidation",
    "Mod Player Reputation",
    "Mod Favor Points",
    "Mod Bribe Amount",
    "Mod Detection Light",
    "Mod Detection Movement",
    "Mod Soul Gem Recharge",
    "Set Sweep Attack",
    "Apply Combat Hit Spell",
    "Apply Bashing Spell",
    "Apply Reanimate Spell",
    "Set Boolean Graph Variable",
    "Mod Spell Casting Sound Event",
    "Mod Pickpocket Chance",
    "Mod Detection Sneak Skill",
    "Mod Falling Damage",
    "Mod Lockpick Sweet Spot",
    "Mod Sell Prices",
    "Can Pickpocket Equipped Item",
    "Mod Lockpick Level Allowed",
    "Set Lockpick Starting Arc",
    "Set Progression Picking",
    "Make Lockpicks Unbreakable",
    "Mod Alchemy Effectiveness",
    "Apply Weapon Swing Spell",
    "Mod Commanded Actor Limit",
    "Apply Sneaking Spell",
    "Mod Player Magic Slowdown",
    "Mod Ward Magicka Absorption Pct",
    "Mod Initial Ingredient Effects Learned",
    "Purify Alchemy Ingredients",
    "Filter Activation",
    "Can Dual Cast Spell",
    "Mod Tempering Health",
    "Mod Enchantment Power",
    "Mod Soul Pct Captured To Weapon",
    "Mod Soul Gem Enchanting",
    "Mod Number Applied Enchantments Allowed",
    "Set Activate Label",
    "Mod Shout OK",
    "Mod Poison Dose Count",
    "Should Apply Placed Item",
    "Mod Armor Rating",
    "Mod Lockpicking Crime Chance",
    "Mod Ingredients Harvested",
    "Mod Spell Range Target Loc",
    "Mod Potions Created",
    "Mod Lockpicking Key Reward Chance",
    "Allow Mount Actor",
}};

// One entry of a perk as a row: the entry point in the Creation Kit's
// words with what the function does to it, an ability by name, a quest
// by name and stage.
SheetRow EntryRow(const RE::BGSPerkEntry *entry)
{
    using Type = RE::PERK_ENTRY_TYPE;
    switch (entry->GetType())
    {
    case Type::kAbility: {
        const auto *ability = static_cast<const RE::BGSAbilityPerkEntry *>(entry);
        return Row(Tr("Ability"), NameOr(ability->ability, "?"));
    }
    case Type::kQuest:
        // The quest entry's record is not modelled in this CommonLibSSE
        // fork; the kind is all that can be said.
        return Row(Tr("Quest"), Tr("a stage set"));
    case Type::kEntryPoint: {
        const auto *point = static_cast<const RE::BGSEntryPointPerkEntry *>(entry);
        const auto index = static_cast<std::size_t>(point->entryData.entryPoint.get());
        const char *name = index < kEntryPointNames.size() ? kEntryPointNames[index] : "?";
        using Function = RE::BGSEntryPointPerkEntry::Function;
        using DataType = RE::BGSEntryPointFunctionData::ENTRY_POINT_FUNCTION_DATA;
        const auto *data = point->functionData;
        const auto dataType = data ? data->GetType() : DataType::kInvalid;
        const float one = dataType == DataType::kOneValue
                              ? static_cast<const RE::BGSEntryPointFunctionDataOneValue *>(data)->data
                              : 0.0f;
        // The two-value record: a range's ends, or for the actor-value
        // functions the value read and its multiplier -- named, so the
        // page and the breakdown that reads the same entry agree on what
        // "One Handed Power Mod x 0.01" is (2026-09-13).
        const float *two =
            dataType == DataType::kTwoValue ? reinterpret_cast<const TwoValueData *>(data)->data : nullptr;
        const std::string share =
            two ? Fmt("%g", two[1]) + " x " + ValueName(static_cast<RE::ActorValue>(static_cast<int>(two[0])))
                : std::string(Tr("a share of an actor value"));
        std::string value;
        switch (point->entryData.function.get())
        {
        case Function::kSetValue:
            value = "= " + Fmt("%g", one);
            break;
        case Function::kAddValue:
            value = Fmt("%+g", one);
            break;
        case Function::kMultiplyValue:
            value = "x " + Fmt("%g", one);
            break;
        case Function::kAddRangeToValue:
            value = two ? TrFormat("+ {} to {}", Fmt("%g", two[0]), Fmt("%g", two[1])) : std::string(Tr("+ a range"));
            break;
        case Function::kAddActorValueMult:
            value = "+ " + share;
            break;
        case Function::kAddLeveledList:
            value = Tr("a leveled list");
            break;
        case Function::kAddActivateChoice:
            value = Tr("an activate choice");
            break;
        case Function::kSetToActorValueMult:
            value = "= " + share;
            break;
        case Function::kMultiplyActorValueMult:
            value = "x " + share;
            break;
        case Function::kMultiplyOnePlusActorValueMult:
            value = "x (1 + " + share + ")";
            break;
        case Function::kSetText:
            value = Tr("a text");
            break;
        default:
            break;
        }
        if (dataType == DataType::kSpellItem)
        {
            const auto *spell = static_cast<const RE::BGSEntryPointFunctionDataSpellItem *>(data)->spell;
            value = NameOr(spell, Tr("a spell"));
        }
        return Row(name, value);
    }
    default:
        return Row(Tr("Entry"), "?");
    }
}
namespace
{

// A perk held that the skill trees' walk does not find, with its rank.
struct HeldPerk
{
    RE::BGSPerk *perk;
    int rank;
};

// The perks outside the trees. An NPC's are on the base record's list,
// where a distributor puts them too. The player's taken in play are on the
// player, not the record, in an array whose offset CommonLib marks as
// guessed on this runtime; so for the player every perk in the load order
// is asked of the engine instead, a few thousand lookups. Hidden perks stay
// out: the skill-boost perks every actor carries.
std::vector<HeldPerk> PerksOutsideTrees(RE::Actor *actor)
{
    std::vector<HeldPerk> out;
    const auto held = [actor](RE::BGSPerk *perk) { return perk && !perk->data.hidden && actor->HasPerk(perk); };
    if (actor->IsPlayerRef())
    {
        auto *handler = RE::TESDataHandler::GetSingleton();
        if (!handler)
            return out;
        for (auto *perk : handler->GetFormArray<RE::BGSPerk>())
        {
            // One row per chain, at the highest rank held, as the trees do.
            if (held(perk) && !(perk->nextPerk && actor->HasPerk(perk->nextPerk)))
                out.push_back({perk, 1});
        }
        return out;
    }
    if (const auto *base = actor->GetActorBase(); base && base->perks)
        for (std::uint32_t i = 0; i < base->perkCount; ++i)
            if (held(base->perks[i].perk))
                out.push_back({base->perks[i].perk, base->perks[i].currentRank});
    return out;
}

// A skill's tree as the menu draws it, walked once per skill and kept, as
// TreePerks is: the positions, links, names, descriptions and requirements
// are the load order's. The actor's part, which ranks they hold, is read per
// page. The root, which names no perk, is left out with its links.
struct TreeShape
{
    struct Node
    {
        std::string name;
        double x{0.0};
        double y{0.0};
        std::vector<RE::BGSPerk *> ranks;
        std::vector<std::string> descriptions; // a rank's each
        std::vector<float> requirements;
        std::vector<std::size_t> children;
    };
    std::vector<Node> nodes;
};

const TreeShape &ShapeOf(RE::ActorValue skill)
{
    static std::unordered_map<RE::ActorValue, TreeShape> cache;
    if (const auto it = cache.find(skill); it != cache.end())
        return it->second;

    TreeShape shape;
    auto *list = RE::ActorValueList::GetSingleton();
    auto *info = list ? list->GetActorValueInfo(skill) : nullptr;
    if (info && info->perkTree)
    {
        // Every node that names a perk, once, each given its index here.
        std::vector<RE::BGSSkillPerkTreeNode *> nodes;
        std::unordered_map<const RE::BGSSkillPerkTreeNode *, std::size_t> index;
        std::unordered_set<const RE::BGSSkillPerkTreeNode *> seen;
        std::vector<RE::BGSSkillPerkTreeNode *> stack{info->perkTree};
        while (!stack.empty())
        {
            auto *node = stack.back();
            stack.pop_back();
            if (!node || !seen.insert(node).second)
                continue;
            if (node->perk)
            {
                index.emplace(node, nodes.size());
                nodes.push_back(node);
            }
            for (auto *child : node->children)
                stack.push_back(child);
        }
        for (auto *node : nodes)
        {
            TreeShape::Node out;
            out.name = PerkName(node->perk);
            out.x = static_cast<double>(node->perkGridX) + node->horizontalPosition;
            out.y = static_cast<double>(node->perkGridY) + node->verticalPosition;
            out.ranks = ft::RankChain(node->perk, [](RE::BGSPerk *rank) { return rank->nextPerk; });
            for (RE::BGSPerk *rank : out.ranks)
            {
                RE::BSString text;
                rank->GetDescription(text, rank);
                out.descriptions.emplace_back(text.c_str() ? text.c_str() : "");
                out.requirements.push_back(SkillRequirement(rank, skill));
            }
            for (auto *child : node->children)
                if (const auto it = index.find(child); it != index.end())
                    out.children.push_back(it->second);
            shape.nodes.push_back(std::move(out));
        }
    }
    return cache.emplace(skill, std::move(shape)).first->second;
}

// A Custom Skills Framework tree's shape, kept as a skill's is. A node's
// requirements are not read: the framework keeps a tree's level in a
// global, and no installed tree has one to measure what its perks ask of
// it. With none, the tree is drawn at the file's own places, as the
// framework's menu draws it (core/PerkTree.h).
const TreeShape &ShapeOf(const CustomSkillTree &tree)
{
    static std::unordered_map<const CustomSkillTree *, TreeShape> cache;
    if (const auto it = cache.find(&tree); it != cache.end())
        return it->second;

    TreeShape shape;
    for (const CustomTreeNode &node : tree.nodes)
    {
        TreeShape::Node out;
        out.name = PerkName(node.ranks.front());
        out.x = node.x;
        out.y = node.y;
        out.ranks = node.ranks;
        for (RE::BGSPerk *rank : node.ranks)
        {
            RE::BSString text;
            rank->GetDescription(text, rank);
            out.descriptions.emplace_back(text.c_str() ? text.c_str() : "");
        }
        out.requirements.assign(node.ranks.size(), 0.0f);
        out.children = node.children;
        shape.nodes.push_back(std::move(out));
    }
    return cache.emplace(&tree, std::move(shape)).first->second;
}

// A custom tree's key, the skill row's and the tree page's: the panel's
// handle for this session, never saved -- the tree is known by its id --
// and clear of every skill's, which is its actor value plus one.
std::uint32_t CustomTreeKey(std::size_t index)
{
    return 0x10000u + static_cast<std::uint32_t>(index);
}

// A tree's nodes for an actor: its shape, with what they hold of each node
// and what their own record gives them.
std::vector<ft::PerkTreeNode> NodesFor(RE::Actor *actor, const TreeShape &shape)
{
    std::unordered_set<const RE::BGSPerk *> record;
    if (const auto *npc = actor->GetActorBase(); npc && npc->perks)
        for (std::uint32_t k = 0; k < npc->perkCount; ++k)
            record.insert(npc->perks[k].perk);
    std::vector<ft::PerkTreeNode> nodes;
    for (const TreeShape::Node &node : shape.nodes)
    {
        ft::PerkTreeNode n;
        n.name = node.name.empty() ? "?" : node.name;
        n.x = node.x;
        n.y = node.y;
        n.ranks = static_cast<int>(node.ranks.size());
        if (!node.ranks.empty())
            n.firstForm = node.ranks.front()->GetFormID();
        if (!node.requirements.empty())
            n.firstRequirement = node.requirements.front();
        for (RE::BGSPerk *rank : node.ranks)
        {
            if (actor->HasPerk(rank))
            {
                ++n.held;
                n.form = rank->GetFormID(); // the top rank held, as the perk rows name it
            }
            n.theirs = n.theirs || record.contains(rank);
        }
        const std::size_t shown = static_cast<std::size_t>((std::min)(n.held, n.ranks - 1));
        n.requirement = node.requirements[shown];
        n.description = node.descriptions[shown];
        n.children = node.children;
        nodes.push_back(std::move(n));
    }
    return nodes;
}

} // namespace

std::vector<ft::PerkTreeView> BuildPerkTrees(RE::Actor *actor)
{
    std::vector<ft::PerkTreeView> out;
    auto *list = RE::ActorValueList::GetSingleton();
    auto *owner = actor ? actor->AsActorValueOwner() : nullptr;
    if (!list || !owner)
        return out;
    for (int i = 0; i < static_cast<int>(RE::ActorValue::kTotal); ++i)
    {
        const auto value = static_cast<RE::ActorValue>(i);
        auto *info = list->GetActorValueInfo(value);
        if (!info || !info->skill)
            continue;
        const TreeShape &shape = ShapeOf(value);
        if (shape.nodes.empty())
            continue;
        ft::PerkTreeView tree;
        tree.key = static_cast<std::uint32_t>(i) + 1;
        tree.skill = i;
        const char *name = info->GetFullName();
        tree.name = name && *name ? name : (info->enumName ? info->enumName : "?");
        tree.level = actor->IsPlayerRef() ? owner->GetBaseActorValue(value) : owner->GetPermanentActorValue(value);
        tree.current = owner->GetActorValue(value);
        tree.value = Fmt("%.0f", tree.level);
        tree.nodes = NodesFor(actor, shape);
        out.push_back(std::move(tree));
    }
    // Custom Skills Framework's trees, after the game's own. The level where
    // a tree keeps one, for the player alone, whose level the framework's
    // globals are.
    const std::vector<CustomSkillTree> &custom = CustomSkillTrees();
    for (std::size_t i = 0; i < custom.size(); ++i)
    {
        const TreeShape &shape = ShapeOf(custom[i]);
        if (shape.nodes.empty())
            continue;
        ft::PerkTreeView tree;
        tree.key = CustomTreeKey(i);
        tree.custom = custom[i].id;
        tree.name = DisplayName(custom[i]);
        if (custom[i].level && actor->IsPlayerRef())
        {
            tree.level = tree.current = custom[i].level->value;
            tree.value = Fmt("%.0f", tree.level);
        }
        tree.nodes = NodesFor(actor, shape);
        out.push_back(std::move(tree));
    }
    return out;
}

std::vector<PerkPage> BuildPerkPages(RE::Actor *actor)
{
    std::vector<PerkPage> out;
    if (!actor)
        return out;
    std::unordered_set<const RE::BGSPerk *> seen;
    // A page for a perk held: its id, its rank in its chain, the skill
    // whose tree it sits in, and what it does, entry by entry.
    const auto page = [&](RE::BGSPerk *perk, int rank, int ranks, const std::string &skill) {
        if (!perk || !seen.insert(perk).second)
            return;
        PerkPage p;
        p.form = perk->GetFormID();
        p.name = PerkName(perk);
        if (p.name.empty())
            p.name = "?";
        RE::BSString text;
        perk->GetDescription(text, perk);
        p.description = text.c_str() ? text.c_str() : "";

        SheetSection info{Tr("Perk Details"), {}, {}};
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", perk->GetFormID());
        info.rows.push_back(Row(Tr("Base ID"), id));
        if (ranks > 1)
            info.rows.push_back(Row(Tr("Rank"), std::to_string(rank) + " / " + std::to_string(ranks)));
        if (!skill.empty())
            info.rows.push_back(Row(Tr("Skill"), skill));
        if (perk->data.hidden)
            info.rows.push_back(Row(Tr("Hidden"), Tr("yes")));
        // A tick while the perk does something for them; no row while not.
        if (PerkActive(actor, perk))
        {
            SheetRow active = Row(Tr("Active"), "");
            active.icon = kGlyphTick;
            info.rows.push_back(std::move(active));
        }
        // Who asks for it, for a marker; and for a perk with no entries
        // that nobody asks for, that it does nothing, and whose record
        // left it so -- the last plugin to touch it.
        if (const auto &readers = PerkReaders(perk); !readers.empty())
        {
            std::string who;
            for (std::size_t i = 0; i < readers.size() && i < 4; ++i)
                who += (who.empty() ? "" : Tr(", ")) + readers[i];
            if (readers.size() > 4)
                who += TrFormat(", +{}", readers.size() - 4);
            info.rows.push_back(Row(Tr("Read by"), who));
        }
        p.sections.push_back(std::move(info));

        // The effects: an entry each, with the conditions that gate it on
        // its owner beneath -- a mod's perk given to everyone is gated
        // there, on the power that turns it on -- greyed while they are
        // not met, as an effect's row is. Not the record's own conditions,
        // which are what the skill tree asks before the player may take
        // it, and nothing to an NPC.
        SheetSection effects{Tr("Effects"), {}, {}};
        for (const auto *entry : perk->perkEntries)
        {
            if (!entry)
                continue;
            SheetRow row = EntryRow(entry);
            bool active = true;
            if (entry->GetType() == RE::PERK_ENTRY_TYPE::kEntryPoint)
            {
                // Tab 0 is on the owner and decides `active`; the other
                // tabs are on the entry point's further arguments (Mod
                // Spell Magnitude's second is the spell), which the library
                // numbers and does not name. They are listed as such,
                // unevaluated.
                const auto *point = static_cast<const RE::BGSEntryPointPerkEntry *>(entry);
                if (point->conditions.size() > 0 && point->conditions[0])
                {
                    row.detail = ConditionRows(point->conditions[0], {actor, actor});
                    active = point->conditions[0].IsTrue(actor, actor);
                }
                for (std::uint32_t tab = 1; tab < point->conditions.size(); ++tab)
                {
                    if (!point->conditions[tab])
                        continue;
                    const std::string on = TrFormat("argument {}", tab + 1);
                    for (SheetRow &r : ConditionRows(point->conditions[tab], {actor, actor}, on.c_str()))
                        row.detail.push_back(std::move(r));
                }
            }
            if (!active)
                row.aside = Tr("Conditions not met");
            effects.rows.push_back(std::move(row));
        }
        // By name, the record's order being the author's; two of one name
        // keep their order, the weaker first as a rule.
        std::stable_sort(effects.rows.begin(), effects.rows.end(),
                         [](const SheetRow &a, const SheetRow &b) { return a.label < b.label; });
        // A perk with no entries shows the table all the same, one row of
        // N/A beside the description below it, so a description promising
        // 60% over an effect of nothing is seen as the mismatch it is
        // (Adamant's stub of Armsman's third rank, 2026-09-13); the hover
        // names the plugin that last changed the record.
        if (effects.rows.empty())
        {
            SheetRow row = Row(Tr("Effect"), Tr("N/A"));
            if (const auto *file = perk->GetFile(); file && !file->GetFilename().empty())
                row.note = TrFormat("Record last changed by {}", file->GetFilename());
            effects.rows.push_back(std::move(row));
        }
        p.sections.push_back(std::move(effects));
        out.push_back(std::move(p));
    };

    if (auto *list = RE::ActorValueList::GetSingleton())
    {
        for (int i = 0; i < static_cast<int>(RE::ActorValue::kTotal); ++i)
        {
            const auto value = static_cast<RE::ActorValue>(i);
            auto *info = list->GetActorValueInfo(value);
            if (!info || !info->skill)
                continue;
            const char *skillName = info->GetFullName();
            // The top rank held; and a perk not held at all, by its first
            // rank, since the skill page's names open any perk's page.
            for (const TreePerk &entry : TreePerks(value))
            {
                const bool held = actor->HasPerk(entry.perk);
                if (held ? entry.perk->nextPerk && actor->HasPerk(entry.perk->nextPerk) : entry.rank != 1)
                    continue;
                page(entry.perk, entry.rank, entry.ranks, skillName ? skillName : "");
            }
        }
    }
    for (const CustomSkillTree &tree : CustomSkillTrees())
        for (const CustomTreeNode &node : tree.nodes)
            for (std::size_t r = 0; r < node.ranks.size(); ++r)
            {
                RE::BGSPerk *perk = node.ranks[r];
                if (actor->HasPerk(perk) ? TopRankHeld(actor, perk) : r == 0)
                    page(perk, static_cast<int>(r) + 1, static_cast<int>(node.ranks.size()), DisplayName(tree));
            }
    for (const HeldPerk &held : PerksOutsideTrees(actor))
        page(held.perk, held.rank, 1, "");
    return out;
}

std::vector<SheetSection> BuildSkillSheet(RE::Actor *actor)
{
    std::vector<SheetSection> out;
    if (!actor)
        return out;
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return out;

    const auto av = [owner](RE::ActorValue value) { return owner->GetActorValue(value); };

    // What a skill's two modifier values do, read from the game's own records
    // rather than a wiki (dev/RESEARCH.md, "Skill modifiers").
    //
    // Every actor carries two hidden perks, PerkSkillBoosts and
    // AlchemySkillBoosts in Skyrim.esm. Each multiplies ONE game quantity by
    // (1 + 0.01 * value): the first reads <skill>Modifier, which Fortify
    // enchantments and perks write; the second reads <skill>PowerModifier,
    // which Fortify potions write. For every skill but the magic schools both
    // perks hit the SAME quantity -- one-handed damage, percent blocked,
    // pickpocket chance -- so the two values are one bonus and are shown as
    // one: "+35% damage". For a school they differ: the modifier cuts spell
    // cost, the power modifier raises magnitude (Destruction, Illusion,
    // Restoration) or duration (Alteration, Conjuration), so both are shown,
    // as what spells now are: "83% cost, 200% magnitude".
    //
    // Enchanting appears in neither perk: Fortify Enchanting writes the skill
    // itself. So do Fortify Heavy Armor and Fortify Light Armor -- the perks
    // would honour those two modifiers as a cut to damage taken, but nothing
    // in the base game sets them, so on a vanilla install they stay at zero
    // and the row stays plain.
    //
    // Brackets appear only when a value is off zero, which for a follower is
    // rare. The tooltip says where the number came from.
    struct Modifier
    {
        RE::ActorValue value;
        const char *effect; // nullptr: nothing reads this value
        int sign;           // +1: each point raises the quantity; -1: lowers it
    };
    struct Skill
    {
        const char *label;
        RE::ActorValue value;
        Modifier mod;   // Fortify enchantments and perks
        Modifier power; // Fortify potions
    };
    using AV = RE::ActorValue;
    constexpr Modifier none{AV::kNone, nullptr, 0};

    const auto skill = [&](SheetSection &s, const Skill &k) {
        SheetRow row = Row(k.label, Fmt("%.0f", av(k.value)));
        row.breakdown = SkillBreakdown(actor, k.value);
        // Only what applies. A follower has Fortify One-handed +35 on the
        // value and no perk to turn it into damage; a bonus that changes
        // nothing is not shown.
        const float m = k.mod.effect && ReadsSkillMods(actor) ? av(k.mod.value) : 0.0f;
        const float p = k.power.effect && ReadsSkillPowerMods(actor) ? av(k.power.value) : 0.0f;

        // Every modifier is a signed change from normal: "+90% damage",
        // "-17% cost", each its own figure with its own breakdown: the
        // sources by name -- the gauntlets, the potion -- and what no
        // effect explains as Other. Power first, then the other, as the
        // two read best.
        const auto part = [&](const std::string &text, std::initializer_list<std::pair<const Modifier *, float>> from,
                              double total) {
            SheetRow::ModifierPart piece;
            piece.text = text;
            piece.breakdown.unit = "%";
            for (const auto &[mod, amount] : from)
            {
                if (amount == 0.0f)
                    continue;
                AddValueLines(piece.breakdown, PartsOf(actor, mod->value), static_cast<float>(mod->sign));
            }
            piece.breakdown.total = total;
            ft::Close(piece.breakdown);
            row.modifiers += (row.modifiers.empty() ? "" : Tr(", ")) + text;
            row.modifierParts.push_back(std::move(piece));
        };
        if (k.mod.effect && k.power.effect && std::string_view(k.mod.effect) == k.power.effect)
        {
            // One quantity, two factors: multiply them and show the change.
            // The sources' sum misses the product by their cross term, an
            // Other line.
            if (m != 0.0f || p != 0.0f)
            {
                const double change = ((1.0 + k.mod.sign * m / 100.0) * (1.0 + k.power.sign * p / 100.0) - 1.0) * 100.0;
                part(TrFormat("{}% {}", Fmt("%+.0f", change), Tr(k.mod.effect)), {{&k.power, p}, {&k.mod, m}}, change);
            }
        }
        else
        {
            if (p != 0.0f)
                part(TrFormat("{}% {}", Fmt("%+.0f", k.power.sign * p), Tr(k.power.effect)), {{&k.power, p}},
                     k.power.sign * p);
            if (m != 0.0f)
                part(TrFormat("{}% {}", Fmt("%+.0f", k.mod.sign * m), Tr(k.mod.effect)), {{&k.mod, m}}, k.mod.sign * m);
        }

        // The Armor Perks value, which the engine adds to either armour
        // skill's multiplier for every piece worn (dev/MODIFIERS.md): on
        // both rows, with what set it on hover.
        if (k.value == AV::kHeavyArmor || k.value == AV::kLightArmor)
        {
            if (const float perks = av(AV::kArmorPerks); perks != 0.0f)
            {
                SheetRow::ModifierPart piece;
                piece.text = TrFormat("{} skill multiplier", Fmt("%+.2f", perks));
                piece.breakdown = ValueBreakdown(actor, AV::kArmorPerks, "");
                piece.breakdown.decimals = 2;
                row.modifiers += (row.modifiers.empty() ? "" : Tr(", ")) + piece.text;
                row.modifierParts.push_back(std::move(piece));
            }
        }

        row.detail = OwnedPerks(actor, k.value);
        if (!ShapeOf(k.value).nodes.empty())
            row.tree = static_cast<std::uint32_t>(k.value) + 1; // BuildPerkTrees' key
        // A skill at zero with no perk in it -- Vampire Lord on a mortal --
        // says nothing; a section of those says nothing either.
        if (av(k.value) == 0.0f && row.detail.empty() && row.modifiers.empty())
            return;
        s.rows.push_back(std::move(row));
    };

    // What the two modifier values of each vanilla skill do. No record
    // links a skill to them, so this is a table; a skill a mod adds gets
    // none, and its row shows the level alone.
    struct Pair
    {
        Modifier mod;
        Modifier power;
    };
    static const std::unordered_map<AV, Pair> kPairs{
        {AV::kOneHanded, {{AV::kOneHandedModifier, N_("damage"), +1}, {AV::kOneHandedPowerModifier, N_("damage"), +1}}},
        {AV::kTwoHanded, {{AV::kTwoHandedModifier, N_("damage"), +1}, {AV::kTwoHandedPowerModifier, N_("damage"), +1}}},
        {AV::kBlock, {{AV::kBlockModifier, N_("blocked"), +1}, {AV::kBlockPowerModifier, N_("blocked"), +1}}},
        {AV::kSmithing,
         {{AV::kSmithingModifier, N_("tempering"), +1}, {AV::kSmithingPowerModifier, N_("tempering"), +1}}},
        {AV::kHeavyArmor,
         {{AV::kHeavyArmorModifier, N_("damage"), -1}, {AV::kHeavyArmorPowerModifier, N_("damage"), -1}}},
        {AV::kLightArmor,
         {{AV::kLightArmorModifier, N_("damage"), -1}, {AV::kLightArmorPowerModifier, N_("damage"), -1}}},
        {AV::kArchery, {{AV::kMarksmanModifier, N_("damage"), +1}, {AV::kMarksmanPowerModifier, N_("damage"), +1}}},
        {AV::kPickpocket,
         {{AV::kPickpocketModifier, N_("chance"), +1}, {AV::kPickpocketPowerModifier, N_("chance"), +1}}},
        {AV::kLockpicking,
         {{AV::kLockpickingModifier, N_("sweet spot"), +1}, {AV::kLockpickingPowerModifier, N_("sweet spot"), +1}}},
        {AV::kSneak, {{AV::kSneakingModifier, N_("stealth"), +1}, {AV::kSneakingPowerModifier, N_("stealth"), +1}}},
        {AV::kAlchemy,
         {{AV::kAlchemyModifier, N_("potion strength"), +1}, {AV::kAlchemyPowerModifier, N_("potion strength"), +1}}},
        // Sell prices up and buy prices down by the same factor: "better prices".
        {AV::kSpeech,
         {{AV::kSpeechcraftModifier, N_("better prices"), +1},
          {AV::kSpeechcraftPowerModifier, N_("better prices"), +1}}},
        {AV::kAlteration,
         {{AV::kAlterationModifier, N_("cost"), -1}, {AV::kAlterationPowerModifier, N_("duration"), +1}}},
        {AV::kConjuration,
         {{AV::kConjurationModifier, N_("cost"), -1}, {AV::kConjurationPowerModifier, N_("duration"), +1}}},
        {AV::kDestruction,
         {{AV::kDestructionModifier, N_("cost"), -1}, {AV::kDestructionPowerModifier, N_("damage"), +1}}},
        {AV::kIllusion, {{AV::kIllusionModifier, N_("cost"), -1}, {AV::kIllusionPowerModifier, N_("magnitude"), +1}}},
        {AV::kRestoration,
         {{AV::kRestorationModifier, N_("cost"), -1}, {AV::kRestorationPowerModifier, N_("healing"), +1}}},
        {AV::kEnchanting, {none, none}},
    };

    // The skills themselves are read off the records, not a list here:
    // every actor value with a skill block, under the category its record
    // carries (CNAM: 1 combat, 2 magic, 3 stealth -- which puts Archery
    // with the warriors and Alchemy with the mages, as the game's own
    // constellations do), by name within it. A mod that retunes, renames
    // or re-trees a skill is read as it stands; one that adds a skill
    // block to another value gets a row, without modifiers.
    struct Found
    {
        std::string name;
        AV value;
        std::uint32_t category;
    };
    std::vector<Found> found;
    if (auto *list = RE::ActorValueList::GetSingleton())
    {
        for (int i = 0; i < static_cast<int>(AV::kTotal); ++i)
        {
            const auto value = static_cast<AV>(i);
            auto *info = list->GetActorValueInfo(value);
            if (!info || !info->skill)
                continue;
            const char *name = info->GetFullName();
            // The record's category (1 combat, 2 magic, 3 stealth) is the
            // constellation, with one exception: Alchemy's record says
            // Magic, and the skill menu, the Thief Stone and every player
            // put it under the Thief. The constellation wins.
            const std::uint32_t category = value == AV::kAlchemy ? 3u : info->unk124;
            found.push_back({name && *name ? name : (info->enumName ? info->enumName : "?"), value, category});
        }
    }
    std::sort(found.begin(), found.end(), [](const Found &a, const Found &b) { return a.name < b.name; });

    struct Category
    {
        std::uint32_t code;
        const char *title;
    };
    constexpr Category kCategories[] = {
        {1, N_("Warrior")}, {3, N_("Thief")}, {2, N_("Magic")}, {0, N_("Other Skills")}};
    for (const Category &category : kCategories)
    {
        SheetSection s{Tr(category.title), {}, {}};
        for (const Found &f : found)
        {
            const bool here = category.code == 0 ? (f.category != 1 && f.category != 2 && f.category != 3)
                                                 : f.category == category.code;
            if (!here)
                continue;
            const auto pair = kPairs.find(f.value);
            skill(s, {f.name.c_str(), f.value, pair != kPairs.end() ? pair->second.mod : none,
                      pair != kPairs.end() ? pair->second.power : none});
        }
        // Custom Skills Framework's trees sit with the other skills: a row
        // each with its perks held, and its level where the tree keeps one,
        // for the player alone, whose level the framework's globals are.
        // Every tree for the player, as the framework's own menu lists
        // them, and for a follower Progression levels, for whom the row is
        // the way to the tree they buy their first perk of it in; for
        // another follower, a tree with nothing to show says nothing, as a
        // vanilla skill at zero.
        if (category.code == 0)
        {
            const bool every = actor->IsPlayerRef() || fp::game::LevelFor(actor->GetFormID()).has_value();
            const std::vector<CustomSkillTree> &custom = CustomSkillTrees();
            for (std::size_t i = 0; i < custom.size(); ++i)
            {
                const CustomSkillTree &tree = custom[i];
                const bool level = tree.level && actor->IsPlayerRef();
                SheetRow row = Row(DisplayName(tree), level ? Fmt("%.0f", tree.level->value) : std::string{});
                row.detail = OwnedPerks(actor, tree);
                if (!tree.nodes.empty())
                    row.tree = CustomTreeKey(i); // BuildPerkTrees' key
                if (!row.detail.empty() || (level && tree.level->value > 0.0f) || (every && !tree.nodes.empty()))
                    s.rows.push_back(std::move(row));
            }
        }
        // By name within a section, the custom trees among the game's own.
        std::stable_sort(s.rows.begin(), s.rows.end(),
                         [](const SheetRow &a, const SheetRow &b) { return a.label < b.label; });
        if (!s.rows.empty())
            out.push_back(std::move(s));
    }

    // The perks held that sit in no skill's tree: a mod's loose perk, a
    // race's, a quest's. The unnamed stay out.
    {
        std::unordered_set<const RE::BGSPerk *> inTrees;
        for (const Found &f : found)
            for (const TreePerk &entry : TreePerks(f.value))
                inTrees.insert(entry.perk);
        for (const CustomSkillTree &tree : CustomSkillTrees())
            for (const CustomTreeNode &node : tree.nodes)
                inTrees.insert(node.ranks.begin(), node.ranks.end());
        SheetSection s{Tr("Other Perks"), {}, {}};
        for (const HeldPerk &held : PerksOutsideTrees(actor))
        {
            if (inTrees.contains(held.perk))
                continue;
            const std::string name = PerkName(held.perk);
            if (name.empty())
                continue;
            RE::BSString text;
            held.perk->GetDescription(text, held.perk);
            SheetRow row = Row(name, held.rank > 1 ? std::to_string(held.rank) : std::string());
            row.modifiers = text.c_str() ? text.c_str() : "";
            row.form = held.perk->GetFormID();
            if (const char *aside = PerkAside(actor, held.perk))
                row.aside = aside;
            s.rows.push_back(std::move(row));
        }
        std::sort(s.rows.begin(), s.rows.end(), [](const SheetRow &a, const SheetRow &b) { return a.label < b.label; });
        if (!s.rows.empty())
            out.push_back(std::move(s));
    }

    return out;
}

} // namespace ft::game
