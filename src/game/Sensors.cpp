#include "game/Sensors.h"

#include "game/Pins.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ft::game
{
namespace
{

// Walk every spell an actor has, from both places the game keeps them.
//
// Two sources, and missing either loses spells that are plainly there:
//   TESNPC::GetSpellList()  what the character was authored with -- Marcurio's
//                           destruction spells come from here.
//   addedSpells             everything granted at runtime, which is what the
//                           console's addspell writes to.
template <typename Fn> void ForEachSpell(RE::Actor *actor, Fn &&fn)
{
    if (auto *npc = actor->GetActorBase())
    {
        if (auto *list = npc->GetSpellList())
        {
            for (std::uint32_t i = 0; i < list->numSpells; ++i)
            {
                if (list->spells[i])
                    fn(list->spells[i]);
            }
        }
    }

    for (auto *spell : actor->GetActorRuntimeData().addedSpells)
    {
        if (spell)
            fn(spell);
    }
}

// Castable means SpellType::kSpell. An actor's spell list also carries
// abilities, diseases and passive racial effects, none of which a follower can
// choose to cast, so a rule naming one could never fire.
bool IsCastable(RE::SpellItem *spell)
{
    return spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell;
}

// Highest restore magnitude this potion offers for the given actor value, or 0
// if it does not restore it at all. Poisons and food are filtered out by the
// caller, so anything reaching here that restores health is a healing potion.
float RestoreMagnitude(RE::AlchemyItem *alch, RE::ActorValue av)
{
    float best = 0.0f;
    for (auto *effect : alch->effects)
    {
        if (!effect || !effect->baseEffect)
            continue;
        if (effect->baseEffect->data.primaryAV != av)
            continue;
        best = std::max(best, effect->effectItem.magnitude);
    }
    return best;
}

void RecordPotion(RE::AlchemyItem *alch, std::int32_t count, ft::PotionStock &stock, PotionChoice &choice)
{
    const auto consider = [&](RE::ActorValue av, int &countOut, float &bestOut, RE::AlchemyItem *&chosen) {
        const float mag = RestoreMagnitude(alch, av);
        if (mag <= 0.0f)
            return;
        countOut += count;
        // "Best" is the largest restore. A rule that fires at 30% health wants
        // the strongest thing in the bag, not whichever came first.
        if (mag > bestOut)
        {
            bestOut = mag;
            chosen = alch;
        }
    };

    consider(RE::ActorValue::kHealth, stock.healthCount, stock.bestHealthMagnitude, choice.health);
    consider(RE::ActorValue::kMagicka, stock.magickaCount, stock.bestMagickaMagnitude, choice.magicka);
    consider(RE::ActorValue::kStamina, stock.staminaCount, stock.bestStaminaMagnitude, choice.stamina);
}

void ScanPotions(RE::Actor *actor, ft::PotionStock &stock, PotionChoice &choice)
{
    // Filtered at the source: asking GetInventory for only AlchemyItems is
    // markedly cheaper than pulling the whole inventory and sorting it here,
    // and a follower's bag can be large.
    auto inventory = actor->GetInventory([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::AlchemyItem); });

    for (auto &[object, entry] : inventory)
    {
        const auto count = entry.first;
        if (count <= 0)
            continue;

        auto *alch = object->As<RE::AlchemyItem>();
        if (!alch)
            continue;
        // Poisons are applied to weapons, not drunk; food is a different action
        // with different timing. Neither belongs in the potion stock.
        if (alch->IsPoison() || alch->IsFood())
            continue;

        stock.carried.push_back({alch->GetFormID(), static_cast<int>(count)});
        RecordPotion(alch, count, stock, choice);
    }
}

// Is a restore effect for this actor value still running?
//
// An INSTANT effect has duration 0 and never lingers here, so on a vanilla game
// this always answers false and the settle time in MinimumCooldown does the
// spacing. Potion overhauls convert restores to over-time effects, and there
// this is the exact answer where a fixed settle would be a guess.
//
// Deliberately not restricted to effects whose source is a potion: a healing
// spell or a regeneration enchantment ticking away is just as good a reason not
// to drink, and asking "is this stat already being restored" says that in one
// question.
bool RestoreEffectRunning(RE::Actor *actor, RE::ActorValue av)
{
    auto *target = actor->AsMagicTarget();
    if (!target)
        return false;

    auto *effects = target->GetActiveEffectList();
    if (!effects)
        return false;

    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        if (ae->effect->baseEffect->data.primaryAV != av)
            continue;
        // duration 0 is an instant effect that has already happened.
        if (ae->duration > 0.0f && ae->elapsedSeconds < ae->duration)
            return true;
    }
    return false;
}

ft::Stat ReadStat(RE::Actor *actor, RE::ActorValue av)
{
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return {};
    // Current is the damaged value; permanent is base plus permanent modifiers,
    // i.e. the maximum the bar can show. Their ratio is what the rules read, so
    // both are logged in Tactics.cpp to make a wrong reading visible rather
    // than merely wrong.
    return ft::Stat{owner->GetActorValue(av), owner->GetPermanentActorValue(av)};
}

// Defined further down, in this same unnamed namespace, with the sheets.
SheetRow Row(std::string label, std::string value);

} // namespace

// "3 min 24 s", "1 h 5 min", "12 s"; nothing for an effect with no
// duration, an ability's or an enchantment's.
std::string RemainingText(float seconds)
{
    if (seconds < 0.0f)
        return {};
    const int total = static_cast<int>(std::lround(seconds));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int secs = total % 60;
    char buf[32];
    if (hours > 0)
        std::snprintf(buf, sizeof(buf), "%d h %d min", hours, minutes);
    else if (minutes > 0)
        std::snprintf(buf, sizeof(buf), "%d min %d s", minutes, secs);
    else
        std::snprintf(buf, sizeof(buf), "%d s", secs);
    return buf;
}

