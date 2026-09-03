#include "game/Sensors.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
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

} // namespace

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
    });

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

    ForEachSpell(actor, [&out](RE::SpellItem *spell) {
        if (!IsCastable(spell))
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

} // namespace

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
        SheetSection s{"General", {}};
        auto *race = actor->GetRace();
        s.rows.push_back({"Race", race && race->GetName() ? race->GetName() : "?", {}, {}});
        s.rows.push_back({"Speed", Fmt("%.0f%%", av(RE::ActorValue::kSpeedMult)), {}, {}});
        s.rows.push_back({"Noise", Fmt("%.0f%%", av(RE::ActorValue::kMovementNoiseMult) * 100.0), {}, {}});
        out.push_back(std::move(s));
    }

    {
        SheetSection s{"Defence", {}};
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

        s.rows.push_back({"Armor", Fmt("%.0f", armor), {}, {}});
        s.rows.push_back({"Resist Damage", CappedPercent(armorPct, GameSetting("fMaxArmorRating", 80.0f)), {}, {}});
        s.rows.push_back({"Health Rate", Fmt("%.2f%%", av(RE::ActorValue::kHealRate)), {}, {}});
        s.rows.push_back({"Stamina Rate", Fmt("%.2f%%", av(RE::ActorValue::kStaminaRate)), {}, {}});
        s.rows.push_back({"Magicka Rate", Fmt("%.2f%%", av(RE::ActorValue::kMagickaRate)), {}, {}});
        s.rows.push_back({"Resist Disease", Fmt("%.0f%%", av(RE::ActorValue::kResistDisease)), {}, {}});
        s.rows.push_back({"Resist Poison", CappedPercent(av(RE::ActorValue::kPoisonResist), resistCap), {}, {}});
        s.rows.push_back({"Resist Fire", CappedPercent(av(RE::ActorValue::kResistFire), resistCap), {}, {}});
        s.rows.push_back({"Resist Shock", CappedPercent(av(RE::ActorValue::kResistShock), resistCap), {}, {}});
        s.rows.push_back({"Resist Frost", CappedPercent(av(RE::ActorValue::kResistFrost), resistCap), {}, {}});
        s.rows.push_back({"Resist Magic", CappedPercent(av(RE::ActorValue::kResistMagic), resistCap), {}, {}});
        out.push_back(std::move(s));
    }

    {
        SheetSection s{"Attack", {}};
        // The right hand's weapon, as authored: base damage, before skill,
        // perks and enchantments. The number the inventory shows is computed
        // by a routine the engine does not expose, so this is the honest
        // figure rather than an approximation of that one.
        auto *right = actor->GetEquippedObject(false);
        auto *weapon = right ? right->As<RE::TESObjectWEAP>() : nullptr;
        if (weapon)
        {
            s.rows.push_back({"Weapon", weapon->GetName() ? weapon->GetName() : "?", {}, {}});
            s.rows.push_back({"Base Damage", Fmt("%.0f", weapon->GetAttackDamage()), {}, {}});
            s.rows.push_back({"Weapon Speed", Fmt("%.2f", weapon->GetSpeed()), {}, {}});
            s.rows.push_back({"Reach", Fmt("%.2f", weapon->GetReach()), {}, {}});
            s.rows.push_back({"Stagger", Fmt("%.2f", weapon->GetStagger()), {}, {}});
        }
        else
        {
            s.rows.push_back({"Weapon", "unarmed", {}, {}});
            s.rows.push_back({"Base Damage", Fmt("%.0f", av(RE::ActorValue::kUnarmedDamage)), {}, {}});
        }
        if (auto *ammo = actor->GetCurrentAmmo())
            s.rows.push_back({"Arrow Damage", Fmt("%.0f", ammo->GetRuntimeData().data.damage), {}, {}});
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
        SheetRow row{k.label, Fmt("%.0f", av(k.value)), {}, {}};
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

        s.rows.push_back(std::move(row));
    };

    {
        SheetSection s{"Warrior", {}};
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
        SheetSection s{"Thief", {}};
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
        SheetSection s{"Magic", {}};
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
