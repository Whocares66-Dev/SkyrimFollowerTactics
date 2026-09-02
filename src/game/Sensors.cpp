#include "game/Sensors.h"

#include <algorithm>

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

    ScanPotions(actor, s.potions, choice);

    s.potions.healthEffectActive = RestoreEffectRunning(actor, RE::ActorValue::kHealth);
    s.potions.magickaEffectActive = RestoreEffectRunning(actor, RE::ActorValue::kMagicka);
    s.potions.staminaEffectActive = RestoreEffectRunning(actor, RE::ActorValue::kStamina);

    // Spells: what she knows, what is running, what is in hand. All three are
    // ids only -- Snapshot never sees an RE:: type -- and all three are needed
    // to tell "cannot", "already up" and "already held" apart in the status
    // column.
    ForEachSpell(actor, [&s](RE::SpellItem *spell) {
        if (IsCastable(spell))
            s.spells.known.push_back(spell->GetFormID());
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

RE::SpellItem *FindSpell(std::uint32_t form)
{
    if (form == 0)
        return nullptr;
    return RE::TESForm::LookupByID<RE::SpellItem>(form);
}

} // namespace ft::game