// The worn item carrying this enchantment, by the name the game shows for
// it, or empty if none is worn.
std::string WornSourceOf(RE::Actor *actor, const RE::MagicItem *magic)
{
    for (const auto &[object, entry] : actor->GetInventory())
    {
        if (!object || entry.first <= 0 || !entry.second || !entry.second->IsWorn())
            continue;
        if (entry.second->GetEnchantment() != magic)
            continue;
        const char *given = entry.second->GetDisplayName();
        if (given && *given)
            return given;
        return object->GetName() ? object->GetName() : "";
    }
    return {};
}

// The effect's description with <mag> and <dur> filled in. Skyrim.esm
// writes the tokens in lower case; mods are not so consistent, and the
// engine takes either.
std::string EffectDescription(const RE::EffectSetting *base, float magnitude, float duration)
{
    const char *text = base->magicItemDescription.c_str();
    std::string line = text ? text : "";
    const auto replace = [&line](std::string_view token, const std::string &with) {
        const auto same = [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        };
        auto at = std::search(line.begin(), line.end(), token.begin(), token.end(), same);
        while (at != line.end())
        {
            const auto index = static_cast<std::size_t>(at - line.begin());
            line.replace(index, token.size(), with);
            at = std::search(line.begin() + static_cast<std::ptrdiff_t>(index + with.size()), line.end(), token.begin(),
                             token.end(), same);
        }
    };
    char num[32];
    std::snprintf(num, sizeof(num), "%.0f", magnitude);
    replace("<mag>", num);
    std::snprintf(num, sizeof(num), "%.0f", duration);
    replace("<dur>", num);
    return line;
}

