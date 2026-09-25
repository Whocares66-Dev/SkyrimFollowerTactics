#pragma once
// The spells an actor knows and can cast: the engine's walk of them, what
// counts as castable or a power, the combat AI's skill gate, the voice's
// recovery, and the editor's spell menu.

#include "core/BagView.h"
#include "core/Blows.h"
#include "core/Breakdown.h"
#include "core/Effects.h"
#include "core/Rule.h"
#include "core/Snapshot.h"
#include "core/Views.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace RE
{
class Actor;
class AlchemyItem;
class BGSAttackData;
struct Effect;
class InventoryEntryData;
class MagicItem;
class SpellItem;
class TESObjectARMO;
class TESObjectWEAP;
} // namespace RE

namespace ft::game
{

// Is the spell kept out of their combat AI's list by skill? The engine's
// list asks EVERY effect with a school for its minimum skill, and one
// short keeps the whole spell out (45328, dev/COMBAT_AI.md "Which spells get
// into the list"): Adamant's Permafrost, at 75 on Ice Storm, kept Serana's
// out at Destruction 50-odd (2026-09-22), where the costliest effect alone
// said 50. The AI's gate, not a casting limit: the UseMagic package would
// cast it regardless, and a rule is kept to what the AI would choose, so
// that cast and pin agree. Never so for the player, who has no such list
// and casts whatever they know.
[[nodiscard]] bool AboveSkillForAI(RE::Actor *actor, const RE::MagicItem *spell);

// The first effect that keeps it out, as AboveSkillForAI reads it: the
// effect's school, its level, and their skill in that school. Empty for a
// spell they may use, and always for the player.
struct SkillGate
{
    RE::ActorValue school{RE::ActorValue::kNone};
    int level{0};
    float has{0.0f};
};
[[nodiscard]] std::optional<SkillGate> FirstSkillGate(RE::Actor *actor, const RE::MagicItem *spell);

// Seconds until the voice can shout again, 0 when it can. The engine
// keeps this per actor, NPCs too; negative or nonsense (an hour or more)
// reads as "can shout".
[[nodiscard]] float VoiceRecoveryOf(RE::Actor *actor);

// One castable spell a follower knows, for the editor's menu.
//
// Name and id together because the menu shows one and stores the other: the
// name is what a player picks by and is translated, the FormID is what the
// rule keeps and what survives a language change. Same split as wire names
// versus display names, for the same reason.
struct SpellOption
{
    std::uint32_t form{0};
    std::string name;
    // Delivery Self: cast on oneself and on no one else. The menu offers it
    // under Self only, and everything aimed under everyone but Self -- save
    // a Location spell (a conjuration), which goes at whoever's feet the
    // rule names, the follower's included, and is offered under everyone.
    bool selfOnly{false};
    bool location{false};
    // Carries a Reanimate-archetype effect: the only kind aimed at a corpse.
    // The archetype is the record property the engine raises a corpse by,
    // so a mod's reanimate spell is found by it whatever it is called.
    bool reanimate{false};
    // CanDualCast's answer; the Dual Cast menu lists these and no other.
    bool dualCast{false};
    // Which menu lists it: Cast spell, Use power, or Shout. One list
    // because all three are found by the same walk of what they know.
    enum class Kind : std::uint8_t
    {
        Spell,
        Power,
        Shout,
        Scroll // carried, not known: read once and spent
    };
    Kind kind{Kind::Spell};
    // Which school the spell and equip menus group it under, by the
    // costliest effect's skill as the Magic tab reads it (game/Magic.h,
    // SchoolOf). Other for a spell with no school -- a vampire's Drain
    // Life, a race's ability cast as a spell. Meaningless on a power or a
    // shout, which are not grouped.
    ft::MagicCategory school{ft::MagicCategory::Other};
};

// Every spell the follower can actually cast, sorted by name.
//
// Sorted here rather than in the UI because the order is a property of the
// list, not of how it is drawn, and doing it once per rebuild beats doing it
// every frame the menu is open.
//
// Filtered to SpellType::kSpell and the two power types, plus the shouts on
// the base record. Abilities, diseases and passive effects also live in an
// actor's spell list and none of them are castable, so offering them would
// be offering rules that can never work.
[[nodiscard]] std::vector<SpellOption> ScanCastableSpells(RE::Actor *actor);

// Castable means SpellType::kSpell; a power is kPower or kLesserPower -- or
// a power a shout slot is leasing, which reads as Voice for the lease
// (Packages.cpp). An actor's spell list also carries abilities, diseases and
// passive racial effects, none of which a follower can choose to cast, so a
// rule naming one could never fire.
[[nodiscard]] bool IsCastable(const RE::SpellItem *spell);
[[nodiscard]] bool IsPower(const RE::SpellItem *spell);

// Walk every spell an actor has, through the engine's own walk
// (Actor::VisitSpells, what HasSpell and the combat AI's inventory use), so a
// spell is here exactly when the game says the actor knows it. It reads four
// places, and missing any loses spells that are plainly there:
//   addedSpells             everything granted at runtime, which is what the
//                           console's addspell writes to.
//   TESNPC::GetSpellList()  what the character was authored with -- Marcurio's
//                           destruction spells come from here.
//   TESRace::actorEffects   the race's: the passive resistances, and the
//                           racial power (Voice of the Emperor on an
//                           Imperial), which is why the Powers chip is not
//                           empty for a vanilla follower.
//   the record's leveled spell lists, as the actor's process resolved them.
// Follower Progression answers the walk for its companions: a spell taught
// from a tome is known without being added, and one of their own set aside
// is not known (its dev/ENGINE_SPELLS.md). Each spell is handed over once.
void ForEachSpell(RE::Actor *actor, const std::function<void(RE::SpellItem *)> &fn);

// Whether the actor can dual cast a spell. The one statement of the rule; the
// menu, the evaluator and the docs point here. A record that fits either
// hand (SpellGrip): the slot decides, never the level, so a mod's one-handed
// master spell is in; a package told to dual cast a one-hand variant fired it
// from that hand alone (Serana's Ice Storm, DLC1IceStormRightHand,
// 2026-09-16). And, where Settings asks for it (dev/PROFILES.md), the perk
// system's answer to the Can Dual Cast Spell entry point, so a mod's perk
// counts the same as the school's Dual Casting perk.
[[nodiscard]] bool CanDualCast(RE::Actor *actor, RE::SpellItem *spell);

// What a dual cast costs the actor: the cost times fMagicDualCastingCostMult
// (2.8 in vanilla), unless the spell is flagged to take no dual-cast change.
[[nodiscard]] float DualCastCost(RE::Actor *actor, RE::SpellItem *spell);

// A corpse-raising effect, by its archetype rather than its name, so a mod's
// own Reanimate counts. Asked of a resolved effect.
[[nodiscard]] bool IsReanimate(const RE::Effect *effect);

// Whose spells a skill keeps out: an NPC's, whose combat AI lists none
// above it (dev/COMBAT_AI.md 0). The player casts whatever they know.
[[nodiscard]] bool SkillGated(RE::Actor *actor);
} // namespace ft::game
