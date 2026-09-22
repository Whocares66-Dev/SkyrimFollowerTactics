#pragma once
// The perk trees as a companion sees them (dev/PROGRESSION.md, "Perks").
//
// A tree is read from the game (progression/game/PerkTrees.cpp) into nodes, each a chain
// of ranks, each rank a perk form with its own conditions. The conditions
// are the truth about what a rank needs -- the lines the Skills menu draws
// are not: vanilla's Magic Resistance is drawn from the root and actually
// requires Apprentice Alteration (tests/progression/data/vanilla-perks.json). Vanilla uses
// two condition functions, GetBaseActorValue and HasPerk, with OR flags on
// the nodes that have two parents; both are evaluated here. A function this
// file does not know is reported, not guessed.
//
// Each node also carries a verdict on whether it does anything for a
// companion (Classify, below), read from what its effects hook into.
//
// What a companion holds is handed in: the perks on their own record
// (innate, not ours) and the ones bought here (the record's). No Skyrim.

#include "progression/core/Ids.h"
#include "progression/core/Settings.h"
#include "progression/core/Skills.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fp
{

// --- conditions -------------------------------------------------------------

enum class ConditionFunction : std::uint8_t
{
    GetBaseActorValue,
    GetActorValue,
    HasPerk,
    Other, // anything else: shown, and passed, with a note
};

enum class Comparison : std::uint8_t
{
    Equal,
    NotEqual,
    Greater,
    GreaterOrEqual,
    Less,
    LessOrEqual,
};

struct Condition
{
    ConditionFunction function{ConditionFunction::Other};
    int actorValue{-1}; // for the actor value functions: the engine's number
    FormKey perk;       // for HasPerk
    Comparison comparison{Comparison::Equal};
    float value{0.0f};
    bool orNext{false};    // the OR flag: this one and the next form a group
    std::string otherName; // for Other: the function's name, for the note
};

// --- the graph ---------------------------------------------------------------

enum class PerkEffect : std::uint8_t
{
    Works,       // hooks into something the engine evaluates for any actor
    Situational, // works, when the companion does something their AI rarely does
    Unverified,  // might; nothing read says either way
    NoEffect,    // only the player's systems read it
};

[[nodiscard]] std::string_view Name(PerkEffect e) noexcept;

struct PerkRank
{
    FormKey form;
    std::string description;
    std::vector<Condition> conditions;
};

struct PerkNode
{
    int id{-1};
    Skill skill{Skill::OneHanded};
    std::string name;
    std::vector<PerkRank> ranks;
    PerkEffect effect{PerkEffect::Unverified};
    std::string note; // why the verdict, for the hover
    float x{0.0f};    // the Skills menu's position, for ordering
    float y{0.0f};
};

class PerkGraph
{
  public:
    // Adds a node and returns its id. Its ranks' forms are indexed.
    int Add(PerkNode node);

    [[nodiscard]] const PerkNode &Node(int id) const;
    [[nodiscard]] std::span<const PerkNode> Nodes() const noexcept
    {
        return nodes_;
    }
    [[nodiscard]] std::size_t Size() const noexcept
    {
        return nodes_.size();
    }
    // The node and the rank index a perk form belongs to.
    [[nodiscard]] std::optional<std::pair<int, int>> Find(const FormKey &form) const;
    // A tree's nodes, in the order the Perks tab lists them: by the lowest
    // skill requirement, then top to bottom as the Skills menu places them.
    [[nodiscard]] std::vector<int> Tree(Skill skill) const;
    // The nodes a node's first rank names in HasPerk conditions: what the
    // player would call its parents.
    [[nodiscard]] std::vector<int> Parents(int id) const;

    void Clear() noexcept;

  private:
    std::vector<PerkNode> nodes_;
    std::unordered_map<FormKey, std::pair<int, int>, FormKeyHash> index_;
};

// The skill requirement a rank's conditions carry for its own tree's skill:
// the largest GetBaseActorValue/GetActorValue >= threshold on that skill.
// 0 when there is none.
[[nodiscard]] int SkillRequirement(const PerkRank &rank, Skill skill) noexcept;

// --- the catalog ---------------------------------------------------------------

// What a perk's effects hook into, as the game side reads them.
struct EffectSummary
{
    std::vector<int> entryPoints; // BGSEntryPoint numbers
    bool ability{false};          // adds an ability spell
    bool quest{false};            // runs a quest stage
};

struct Verdict
{
    PerkEffect effect{PerkEffect::Unverified};
    std::string note;
};

// A node's verdict: the catalog's own word for a vanilla perk it knows,
// else a reading of its effects. `firstRank` identifies the node.
[[nodiscard]] Verdict Classify(const FormKey &firstRank, const EffectSummary &effects);

// --- a companion's holdings, and the rules -------------------------------------

struct Holdings
{
    std::unordered_set<FormKey, FormKeyHash> innate;  // on their own record
    std::unordered_set<FormKey, FormKeyHash> learned; // bought here
};

enum class PerkBlock : std::uint8_t
{
    None,     // the next rank can be learned
    Maxed,    // every rank is held
    Skill,    // a skill requirement is not met
    Requires, // a perk it needs is not held
    NoPoints, // everything is met but there is nothing to spend
};

struct Requirement
{
    enum class Kind : std::uint8_t
    {
        Skill,
        Perk,
        Other
    } kind{Kind::Skill};
    Skill skill{Skill::OneHanded};
    int need{0};
    int have{0};
    int node{-1};     // for a perk: the node, when the graph has it
    std::string name; // for a perk or an Other: what to call it
    bool met{false};
    bool alternative{false}; // one of an OR group: any one of the group will do
};

struct PerkStatus
{
    int held{0};    // ranks held, counting up from the first
    int innate{0};  // of them, on their own record
    int learned{0}; // of them, bought here
    PerkBlock block{PerkBlock::None};
    // The next rank's conditions, each with whether it is met. Empty when
    // every rank is held.
    std::vector<Requirement> requirements;
    // Unlearning: the top rank must be one bought here, and nothing bought
    // here may depend on it.
    bool canUnlearn{false};
    std::vector<int> dependants; // nodes whose learned ranks would fail without it
};

struct PerkRules
{
    const PerkGraph &graph;
    const Holdings &holdings;
    const PerSkill<int> &skills; // base plus training: what requirements read
    int points{0};
};

// Whether a perk form counts as held: innate or learned, or implied by a
// higher rank of its node held.
[[nodiscard]] bool Held(const PerkRules &rules, const FormKey &form);

[[nodiscard]] PerkStatus Status(const PerkRules &rules, int node);

// Whether a rank's conditions all pass.
[[nodiscard]] bool ConditionsMet(const PerkRules &rules, const PerkRank &rank);

// The nodes of perks bought here whose conditions would fail if `removing`
// were no longer held -- the direct dependants, which is enough to refuse:
// what blocks setting a perk aside, or unlearning it.
[[nodiscard]] std::vector<int> WouldBreak(const PerkRules &rules, std::span<const FormKey> removing);

// The perks bought here that no longer meet their conditions once `skills`
// are what they will be: what a retrain refunds. Found to a fixed point, so a
// perk that fails only because another one went is included, after it.
// Higher ranks come before lower ones of the same node.
[[nodiscard]] std::vector<FormKey> Invalidated(const PerkGraph &graph, const Holdings &holdings,
                                               const PerSkill<int> &skills);

} // namespace fp