std::vector<EffectRow> ScanActiveEffects(RE::Actor *actor)
{
    std::vector<EffectRow> out;
    auto *target = actor ? actor->AsMagicTarget() : nullptr;
    auto *effects = target ? target->GetActiveEffectList() : nullptr;
    if (!effects)
        return out;

    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        const auto *base = ae->effect->baseEffect;
        // As the game's own Active Effects list: hidden ones stay hidden,
        // and one that has run out is gone.
        if (base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kHideInUI))
            continue;
        if (ae->duration > 0.0f && ae->elapsedSeconds >= ae->duration)
            continue;
        const char *name = base->GetName();
        if (!name || !*name)
            continue;

        EffectRow row;
        row.form = base->GetFormID();
        row.sourceForm = ae->spell ? ae->spell->GetFormID() : 0;
        row.name = name;
        row.magnitude = ae->magnitude;
        row.duration = ae->duration;
        row.remaining = ae->duration > 0.0f ? ae->duration - ae->elapsedSeconds : -1.0f;
        row.remainingText = RemainingText(row.remaining);
        if (ae->spell)
        {
            if (ae->spell->As<RE::EnchantmentItem>())
                row.source = WornSourceOf(actor, ae->spell);
            if (row.source.empty() && ae->spell->GetName())
                row.source = ae->spell->GetName();
        }

        // The page.
        SheetSection stats{"Effect", {}, {}};
        char num[32];
        std::snprintf(num, sizeof(num), "%.0f", row.magnitude);
        stats.rows.push_back(Row("Magnitude", num));
        stats.rows.push_back(Row("Duration", ae->duration > 0.0f ? RemainingText(ae->duration) : "none"));
        if (row.remaining >= 0.0f)
            stats.rows.push_back(Row("Remaining", row.remainingText));
        if (!row.source.empty())
            stats.rows.push_back(Row("Source", row.source));
        // Whoever cast it, when it was not the follower: the player's
        // Courage, an enemy's Fury.
        if (auto caster = ae->caster.get(); caster && caster.get() != actor && caster->GetName() && *caster->GetName())
            stats.rows.push_back(Row("Caster", caster->GetName()));
        row.detail.push_back(std::move(stats));
        row.description = EffectDescription(base, row.magnitude, row.duration);

        out.push_back(std::move(row));
    }

    std::sort(out.begin(), out.end(),
              [](const EffectRow &a, const EffectRow &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

void LogActiveEffects(RE::Actor *actor, const char *when)
{
    auto *target = actor ? actor->AsMagicTarget() : nullptr;
    if (!target)
        return;

    auto *effects = target->GetActiveEffectList();
    if (!effects)
    {
        logger::info("  active effects [{}]: <none>", when);
        return;
    }

    int count = 0;
    for (auto *ae : *effects)
    {
        if (!ae || !ae->effect || !ae->effect->baseEffect)
            continue;
        ++count;

        const auto *base = ae->effect->baseEffect;
        const char *sourceName = ae->spell ? ae->spell->GetName() : "<none>";
        logger::info("  active effect [{}]: \"{}\" from \"{}\"  elapsed {:.1f}/{:.1f}s  mag {:.1f}", when,
                     base->GetName(), sourceName, ae->elapsedSeconds, ae->duration, ae->magnitude);
    }

    if (count == 0)
        logger::info("  active effects [{}]: <none>", when);
}

ft::Snapshot BuildSnapshot(RE::Actor *actor, double now, PotionChoice &choice)
{
    ft::Snapshot s;
    choice = {};

    if (!actor)
        return s;

    s.self = actor->GetFormID();
    s.now = now;

    s.health = ReadStat(actor, RE::ActorValue::kHealth);
    s.magicka = ReadStat(actor, RE::ActorValue::kMagicka);
    s.stamina = ReadStat(actor, RE::ActorValue::kStamina);

    s.inCombat = actor->IsInCombat();
    if (auto *state = actor->AsActorState())
    {
        s.inBleedout = state->IsBleedingOut();
        s.weaponDrawn = state->IsWeaponDrawn();
        s.sneaking = state->IsSneaking();
    }

    if (auto *player = RE::PlayerCharacter::GetSingleton())
    {
        s.playerHealth = ReadStat(player, RE::ActorValue::kHealth);
        s.playerInCombat = player->IsInCombat();
        s.distanceToPlayer = actor->GetPosition().GetDistance(player->GetPosition());
    }

    // Whom she is fighting, as the engine sees it. This is what "current
    // target" resolves to, and the one enemy the snapshot carries until the
    // combat group is read in full (Phase 2).
    if (auto target = actor->GetActorRuntimeData().currentCombatTarget.get(); target && !target->IsDead())
    {
        s.currentTarget = target->GetFormID();

        ft::EnemyView enemy;
        enemy.id = target->GetFormID();
        enemy.health = ReadStat(target.get(), RE::ActorValue::kHealth);
        enemy.distance = actor->GetPosition().GetDistance(target->GetPosition());
        if (auto *player = RE::PlayerCharacter::GetSingleton())
        {
            auto theirTarget = target->GetActorRuntimeData().currentCombatTarget.get();
            enemy.isAttackingPlayer = theirTarget && theirTarget.get() == player;
        }
        bool losArg = false;
        enemy.hasLineOfSight = actor->HasLineOfSight(target.get(), losArg);
        s.enemies.push_back(enemy);
    }

    ScanPotions(actor, s.potions, choice);

    s.potions.healthEffectActive = RestoreEffectRunning(actor, RE::ActorValue::kHealth);
    s.potions.magickaEffectActive = RestoreEffectRunning(actor, RE::ActorValue::kMagicka);
    s.potions.staminaEffectActive = RestoreEffectRunning(actor, RE::ActorValue::kStamina);

    // Spells: what she knows, what is running, what is in hand. All three are
    // ids only -- Snapshot never sees an RE:: type -- and all three are needed
    // to tell "cannot", "already up" and "already held" apart in the status
    // column.
    ForEachSpell(actor, [&s, actor](RE::SpellItem *spell) {
        if (!IsCastable(spell))
            return;
        s.spells.known.push_back(spell->GetFormID());
        // Her cost, not the base cost: CalculateMagickaCost applies her skill
        // and perks, which is what the AI will charge her.
        s.spells.costs.push_back({spell->GetFormID(), spell->CalculateMagickaCost(actor)});
        // And as the pin book sees it, for an equip rule.
        s.loadout.push_back(DescribeHoldable(actor, spell));
    });

    // What she could hold or wear, as the pin book sees it, and what is
    // pinned. A walk of her inventory that keeps only the equipable kinds;
    // the potion scan above walks it too, and the two could share one pass
    // if the cost ever showed, which at tens of microseconds it does not.
    for (const auto &[object, entry] : actor->GetInventory())
    {
        if (!object || entry.first <= 0)
            continue;
        if (!(object->Is(RE::FormType::Weapon) || object->Is(RE::FormType::Armor) || object->Is(RE::FormType::Ammo) ||
              object->Is(RE::FormType::Light)))
            continue;
        s.loadout.push_back(DescribeHoldable(actor, object));
    }
    s.pins = PinsOf(s.self);

    if (auto *target = actor->AsMagicTarget())
    {
        if (auto *effects = target->GetActiveEffectList())
        {
            for (auto *ae : *effects)
            {
                if (!ae || !ae->spell)
                    continue;
                // Instant effects have already happened and never lapse, so
                // treating them as "still up" would block the rule forever.
                if (ae->duration <= 0.0f)
                    continue;
                if (ae->elapsedSeconds >= ae->duration)
                    continue;
                s.spells.active.push_back(ae->spell->GetFormID());
            }
        }
    }

    // selectedSpells is indexed by Actor::SlotTypes, NOT by
    // MagicSystem::CastingSource. The two enums start with the same two names
    // in the same order, which makes mixing them up easy and silent.
    for (const auto slot : {RE::Actor::SlotTypes::kLeftHand, RE::Actor::SlotTypes::kRightHand})
    {
        if (auto *held = actor->GetActorRuntimeData().selectedSpells[slot])
            s.spells.equipped.push_back(held->GetFormID());
    }

    // enemies / allies deliberately left empty -- see the header.
    return s;
}

std::vector<PotionOption> ScanCarriedPotions(RE::Actor *actor)
{
    std::vector<PotionOption> out;
    if (!actor)
        return out;

    auto inventory = actor->GetInventory([](RE::TESBoundObject &obj) { return obj.Is(RE::FormType::AlchemyItem); });
    for (auto &[object, entry] : inventory)
    {
        const auto count = entry.first;
        auto *alch = object->As<RE::AlchemyItem>();
        if (count <= 0 || !alch || alch->IsPoison() || alch->IsFood())
            continue;
        out.push_back({alch->GetFormID(), alch->GetName() ? alch->GetName() : "?", static_cast<int>(count)});
    }
    std::sort(out.begin(), out.end(), [](const PotionOption &a, const PotionOption &b) { return a.name < b.name; });
    return out;
}

std::vector<SpellOption> ScanCastableSpells(RE::Actor *actor)
{
    std::vector<SpellOption> out;
    if (!actor)
        return out;

    ForEachSpell(actor, [&out, actor](RE::SpellItem *spell) {
        if (!IsCastable(spell))
            return;
        // Above the follower's skill: not offered for casting, as it is not
        // for pinning, so the two menus agree on what they can use.
        if (DescribeHoldable(actor, spell).unusable)
            return;
        // The same spell can appear in both sources; show it once.
        const std::uint32_t id = spell->GetFormID();
        if (std::any_of(out.begin(), out.end(), [id](const SpellOption &o) { return o.form == id; }))
            return;

        std::string name = spell->GetName() ? spell->GetName() : "";
        if (name.empty())
            return; // nameless entries are internal; nothing to show a player
        out.push_back(SpellOption{id, std::move(name)});
    });

    std::sort(out.begin(), out.end(), [](const SpellOption &a, const SpellOption &b) { return a.name < b.name; });
    return out;
}

// --- character sheet ---------------------------------------------------------

namespace
{

std::string Fmt(const char *fmt, double value)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, value);
    return buf;
}

// A float game setting, or the vanilla value if the collection has no such
// entry. The fallbacks are vanilla's numbers so a missing setting degrades to
// "what the unmodded game does", not to a zero that reads as a broken sheet.
float GameSetting(const char *name, float vanilla)
{
    auto *collection = RE::GameSettingCollection::GetSingleton();
    auto *setting = collection ? collection->GetSetting(name) : nullptr;
    return setting ? setting->GetFloat() : vanilla;
}

// "83%", or past the engine's cap "85% (110.00%)": what is actually applied
// first, what the gear adds up to in brackets. The bracketed number is the one
// worth seeing when it is there -- it says how much of the follower's kit is
// doing nothing.
std::string CappedPercent(float value, float cap)
{
    if (value > cap)
        return Fmt("%.0f%%", cap) + " (" + Fmt("%.2f%%", value) + ")";
    return Fmt("%.0f%%", value);
}

SheetRow Row(std::string label, std::string value)
{
    SheetRow row;
    row.label = std::move(label);
    row.value = std::move(value);
    return row;
}

std::string NameOr(const RE::TESForm *form, const char *fallback)
{
    return form && form->GetName() && *form->GetName() ? form->GetName() : fallback;
}

// --- perks -------------------------------------------------------------------

struct TreePerk
{
    RE::BGSPerk *perk;
    int rank;          // 1-based position in the perk's rank chain
    int ranks;         // length of that chain
    float requirement; // the skill level the perk asks for; 0 if it asks nothing
    std::string description;
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
    auto *info = list ? list->GetActorValue(skill) : nullptr;
    if (info && info->perkTree)
    {
        std::vector<RE::BGSSkillPerkTreeNode *> stack{info->perkTree};
        std::unordered_set<const RE::BGSSkillPerkTreeNode *> seen;
        while (!stack.empty())
        {
            auto *node = stack.back();
            stack.pop_back();
            if (!node || !seen.insert(node).second)
                continue;

            // A node names the first rank; the rest chain through nextPerk.
            // Bounded, because a malformed chain that loops would hang the
            // game thread, and a chain longer than this is not a rank chain.
            std::vector<RE::BGSPerk *> chain;
            for (auto *perk = node->perk; perk && chain.size() < 16; perk = perk->nextPerk)
                chain.push_back(perk);
            for (std::size_t i = 0; i < chain.size(); ++i)
            {
                RE::BSString text;
                chain[i]->GetDescription(text, chain[i]);
                out.push_back({chain[i], static_cast<int>(i) + 1, static_cast<int>(chain.size()),
                               SkillRequirement(chain[i], skill), text.c_str() ? text.c_str() : ""});
            }

            for (auto *child : node->children)
                stack.push_back(child);
        }
    }

    // Least demanding first: the requirement is the game's own statement of
    // how strong a perk is, so the list reads weakest to strongest.
    std::sort(out.begin(), out.end(), [](const TreePerk &a, const TreePerk &b) {
        if (a.requirement != b.requirement)
            return a.requirement < b.requirement;
        return a.rank < b.rank;
    });
    return cache.emplace(skill, std::move(out)).first->second;
}

// The perks this follower holds in one skill's tree, one row per perk at
// the highest rank held. Asked of the engine with HasPerk rather than read
// off her record, so a perk a mod granted at runtime counts the same as one
// she was authored with. Ordered by the skill level each perk asks for,
// weakest first; the modifiers column carries its own in-game description.
std::vector<SheetRow> OwnedPerks(RE::Actor *actor, RE::ActorValue skill)
{
    std::vector<SheetRow> rows;
    for (const TreePerk &entry : TreePerks(skill))
    {
        if (!actor->HasPerk(entry.perk))
            continue;
        if (entry.perk->nextPerk && actor->HasPerk(entry.perk->nextPerk))
            continue; // a higher rank is held; that one gets the row

        // Trimmed, because the records are not: Skyrim.esm's first rank of
        // Magic Resistance is named " Magic Resistance", leading space and
        // all, and on screen that reads as a row set in for no reason.
        std::string label = entry.perk->GetName() ? entry.perk->GetName() : "?";
        const auto first = label.find_first_not_of(' ');
        const auto last = label.find_last_not_of(' ');
        label = first == std::string::npos ? "?" : label.substr(first, last - first + 1);

        const std::string rank = entry.ranks > 1 ? std::to_string(entry.rank) + "/" + std::to_string(entry.ranks) : "";
        SheetRow row = Row(std::move(label), rank);
        row.modifiers = entry.description;
        rows.push_back(std::move(row));
    }
    return rows;
}

// What one hand holds, as rows: a weapon and its numbers, a spell and its
// cost and strongest effect, a shield and its rating, or a torch. An empty
// hand adds no rows, and the caller shows no table for it. A two-hander
// shows in the right hand and the left says so.
void HandRows(RE::Actor *actor, bool left, std::vector<SheetRow> &rows)
{
    RE::TESForm *held = actor->GetEquippedObject(left);
    if (!held)
        return;

    if (auto *weapon = held->As<RE::TESObjectWEAP>())
    {
        const bool twoHanded =
            weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe() || weapon->IsBow() || weapon->IsCrossbow();
        if (left && twoHanded)
        {
            rows.push_back(Row("Held", "the same, two-handed"));
            return;
        }
        rows.push_back(Row("Weapon", NameOr(weapon, "?")));
        rows.back().form = weapon->GetFormID();
        // In her hands: the carried item, for its tempering.
        auto inventory = actor->GetInventory([weapon](RE::TESBoundObject &o) { return &o == weapon; });
        const auto found = inventory.find(weapon);
        auto *entry = found != inventory.end() ? found->second.second.get() : nullptr;
        rows.push_back(Row("Damage", Fmt("%.0f", WeaponDamage(actor, weapon, entry))));
        rows.push_back(Row("Speed", Fmt("%.2f", weapon->GetSpeed())));
        rows.push_back(Row("Reach", Fmt("%.2f", weapon->GetReach())));
        rows.push_back(Row("Stagger", Fmt("%.2f", weapon->GetStagger())));
        if (weapon->IsBow() || weapon->IsCrossbow())
        {
            if (auto *ammo = actor->GetCurrentAmmo())
            {
                rows.push_back(Row("Ammo", NameOr(ammo, "?")));
                rows.back().form = ammo->GetFormID();
                rows.push_back(Row("Ammo Damage", Fmt("%.0f", ammo->GetRuntimeData().data.damage)));
            }
            else
            {
                rows.push_back(Row("Ammo", "none"));
            }
        }
        return;
    }

    if (auto *spell = held->As<RE::SpellItem>())
    {
        rows.push_back(Row("Spell", NameOr(spell, "?")));
        rows.push_back(Row("Cost", Fmt("%.0f", spell->CalculateMagickaCost(actor))));
        if (const auto *effect = spell->GetCostliestEffectItem(); effect && effect->baseEffect)
        {
            std::string what = NameOr(effect->baseEffect, "?");
            what += " " + Fmt("%.0f", effect->effectItem.magnitude);
            if (effect->effectItem.duration > 0)
                what += " for " + std::to_string(effect->effectItem.duration) + " s";
            rows.push_back(Row("Effect", what));
        }
        return;
    }

    if (auto *armor = held->As<RE::TESObjectARMO>())
    {
        const bool shield = armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kShield);
        rows.push_back(Row(shield ? "Shield" : "Held", NameOr(armor, "?")));
        rows.back().form = armor->GetFormID();
        auto inventory = actor->GetInventory([armor](RE::TESBoundObject &o) { return &o == armor; });
        const auto found = inventory.find(armor);
        auto *entry = found != inventory.end() ? found->second.second.get() : nullptr;
        rows.push_back(Row("Armor", Fmt("%.0f", ArmorRating(actor, armor, entry))));
        return;
    }

    if (held->Is(RE::FormType::Light))
    {
        rows.push_back(Row("Held", NameOr(held, "torch")));
        rows.back().form = held->GetFormID();
        return;
    }

    rows.push_back(Row("Held", NameOr(held, "?")));
}

} // namespace

