#include "progression/core/Perks.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace fp
{
namespace
{

// --- the catalog ---------------------------------------------------------------

// The engine's entry point numbers (BGSEntryPoint), grouped by whether an
// NPC's use of them reaches anything. Read against the 18 vanilla trees
// (tests/progression/data/vanilla-perks.json); unverified in play.
enum EntryPoint : int
{
    kCalculateWeaponDamage = 0,
    kCalculateMyCriticalHitChance = 1,
    kCalculateMyCriticalHitDamage = 2,
    kGetMaxCarryWeight = 10,
    kModEnemyCriticalHitChance = 17,
    kModSneakAttackMult = 18,
    kModBowZoom = 20,
    kModBashingDamage = 26,
    kModPowerAttackStamina = 27,
    kModPowerAttackDamage = 28,
    kModSpellMagnitude = 29,
    kModSpellDuration = 30,
    kModArmorWeight = 32,
    kModIncomingStagger = 33,
    kModTargetStagger = 34,
    kModAttackDamage = 35,
    kModIncomingDamage = 36,
    kModTargetDamageResistance = 37,
    kModSpellCost = 38,
    kModPercentBlocked = 39,
    kModShieldDeflectArrowChance = 40,
    kModIncomingSpellMagnitude = 41,
    kModIncomingSpellDuration = 42,
    kModDetectionLight = 47,
    kModDetectionMovement = 48,
    kSetSweepAttack = 50,
    kApplyCombatHitSpell = 51,
    kApplyBashingSpell = 52,
    kApplyReanimateSpell = 53,
    kSetBooleanGraphVariable = 54,
    kModSpellCastingSoundEvent = 55,
    kModDetectionSneakSkill = 57,
    kModFallingDamage = 58,
    kApplyWeaponSwingSpell = 67,
    kModCommandedActorLimit = 68,
    kApplySneakingSpell = 69,
    kModPlayerMagicSlowdown = 70,
    kModWardMagickaAbsorptionPct = 71,
    kCanDualCastSpell = 75,
    kModArmorRating = 85,
    kModSpellRangeTargetLoc = 88,
};

PerkEffect ByEntryPoint(int ep)
{
    switch (ep)
    {
    case kCalculateWeaponDamage:
    case kCalculateMyCriticalHitChance:
    case kCalculateMyCriticalHitDamage:
    case kGetMaxCarryWeight:
    case kModEnemyCriticalHitChance:
    case kModBashingDamage:
    case kModPowerAttackStamina:
    case kModPowerAttackDamage:
    case kModSpellMagnitude:
    case kModSpellDuration:
    case kModIncomingStagger:
    case kModTargetStagger:
    case kModAttackDamage:
    case kModIncomingDamage:
    case kModTargetDamageResistance:
    case kModSpellCost:
    case kModPercentBlocked:
    case kModShieldDeflectArrowChance:
    case kModIncomingSpellMagnitude:
    case kModIncomingSpellDuration:
    case kModDetectionLight:
    case kModDetectionMovement:
    case kSetSweepAttack:
    case kApplyCombatHitSpell:
    case kApplyBashingSpell:
    case kApplyReanimateSpell:
    case kModDetectionSneakSkill:
    case kModCommandedActorLimit:
    case kModWardMagickaAbsorptionPct:
    case kModArmorRating:
    case kModSpellRangeTargetLoc:
        return PerkEffect::Works;
    case kModSneakAttackMult:
    case kModSpellCastingSoundEvent:
    case kModFallingDamage:
    case kCanDualCastSpell:
        return PerkEffect::Situational;
    case kSetBooleanGraphVariable:
    case kApplyWeaponSwingSpell:
    case kApplySneakingSpell:
    case kModArmorWeight:
        return PerkEffect::Unverified;
    case kModBowZoom:
    case kModPlayerMagicSlowdown:
        return PerkEffect::NoEffect;
    default:
        // Everything else in the list is crafting, trade, locks, pockets,
        // persuasion, activation or a player-only counter; and a number past
        // the list is a mod's own, of which nothing is known.
        return ep >= 0 && ep <= 91 ? PerkEffect::NoEffect : PerkEffect::Unverified;
    }
}

struct Known
{
    std::uint32_t local;
    PerkEffect effect;
    std::string_view note;
};

constexpr std::string_view kDualCast = "Only when they dual-cast. With Follower Tactics, a setting can require this "
                                       "perk before a follower dual-casts.";

// Skyrim.esm's perks the reading above would misjudge, or leave without a
// word worth saying. Keyed by the node's first rank.
constexpr std::array<Known, 40> kVanilla{{
    // Archery
    {0x058F61, PerkEffect::NoEffect, "Zooming is the player's alone."},               // Eagle Eye
    {0x103ADA, PerkEffect::NoEffect, "Slows time while the player zooms."},           // Steady Hand
    {0x051B12, PerkEffect::NoEffect, "Only the player recovers arrows from bodies."}, // Hunter's Discipline
    {0x058F63, PerkEffect::Unverified, "Sets an animation switch a companion's graph may not read."}, // Ranger
    {0x105F19, PerkEffect::Unverified, "Sets an animation switch a companion's graph may not read."}, // Quick Shot
    {0x058F62, PerkEffect::Works, "Adds an ability: arrows stagger."},                                // Power Shot
    // Block
    {0x0D8C33, PerkEffect::NoEffect, "Slows time for the player."},                                   // Quick Reflexes
    {0x106253, PerkEffect::Unverified, "Sets an animation switch a companion's graph may not read."}, // Block Runner
    {0x058F67, PerkEffect::Situational,
     "Lets them power-bash. With Follower Tactics, a setting can require this perk for it."}, // Power Bash
    {0x058F6A, PerkEffect::Situational, "Only while sprinting with a shield raised."},        // Shield Charge
    // One-Handed, Two-Handed
    {0x0CB406, PerkEffect::Situational,
     "Only on a sprinting power attack, which the AI rarely makes."}, // Critical Charge
    {0x0CB407, PerkEffect::Situational,
     "Only on a sprinting power attack, which the AI rarely makes."}, // Great Critical Charge
    {0x106256, PerkEffect::Situational, "Only while dual wielding."}, // Dual Flurry
    {0x106258, PerkEffect::Situational, "Only while dual wielding."}, // Dual Savagery
    // Heavy and Light Armor
    {0x0BCD2B, PerkEffect::Situational, "Only when they fall."},                                     // Cushioned
    {0x058F6D, PerkEffect::Unverified, "Armour weight; whether it slows a companion is not known."}, // Conditioning
    {0x051B1C, PerkEffect::Unverified, "Armour weight; whether it slows a companion is not known."}, // Unhindered
    // Sneak
    {0x058214, PerkEffect::NoEffect, "Crouching to break combat is the player's move."}, // Shadow Warrior
    {0x105F23, PerkEffect::NoEffect, "A move only the player makes."},                   // Silent Roll
    {0x05820C, PerkEffect::Unverified, "Pressure plates may or may not ignore a companion who has it."}, // Light Foot
    {0x058210, PerkEffect::Situational, "Only on sneak attacks."},                                       // Backstab
    {0x058211, PerkEffect::Situational, "Only on sneak attacks."},              // Assassin's Blade
    {0x1036F0, PerkEffect::Situational, "Only on sneak attacks."},              // Deadly Aim
    {0x0BE126, PerkEffect::Works, "Harder to detect while sneaking with you."}, // Stealth
    // Magic: read by the spells' own conditions, not an entry point
    {0x0D7999, PerkEffect::Works, "Read by the protection spells themselves, when no armour is worn."}, // Mage Armor
    {0x0F392E, PerkEffect::Works, "Read by the fire spells' own effects."},    // Intense Flames
    {0x0F3933, PerkEffect::Works, "Read by the frost spells' own effects."},   // Deep Freeze
    {0x0F3F0E, PerkEffect::Works, "Read by the shock spells' own effects."},   // Disintegrate
    {0x0CB41A, PerkEffect::Works, "Read by the atronach spells."},             // Elemental Potency
    {0x0581F9, PerkEffect::Works, "Read by the healing spells."},              // Respite
    {0x059B76, PerkEffect::Works, "Read by the Illusion spells' conditions."}, // Master of the Mind
    {0x0640B3, PerkEffect::Situational, "Only with bound weapons."},           // Mystic Binding
    {0x0D799E, PerkEffect::Situational, "Only with bound weapons."},           // Soul Stealer
    {0x0D799C, PerkEffect::Situational, "Only with bound weapons."},           // Oblivion Binding
    {0x0153D2, PerkEffect::Situational, kDualCast},                            // Impact
    {0x0153CD, PerkEffect::Situational, kDualCast},                            // Alteration Dual Casting
    {0x0153CE, PerkEffect::Situational, kDualCast},                            // Conjuration Dual Casting
    {0x0153CF, PerkEffect::Situational, kDualCast},                            // Destruction Dual Casting
    {0x0153D0, PerkEffect::Situational, kDualCast},                            // Illusion Dual Casting
    {0x0153D1, PerkEffect::Situational, kDualCast},                            // Restoration Dual Casting
}};

std::string_view UntrainedNote(Skill tree)
{
    switch (tree)
    {
    case Skill::Smithing:
        return "Companions don't smith.";
    case Skill::Alchemy:
        return "Companions don't brew.";
    case Skill::Enchanting:
        return "Companions don't enchant.";
    case Skill::Speech:
        return "Companions don't barter or persuade.";
    case Skill::Lockpicking:
        return "Companions don't pick locks.";
    case Skill::Pickpocket:
        return "Companions don't pick pockets.";
    default:
        return "Companions are not trained in this skill.";
    }
}

bool Compare(float lhs, Comparison op, float rhs) noexcept
{
    switch (op)
    {
    case Comparison::Equal:
        return lhs == rhs;
    case Comparison::NotEqual:
        return lhs != rhs;
    case Comparison::Greater:
        return lhs > rhs;
    case Comparison::GreaterOrEqual:
        return lhs >= rhs;
    case Comparison::Less:
        return lhs < rhs;
    case Comparison::LessOrEqual:
        return lhs <= rhs;
    default:
        return false;
    }
}

// Conditions evaluated against a companion, with the bridges resolved on
// the way: a bridge is held when its own first rank's conditions pass, which
// may mean another bridge's. A cycle -- no vanilla tree has one -- reads as
// not held rather than recursing forever.
class Evaluator
{
  public:
    explicit Evaluator(const PerkRules &rules) : rules_(rules)
    {
    }

    bool Direct(const FormKey &form) const
    {
        return rules_.holdings.innate.contains(form) || rules_.holdings.learned.contains(form);
    }

    // Held directly, or implied by a higher rank of the same node held
    // directly -- Marcurio's record carries Recovery's second rank without
    // its first -- or a bridge whose conditions are met.
    bool Held(const FormKey &form)
    {
        if (Direct(form))
            return true;
        const auto found = rules_.graph.Find(form);
        if (!found)
            return false;
        const PerkNode &node = rules_.graph.Node(found->first);
        for (std::size_t r = static_cast<std::size_t>(found->second) + 1; r < node.ranks.size(); ++r)
            if (Direct(node.ranks[r].form))
                return true;
        return IsBridge(found->first) && BridgeSatisfied(found->first);
    }

    bool IsBridge(int node) const
    {
        return rules_.graph.Node(node).effect == PerkEffect::NoEffect && !rules_.offerNoEffect;
    }

    bool BridgeSatisfied(int node)
    {
        auto &state = bridges_[node];
        if (state == State::Yes)
            return true;
        if (state == State::No || state == State::Visiting)
            return false;
        state = State::Visiting;
        const PerkNode &n = rules_.graph.Node(node);
        const bool ok = !n.ranks.empty() && Met(n.ranks.front());
        bridges_[node] = ok ? State::Yes : State::No;
        return ok;
    }

    bool One(const Condition &c)
    {
        switch (c.function)
        {
        case ConditionFunction::GetBaseActorValue:
        case ConditionFunction::GetActorValue: {
            const auto skill = SkillFromActorValue(c.actorValue);
            if (!skill)
                return true; // not a skill: nothing here to judge it by
            return Compare(static_cast<float>(rules_.skills[Index(*skill)]), c.comparison, c.value);
        }
        case ConditionFunction::HasPerk:
            return Compare(Held(c.perk) ? 1.0f : 0.0f, c.comparison, c.value);
        case ConditionFunction::Other:
        default:
            return true;
        }
    }

    // Skyrim's rule: a run of conditions joined by OR flags is one group,
    // and every group must pass.
    bool Met(const PerkRank &rank)
    {
        bool all = true;
        bool group = false;
        for (const Condition &c : rank.conditions)
        {
            group = group || One(c);
            if (!c.orNext)
            {
                all = all && group;
                group = false;
            }
        }
        // A trailing OR flag closes its group with the list.
        if (!rank.conditions.empty() && rank.conditions.back().orNext)
            all = all && group;
        return all;
    }

  private:
    enum class State : std::uint8_t
    {
        Unknown,
        Visiting,
        Yes,
        No
    };
    const PerkRules &rules_;
    std::unordered_map<int, State> bridges_;
};

} // namespace

std::string_view Name(PerkEffect e) noexcept
{
    switch (e)
    {
    case PerkEffect::Works:
        return "Works";
    case PerkEffect::Situational:
        return "Situational";
    case PerkEffect::NoEffect:
        return "No effect";
    case PerkEffect::Unverified:
    default:
        return "Unverified";
    }
}

// --- the graph -----------------------------------------------------------------

int PerkGraph::Add(PerkNode node)
{
    const int id = static_cast<int>(nodes_.size());
    node.id = id;
    for (std::size_t r = 0; r < node.ranks.size(); ++r)
        index_[node.ranks[r].form] = {id, static_cast<int>(r)};
    nodes_.push_back(std::move(node));
    return id;
}

const PerkNode &PerkGraph::Node(int id) const
{
    if (id < 0 || static_cast<std::size_t>(id) >= nodes_.size())
        throw std::out_of_range("no such perk node");
    return nodes_[static_cast<std::size_t>(id)];
}

std::optional<std::pair<int, int>> PerkGraph::Find(const FormKey &form) const
{
    const auto it = index_.find(form);
    if (it == index_.end())
        return std::nullopt;
    return it->second;
}

std::vector<int> PerkGraph::Tree(Skill skill) const
{
    std::vector<int> ids;
    for (const PerkNode &node : nodes_)
        if (node.skill == skill)
            ids.push_back(node.id);
    std::stable_sort(ids.begin(), ids.end(), [&](int a, int b) {
        const PerkNode &na = nodes_[static_cast<std::size_t>(a)];
        const PerkNode &nb = nodes_[static_cast<std::size_t>(b)];
        const int ra = na.ranks.empty() ? 0 : SkillRequirement(na.ranks.front(), skill);
        const int rb = nb.ranks.empty() ? 0 : SkillRequirement(nb.ranks.front(), skill);
        if (ra != rb)
            return ra < rb;
        return na.y < nb.y;
    });
    return ids;
}

std::vector<int> PerkGraph::Parents(int id) const
{
    std::vector<int> parents;
    const PerkNode &node = Node(id);
    if (node.ranks.empty())
        return parents;
    for (const Condition &c : node.ranks.front().conditions)
    {
        if (c.function != ConditionFunction::HasPerk || c.value < 1.0f)
            continue;
        const auto found = Find(c.perk);
        if (found && found->first != id && std::find(parents.begin(), parents.end(), found->first) == parents.end())
            parents.push_back(found->first);
    }
    return parents;
}

void PerkGraph::Clear() noexcept
{
    nodes_.clear();
    index_.clear();
}

int SkillRequirement(const PerkRank &rank, Skill skill) noexcept
{
    int need = 0;
    for (const Condition &c : rank.conditions)
    {
        if (c.function != ConditionFunction::GetBaseActorValue && c.function != ConditionFunction::GetActorValue)
            continue;
        if (SkillFromActorValue(c.actorValue) != skill)
            continue;
        if (c.comparison == Comparison::GreaterOrEqual)
            need = std::max(need, static_cast<int>(c.value));
        else if (c.comparison == Comparison::Greater)
            need = std::max(need, static_cast<int>(c.value) + 1);
    }
    return need;
}

// --- the catalog ---------------------------------------------------------------

Verdict Classify(Skill tree, const FormKey &firstRank, const EffectSummary &effects)
{
    if (!IsTrainable(tree))
        return {PerkEffect::NoEffect, std::string(UntrainedNote(tree))};

    if (firstRank.plugin == "Skyrim.esm")
        for (const Known &known : kVanilla)
            if (known.local == firstRank.local)
                return {known.effect, std::string(known.note)};

    if (effects.entryPoints.empty() && !effects.ability)
        return {PerkEffect::Unverified, effects.quest ? "Works through a quest, which may only watch the player."
                                                      : "No effect of its own: other records' conditions may read it."};

    // The best any of its effects does, except that a zoom or a slowdown
    // anywhere in it means the perk is the player's.
    PerkEffect best = effects.ability ? PerkEffect::Works : PerkEffect::NoEffect;
    for (const int ep : effects.entryPoints)
    {
        if (ep == kModBowZoom || ep == kModPlayerMagicSlowdown)
            return {PerkEffect::NoEffect, "Changes what only the player sees or feels."};
        const PerkEffect e = ByEntryPoint(ep);
        if (static_cast<int>(e) < static_cast<int>(best))
            best = e;
    }
    switch (best)
    {
    case PerkEffect::Works:
        return {best, effects.ability ? "Adds an ability that works on anyone." : "Works in combat for anyone."};
    case PerkEffect::Situational:
        return {best, "Only in some situations."};
    case PerkEffect::NoEffect:
        return {best, "Only the player's systems read it."};
    case PerkEffect::Unverified:
    default:
        return {best, "Nothing read says whether it works for a companion."};
    }
}

// --- the rules -------------------------------------------------------------------

bool Held(const PerkRules &rules, const FormKey &form)
{
    Evaluator evaluator(rules);
    return evaluator.Held(form);
}

bool ConditionsMet(const PerkRules &rules, const PerkRank &rank)
{
    Evaluator evaluator(rules);
    return evaluator.Met(rank);
}

PerkStatus Status(const PerkRules &rules, int nodeId)
{
    PerkStatus status;
    const PerkNode &node = rules.graph.Node(nodeId);
    Evaluator evaluator(rules);

    const bool bridge = evaluator.IsBridge(nodeId);
    // Held up to the highest rank held directly: the ranks below it are
    // implied, and count as theirs unless bought here.
    int top = -1;
    for (std::size_t r = 0; r < node.ranks.size(); ++r)
        if (evaluator.Direct(node.ranks[r].form))
            top = static_cast<int>(r);
    for (int r = 0; r <= top; ++r)
    {
        if (rules.holdings.learned.contains(node.ranks[static_cast<std::size_t>(r)].form))
            ++status.learned;
        else
            ++status.innate;
    }
    status.held = top + 1;
    if (bridge && status.held == 0 && evaluator.BridgeSatisfied(nodeId))
    {
        status.bridge = true;
        status.held = static_cast<int>(node.ranks.size());
    }

    // Unlearning: the top rank held must be ours, and nothing else of ours
    // may need it.
    if (status.held > 0 && !status.bridge)
    {
        const FormKey &highest = node.ranks[static_cast<std::size_t>(status.held - 1)].form;
        if (rules.holdings.learned.contains(highest))
        {
            Holdings without = rules.holdings;
            without.learned.erase(highest);
            const PerkRules after{rules.graph, without, rules.skills, rules.points, rules.offerNoEffect};
            Evaluator check(after);
            for (const FormKey &other : without.learned)
            {
                const auto found = rules.graph.Find(other);
                if (!found)
                    continue;
                const PerkRank &rank = rules.graph.Node(found->first).ranks[static_cast<std::size_t>(found->second)];
                if (!check.Met(rank) && std::find(status.dependants.begin(), status.dependants.end(), found->first) ==
                                            status.dependants.end())
                    status.dependants.push_back(found->first);
            }
            status.canUnlearn = status.dependants.empty();
        }
    }

    if (status.held >= static_cast<int>(node.ranks.size()))
    {
        status.block = status.bridge ? PerkBlock::NoEffect : PerkBlock::Maxed;
        return status;
    }

    // The next rank's conditions, each said in words. A HasPerk on this
    // node's own lower rank is the rank chain, which "2/5" says already.
    const PerkRank &next = node.ranks[static_cast<std::size_t>(status.held)];
    bool skillFails = false;
    bool perkFails = false;
    bool groupMet = false;
    std::size_t groupStart = 0;
    for (std::size_t i = 0; i < next.conditions.size(); ++i)
    {
        const Condition &c = next.conditions[i];
        const bool met = evaluator.One(c);
        const bool inGroup = c.orNext || (i > 0 && next.conditions[i - 1].orNext);
        Requirement req;
        req.met = met;
        req.alternative = inGroup;
        switch (c.function)
        {
        case ConditionFunction::GetBaseActorValue:
        case ConditionFunction::GetActorValue:
            if (const auto skill = SkillFromActorValue(c.actorValue))
            {
                req.kind = Requirement::Kind::Skill;
                req.skill = *skill;
                req.need = static_cast<int>(c.value) + (c.comparison == Comparison::Greater ? 1 : 0);
                req.have = rules.skills[Index(*skill)];
            }
            else
            {
                req.kind = Requirement::Kind::Other;
                req.name = "actor value " + std::to_string(c.actorValue);
            }
            break;
        case ConditionFunction::HasPerk:
            req.kind = Requirement::Kind::Perk;
            if (const auto found = rules.graph.Find(c.perk))
            {
                req.node = found->first;
                req.name = rules.graph.Node(found->first).name;
            }
            else
                req.name = ToString(c.perk);
            break;
        case ConditionFunction::Other:
        default:
            req.kind = Requirement::Kind::Other;
            req.name = c.otherName;
            break;
        }
        const bool chain = req.kind == Requirement::Kind::Perk && req.node == nodeId;
        const bool negated = req.kind == Requirement::Kind::Perk && c.value < 1.0f;
        if (!chain && !negated)
            status.requirements.push_back(req);

        groupMet = groupMet || met;
        if (!c.orNext)
        {
            if (!groupMet)
                for (std::size_t j = groupStart; j <= i; ++j)
                {
                    const ConditionFunction f = next.conditions[j].function;
                    if (f == ConditionFunction::HasPerk)
                        perkFails = true;
                    else
                        skillFails = true;
                }
            groupMet = false;
            groupStart = i + 1;
        }
    }
    if (!next.conditions.empty() && next.conditions.back().orNext && !groupMet)
        perkFails = true;

    if (evaluator.IsBridge(nodeId))
        status.block = PerkBlock::NoEffect;
    else if (perkFails)
        status.block = PerkBlock::Requires;
    else if (skillFails)
        status.block = PerkBlock::Skill;
    else if (rules.points <= 0)
        status.block = PerkBlock::NoPoints;
    else
        status.block = PerkBlock::None;
    return status;
}

std::vector<int> WouldBreak(const PerkRules &rules, std::span<const FormKey> removing)
{
    Holdings without = rules.holdings;
    for (const FormKey &form : removing)
    {
        without.innate.erase(form);
        without.learned.erase(form);
    }
    const PerkRules after{rules.graph, without, rules.skills, rules.points, rules.offerNoEffect};
    Evaluator check(after);
    std::vector<int> broken;
    for (const FormKey &form : without.learned)
    {
        const auto found = rules.graph.Find(form);
        if (!found)
            continue;
        const PerkRank &rank = rules.graph.Node(found->first).ranks[static_cast<std::size_t>(found->second)];
        if (!check.Met(rank) && std::find(broken.begin(), broken.end(), found->first) == broken.end())
            broken.push_back(found->first);
    }
    std::sort(broken.begin(), broken.end());
    return broken;
}

std::vector<FormKey> Invalidated(const PerkGraph &graph, const Holdings &holdings, const PerSkill<int> &skills,
                                 bool offerNoEffect)
{
    std::vector<std::pair<int, FormKey>> gone; // rank index, form
    Holdings working = holdings;
    for (;;)
    {
        const PerkRules rules{graph, working, skills, 0, offerNoEffect};
        Evaluator evaluator(rules);
        std::vector<std::pair<int, FormKey>> pass;
        for (const FormKey &form : working.learned)
        {
            const auto found = graph.Find(form);
            if (!found)
                continue;
            const PerkRank &rank = graph.Node(found->first).ranks[static_cast<std::size_t>(found->second)];
            if (!evaluator.Met(rank))
                pass.push_back({found->second, form});
        }
        if (pass.empty())
            break;
        // The set's order is not stable; the result must be.
        std::sort(pass.begin(), pass.end(), [](const auto &a, const auto &b) {
            if (a.first != b.first)
                return a.first > b.first;
            return a.second < b.second;
        });
        for (const auto &[rank, form] : pass)
        {
            working.learned.erase(form);
            gone.push_back({rank, form});
        }
    }
    std::stable_sort(gone.begin(), gone.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    std::vector<FormKey> forms;
    forms.reserve(gone.size());
    for (auto &[rank, form] : gone)
        forms.push_back(std::move(form));
    return forms;
}

} // namespace fp