namespace
{

// Tempering lives on the carried item, not the record: an item's health is
// 1.0 untempered and climbs with each visit to a grindstone or workbench,
// and the engine multiplies damage and armour by it.
float Tempering(RE::InventoryEntryData *entry)
{
    if (!entry || !entry->extraLists)
        return 1.0f;
    for (auto *list : *entry->extraLists)
    {
        if (auto *health = list ? list->GetByType<RE::ExtraHealth>() : nullptr; health && health->health > 0.0f)
            return health->health;
    }
    return 1.0f;
}

} // namespace

float WeaponDamage(RE::Actor *actor, RE::TESObjectWEAP *weapon, RE::InventoryEntryData *entry)
{
    if (!actor || !weapon)
        return 0.0f;
    float damage = weapon->GetAttackDamage() * Tempering(entry);

    // The skill curve: UESP gives it as (1 + skill / 200), which is what the
    // fallbacks below encode. The settings are read by the names the engine
    // uses so a rebalancing mod that changes them is honoured; the resolved
    // curve is logged once so a wrong name shows up as a wrong number in the
    // log rather than as a silently vanilla curve.
    using AV = RE::ActorValue;
    AV skill = AV::kOneHanded;
    AV fortify = AV::kOneHandedModifier;
    AV fortifyPower = AV::kOneHandedPowerModifier;
    if (weapon->IsTwoHandedSword() || weapon->IsTwoHandedAxe())
    {
        skill = AV::kTwoHanded;
        fortify = AV::kTwoHandedModifier;
        fortifyPower = AV::kTwoHandedPowerModifier;
    }
    else if (weapon->IsBow() || weapon->IsCrossbow())
    {
        skill = AV::kArchery;
        fortify = AV::kMarksmanModifier;
        fortifyPower = AV::kMarksmanPowerModifier;
    }

    static const float curveBase = GameSetting("fDamageSkillBase", 1.0f);
    static const float curveMult = GameSetting("fDamageSkillMult", 0.5f);
    static const bool logged = [] {
        logger::info("damage: skill curve base {:.2f} + {:.2f} * skill/100", curveBase, curveMult);
        return true;
    }();
    (void)logged;

    auto *owner = actor->AsActorValueOwner();
    const float skillLevel = owner ? owner->GetActorValue(skill) : 0.0f;
    damage *= curveBase + curveMult * skillLevel / 100.0f;

    // Perks, through the engine's own entry point, so Armsman and the rest
    // count exactly as they do in a swing. The entry point wants a target,
    // and there is none outside a fight; she stands in for it herself. A
    // perk that reads the target (against undead, say) evaluates against
    // her and so stays out of the figure -- the same figure the player's
    // own inventory menu shows, which has no target either.
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModAttackDamage, actor, weapon, actor,
                                        &damage);

    // Fortify One-handed and its kin: enchantments on the first value,
    // potions on the second, both in percent.
    if (owner)
        damage *= 1.0f + (owner->GetActorValue(fortify) + owner->GetActorValue(fortifyPower)) / 100.0f;

    return damage;
}

float ArmorRating(RE::Actor *actor, RE::TESObjectARMO *armor, RE::InventoryEntryData *entry)
{
    if (!actor || !armor)
        return 0.0f;
    using Class = RE::BGSBipedObjectForm::ArmorType;
    const Class armorClass = armor->GetArmorType();
    if (armorClass == Class::kClothing)
        return 0.0f;
    float rating = armor->GetArmorRating() * Tempering(entry);

    // The skill curve. UESP gives displayed armour as base * (1 + 0.4 *
    // skill / 100), which the fallbacks encode; the names are the engine's,
    // logged once, as for damage.
    using AV = RE::ActorValue;
    const bool heavy = armorClass == Class::kHeavyArmor;
    const AV skill = heavy ? AV::kHeavyArmor : AV::kLightArmor;
    const AV fortify = heavy ? AV::kHeavyArmorModifier : AV::kLightArmorModifier;
    const AV fortifyPower = heavy ? AV::kHeavyArmorPowerModifier : AV::kLightArmorPowerModifier;

    static const float curveBase = GameSetting("fArmorSkillBase", 1.0f);
    static const float curveMult = GameSetting("fArmorSkillMult", 0.4f);
    static const bool logged = [] {
        logger::info("armor: skill curve base {:.2f} + {:.2f} * skill/100", curveBase, curveMult);
        return true;
    }();
    (void)logged;

    auto *owner = actor->AsActorValueOwner();
    const float skillLevel = owner ? owner->GetActorValue(skill) : 0.0f;
    rating *= curveBase + curveMult * skillLevel / 100.0f;

    // Perks: Juggernaut, Agile Defender and their kin, through the engine's
    // entry point for armour, which takes the piece and the value.
    RE::BGSEntryPoint::HandleEntryPoint(RE::BGSEntryPoint::ENTRY_POINT::kModArmorRating, actor, armor, &rating);

    if (owner)
        rating *= 1.0f + (owner->GetActorValue(fortify) + owner->GetActorValue(fortifyPower)) / 100.0f;
    return rating;
}

std::vector<SheetSection> BuildCharacterSheet(RE::Actor *actor)
{
    std::vector<SheetSection> out;
    if (!actor)
        return out;
    auto *owner = actor->AsActorValueOwner();
    if (!owner)
        return out;

    const auto av = [owner](RE::ActorValue value) { return owner->GetActorValue(value); };

    {
        SheetSection s{"General", {}, {}};
        auto *race = actor->GetRace();
        s.rows.push_back(Row("Race", race && race->GetName() ? race->GetName() : "?"));
        s.rows.push_back(Row("Speed", Fmt("%.0f%%", av(RE::ActorValue::kSpeedMult))));
        s.rows.push_back(Row("Noise", Fmt("%.0f%%", av(RE::ActorValue::kMovementNoiseMult) * 100.0)));
        out.push_back(std::move(s));
    }

    // Attack: what each hand holds, whatever it is. The old Attack section
    // knew only weapons, which left a mage's page saying "unarmed". A hand
    // holding nothing gets no table; with both empty, the one thing worth
    // saying is what her fists do.
    {
        SheetSection right{"Right Hand", {}, "Attack"};
        HandRows(actor, false, right.rows);
        SheetSection left{"Left Hand", {}, "Attack"};
        HandRows(actor, true, left.rows);

        if (right.rows.empty() && left.rows.empty())
        {
            SheetSection s{"Attack", {}, {}};
            s.rows.push_back(Row("Held", "unarmed"));
            s.rows.push_back(Row("Base Damage", Fmt("%.0f", av(RE::ActorValue::kUnarmedDamage))));
            out.push_back(std::move(s));
        }
        else
        {
            if (!right.rows.empty())
                out.push_back(std::move(right));
            if (!left.rows.empty())
                out.push_back(std::move(left));
        }
    }

    {
        SheetSection s{"Defence", {}, {}};
        // The armour rating the game shows is not the one it applies. Each of
        // the four main pieces worn adds a hidden 25 before the scaling factor,
        // which is why a displayed 609 lands at 85% and not 73%. Whether a
        // shield also counts is disputed; it is left out here.
        const float armor = av(RE::ActorValue::kDamageResist);
        float hidden = 0.0f;
        using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
        for (const Slot slot : {Slot::kBody, Slot::kHead, Slot::kHands, Slot::kFeet})
        {
            if (actor->GetWornArmor(slot))
                hidden += 25.0f;
        }
        const float armorPct = (armor + hidden) * GameSetting("fArmorScalingFactor", 0.12f);
        const float resistCap = GameSetting("fPlayerMaxResistance", 85.0f);

        s.rows.push_back(Row("Armor", Fmt("%.0f", armor)));
        s.rows.push_back(Row("Resist Damage", CappedPercent(armorPct, GameSetting("fMaxArmorRating", 80.0f))));
        s.rows.push_back(Row("Resist Disease", Fmt("%.0f%%", av(RE::ActorValue::kResistDisease))));
        s.rows.push_back(Row("Resist Poison", CappedPercent(av(RE::ActorValue::kPoisonResist), resistCap)));
        s.rows.push_back(Row("Resist Fire", CappedPercent(av(RE::ActorValue::kResistFire), resistCap)));
        s.rows.push_back(Row("Resist Frost", CappedPercent(av(RE::ActorValue::kResistFrost), resistCap)));
        s.rows.push_back(Row("Resist Shock", CappedPercent(av(RE::ActorValue::kResistShock), resistCap)));
        s.rows.push_back(Row("Resist Magic", CappedPercent(av(RE::ActorValue::kResistMagic), resistCap)));
        out.push_back(std::move(s));
    }

    {
        SheetSection s{"Regen", {}, {}};
        s.rows.push_back(Row("Health Rate", Fmt("%.2f%%", av(RE::ActorValue::kHealRate))));
        s.rows.push_back(Row("Stamina Rate", Fmt("%.2f%%", av(RE::ActorValue::kStaminaRate))));
        s.rows.push_back(Row("Magicka Rate", Fmt("%.2f%%", av(RE::ActorValue::kMagickaRate))));
        out.push_back(std::move(s));
    }

    return out;
}

std::vector<SheetSection> BuildCombatStyleSheet(RE::Actor *actor)
{
    std::vector<SheetSection> out;
    if (!actor)
        return out;
    auto *npc = actor->GetActorBase();
    auto *record = npc ? npc->GetCombatStyle() : nullptr;
    auto *controller = actor->GetActorRuntimeData().combatController;
    auto *live = controller && controller->combatStyle ? controller->combatStyle : record;
    if (!live)
        return out;

    // Two scales, read off every style in the load order (163 of them):
    // the chances and movement multipliers run 0 to 1, and the score and
    // attack multipliers run 0 to 10, with 1 as the neutral value.
    const auto chance = [](float x) { return Fmt("%.2f", x) + " / 1"; };
    const auto score = [](float x) { return Fmt("%.2f", x) + " / 10"; };
    // Hover text: the Creation Kit wiki's word on each field, as bullets.
    // docs/COMBAT_STYLE.md has the page.
    const auto note = [](SheetRow row, const char *text) {
        row.note = text;
        return row;
    };

    using Flag = RE::TESCombatStyle::FLAG;
    const bool flanking = live->flags.all(Flag::kFlankingStyle);
    {
        SheetSection s{"Style", {}, {}};
        // A runtime copy has a 0xFF FormID; a record's is its plugin's.
        const bool ours = (live->GetFormID() & 0xFF000000U) == 0xFF000000U;
        char id[16];
        std::snprintf(id, sizeof(id), "%08X", live->GetFormID());
        s.rows.push_back(Row("Record", ours ? std::string(id) + "  (our copy)" : id));
        if (controller && controller->combatStyle && record && controller->combatStyle != record)
        {
            char recordId[16];
            std::snprintf(recordId, sizeof(recordId), "%08X", record->GetFormID());
            s.rows.push_back(Row("On Record", recordId));
        }
        s.rows.push_back(note(Row("Close Range", flanking ? "Flanking" : "Dueling"),
                              "- Dueling: circles, falls back\n"
                              "- Flanking: keeps a distance, stalks\n"
                              "- One or the other"));
        s.rows.push_back(note(Row("Dual Wield", live->flags.all(Flag::kAllowDualWielding) ? "allowed" : "no"),
                              "- May hold a weapon in each hand\n"
                              "- Humanoids only"));
        out.push_back(std::move(s));
    }
    {
        const auto &g = live->generalData;
        SheetSection s{"General", {}, {}};
        s.rows.push_back(note(Row("Offensive", chance(g.offensiveMult)), "- Higher: attacks more often\n"
                                                                         "- More power attacks\n"
                                                                         "- Paired with Defensive"));
        s.rows.push_back(note(Row("Defensive", chance(g.defensiveMult)), "- Higher: blocks more, holds it longer\n"
                                                                         "- Bashes more, given a shield or a weapon"));
        s.rows.push_back(note(Row("Group Offensive", chance(g.groupOffensiveMult)),
                              "- Replaces Offensive when several attack one target\n"
                              "- Higher: stays offensive in a crowd"));
        out.push_back(std::move(s));
    }
    {
        // The six that decide what she prefers to hold.
        const auto &g = live->generalData;
        SheetSection s{"Equipment Scores", {}, {}};
        constexpr const char *kScore = "- Multiplies the damage of attacks of this kind\n"
                                       "- The highest score is what gets used\n"
                                       "- A weak weapon needs a high score to beat a strong spell";
        s.rows.push_back(note(Row("Melee", score(g.meleeScoreMult)), kScore));
        s.rows.push_back(note(Row("Magic", score(g.magicScoreMult)), kScore));
        s.rows.push_back(note(Row("Ranged", score(g.rangedScoreMult)), kScore));
        s.rows.push_back(note(Row("Staff", score(g.staffScoreMult)), kScore));
        s.rows.push_back(note(Row("Shout", score(g.shoutScoreMult)), kScore));
        s.rows.push_back(note(Row("Unarmed", score(g.unarmedScoreMult)), kScore));
        out.push_back(std::move(s));
    }
    {
        const auto &m = live->meleeData;
        SheetSection s{"Melee", {}, {}};
        s.rows.push_back(note(Row("Attack, Staggered", score(m.attackIncapacitatedMult)),
                              "- Higher: attacks a staggered target more"));
        s.rows.push_back(note(Row("Power Attack, Staggered", score(m.powerAttackIncapacitatedMult)),
                              "- Higher: power-attacks a staggered target more"));
        s.rows.push_back(note(Row("Power Attack, Blocking", score(m.powerAttackBlockingMult)),
                              "- Higher: power-attacks a blocking target more\n"
                              "- Breaks the block"));
        s.rows.push_back(note(Row("Bash", score(m.bashMult)), "- Higher: bashes more, with a shield or a bash attack\n"
                                                              "- A bash can stagger"));
        s.rows.push_back(note(Row("Bash, Recoiled", score(m.bashRecoilMult)),
                              "- Higher: bashes a target recoiling from its blocked attack"));
        s.rows.push_back(note(Row("Bash, Attacking", score(m.bashAttackMult)), "- Higher: bashes a target mid-attack"));
        s.rows.push_back(note(Row("Bash, Power Attacking", score(m.bashPowerAttackMult)),
                              "- Higher: bashes a target mid-power-attack"));
        out.push_back(std::move(s));
    }
    {
        // Only the active pair: dueling circles and falls back, flanking
        // keeps a distance and stalks. The other pair is dead data.
        const auto &c = live->closeRangeData;
        SheetSection s{"Range", {}, {}};
        if (flanking)
        {
            s.rows.push_back(
                note(Row("Flank Distance", chance(c.flankDistanceMult)), "- Distance kept while flanking"));
            s.rows.push_back(
                note(Row("Stalk Time", chance(c.stalkTimeMult)), "- Time spent flanking before attacking"));
        }
        else
        {
            s.rows.push_back(note(Row("Circle", chance(c.circleMult)), "- Higher: circles the target more"));
            s.rows.push_back(note(Row("Fallback", chance(c.fallbackMult)), "- Chance to back off"));
        }
        s.rows.push_back(note(Row("Strafe", chance(live->longRangeData.strafeMult)),
                              "- Higher: strafes more to dodge projectiles at range"));
        out.push_back(std::move(s));
    }
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
    // rather than a wiki (docs/RESEARCH.md, "Skill modifiers").
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
        const float m = k.mod.effect ? av(k.mod.value) : 0.0f;
        const float p = k.power.effect ? av(k.power.value) : 0.0f;

        // Every modifier is a signed change from normal: "+90% damage",
        // "-17% cost". Power first, then the other, as the two read best.
        const auto add = [&row](const std::string &text) {
            row.modifiers += (row.modifiers.empty() ? "" : ", ") + text;
        };
        if (k.mod.effect && k.power.effect && std::string_view(k.mod.effect) == k.power.effect)
        {
            // One quantity, two factors: multiply them and show the change.
            if (m != 0.0f || p != 0.0f)
            {
                const double factor = (1.0 + k.mod.sign * m / 100.0) * (1.0 + k.power.sign * p / 100.0);
                add(Fmt("%+.0f%% ", (factor - 1.0) * 100.0) + k.mod.effect);
            }
        }
        else
        {
            if (p != 0.0f)
                add(Fmt("%+.0f%% ", k.power.sign * p) + k.power.effect);
            if (m != 0.0f)
                add(Fmt("%+.0f%% ", k.mod.sign * m) + k.mod.effect);
        }

        if (m != 0.0f)
            row.note += std::string(k.mod.effect) + ": enchantments and perks " + Fmt("%+.0f", m);
        if (p != 0.0f)
            row.note += (row.note.empty() ? "" : "\n") + std::string(k.power.effect) + ": potions " + Fmt("%+.0f", p);

        row.detail = OwnedPerks(actor, k.value);
        s.rows.push_back(std::move(row));
    };

    {
        SheetSection s{"Warrior", {}, {}};
        skill(s, {"One-Handed",
                  AV::kOneHanded,
                  {AV::kOneHandedModifier, "damage", +1},
                  {AV::kOneHandedPowerModifier, "damage", +1}});
        skill(s, {"Two-Handed",
                  AV::kTwoHanded,
                  {AV::kTwoHandedModifier, "damage", +1},
                  {AV::kTwoHandedPowerModifier, "damage", +1}});
        skill(s, {"Block", AV::kBlock, {AV::kBlockModifier, "blocked", +1}, {AV::kBlockPowerModifier, "blocked", +1}});
        skill(s, {"Smithing",
                  AV::kSmithing,
                  {AV::kSmithingModifier, "tempering", +1},
                  {AV::kSmithingPowerModifier, "tempering", +1}});
        skill(s, {"Heavy Armor",
                  AV::kHeavyArmor,
                  {AV::kHeavyArmorModifier, "damage", -1},
                  {AV::kHeavyArmorPowerModifier, "damage", -1}});
        skill(s, {"Light Armor",
                  AV::kLightArmor,
                  {AV::kLightArmorModifier, "damage", -1},
                  {AV::kLightArmorPowerModifier, "damage", -1}});
        out.push_back(std::move(s));
    }

    {
        SheetSection s{"Thief", {}, {}};
        skill(s, {"Archery",
                  AV::kArchery,
                  {AV::kMarksmanModifier, "damage", +1},
                  {AV::kMarksmanPowerModifier, "damage", +1}});
        skill(s, {"Pickpocket",
                  AV::kPickpocket,
                  {AV::kPickpocketModifier, "chance", +1},
                  {AV::kPickpocketPowerModifier, "chance", +1}});
        skill(s, {"Lockpicking",
                  AV::kLockpicking,
                  {AV::kLockpickingModifier, "sweet spot", +1},
                  {AV::kLockpickingPowerModifier, "sweet spot", +1}});
        skill(
            s,
            {"Sneak", AV::kSneak, {AV::kSneakingModifier, "stealth", +1}, {AV::kSneakingPowerModifier, "stealth", +1}});
        skill(s, {"Alchemy",
                  AV::kAlchemy,
                  {AV::kAlchemyModifier, "potion strength", +1},
                  {AV::kAlchemyPowerModifier, "potion strength", +1}});
        // Sell prices up and buy prices down by the same factor: "better prices".
        skill(s, {"Speech",
                  AV::kSpeech,
                  {AV::kSpeechcraftModifier, "better prices", +1},
                  {AV::kSpeechcraftPowerModifier, "better prices", +1}});
        out.push_back(std::move(s));
    }

    {
        SheetSection s{"Magic", {}, {}};
        skill(s, {"Alteration",
                  AV::kAlteration,
                  {AV::kAlterationModifier, "cost", -1},
                  {AV::kAlterationPowerModifier, "duration", +1}});
        skill(s, {"Conjuration",
                  AV::kConjuration,
                  {AV::kConjurationModifier, "cost", -1},
                  {AV::kConjurationPowerModifier, "duration", +1}});
        skill(s, {"Destruction",
                  AV::kDestruction,
                  {AV::kDestructionModifier, "cost", -1},
                  {AV::kDestructionPowerModifier, "damage", +1}});
        skill(s, {"Illusion",
                  AV::kIllusion,
                  {AV::kIllusionModifier, "cost", -1},
                  {AV::kIllusionPowerModifier, "magnitude", +1}});
        skill(s, {"Restoration",
                  AV::kRestoration,
                  {AV::kRestorationModifier, "cost", -1},
                  {AV::kRestorationPowerModifier, "healing", +1}});
        skill(s, {"Enchanting", AV::kEnchanting, none, none});
        out.push_back(std::move(s));
    }

    return out;
}

RE::SpellItem *FindSpell(std::uint32_t form)
{
    if (form == 0)
        return nullptr;
    return RE::TESForm::LookupByID<RE::SpellItem>(form);
}

} // namespace ft::game
