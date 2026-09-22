#include "progression/game/Tomes.h"

#include "progression/game/Forms.h"
#include "progression/game/Log.h"
#include "progression/game/SpellView.h"

#include <algorithm>
#include <unordered_set>

namespace fp::game
{

SpellFacts FactsOf(RE::SpellItem *spell, RE::Actor *caster)
{
    SpellFacts facts;
    if (!spell)
        return facts;
    if (auto key = KeyOf(spell))
        facts.spell = std::move(*key);
    facts.name = NameOf(spell);
    facts.ordinary = spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell;
    // The school is the costliest effect's skill: the spell's own level is
    // that effect's minimum skill, as the game's Novice-Master labels are.
    if (const auto *effect = spell->GetCostliestEffectItem(); effect && effect->baseEffect)
    {
        facts.school = SkillFromActorValue(static_cast<int>(effect->baseEffect->GetMagickSkill()));
        facts.minimumSkill = effect->baseEffect->GetMinimumSkillLevel();
    }
    facts.cost = static_cast<int>(spell->CalculateMagickaCost(caster));
    return facts;
}

std::vector<Tome> TomesCarried(RE::Actor *forActor)
{
    std::vector<Tome> out;
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!player)
        return out;
    const auto inventory =
        player->GetInventory([](RE::TESBoundObject &object) { return object.Is(RE::FormType::Book); });
    for (const auto &[object, entry] : inventory)
    {
        auto *book = object ? object->As<RE::TESObjectBOOK>() : nullptr;
        const int count = entry.first;
        if (!book || count <= 0 || !book->TeachesSpell())
            continue;
        RE::SpellItem *spell = book->GetSpell();
        if (!spell)
            continue;
        SpellFacts facts = FactsOf(spell, forActor);
        if (!facts.ordinary || !facts.school || facts.spell.Empty())
            continue;
        const auto bookKey = KeyOf(book);
        if (!bookKey)
            continue;
        const auto same =
            std::find_if(out.begin(), out.end(), [&](const Tome &t) { return t.facts.spell == facts.spell; });
        if (same != out.end())
        {
            same->count += count;
            continue;
        }
        out.push_back({*bookKey, NameOf(book), std::move(facts), count});
    }
    std::sort(out.begin(), out.end(), [](const Tome &a, const Tome &b) { return a.facts.name < b.facts.name; });
    return out;
}

std::vector<KnownSpell> KnownSpells(RE::Actor *actor, std::span<RE::SpellItem *const> taught)
{
    std::vector<KnownSpell> out;
    if (!actor)
        return out;
    std::unordered_set<RE::SpellItem *> seen;
    const auto consider = [&](RE::SpellItem *spell, bool onRecord) {
        if (!spell || !seen.insert(spell).second)
            return;
        if (spell->GetSpellType() != RE::MagicSystem::SpellType::kSpell)
            return;
        out.push_back({FactsOf(spell, actor), onRecord});
    };
    if (auto *npc = actor->GetActorBase())
        if (auto *list = npc->GetSpellList(); list && list->spells)
            for (std::uint32_t i = 0; i < list->numSpells; ++i)
                consider(list->spells[i], true);
    for (auto *spell : actor->GetActorRuntimeData().addedSpells)
        consider(spell, false);
    for (auto *spell : taught)
        consider(spell, false);
    std::sort(out.begin(), out.end(),
              [](const KnownSpell &a, const KnownSpell &b) { return a.facts.name < b.facts.name; });
    return out;
}

bool SpellOnRecord(RE::Actor *actor, const RE::SpellItem *spell)
{
    auto *npc = actor ? actor->GetActorBase() : nullptr;
    auto *list = npc ? npc->GetSpellList() : nullptr;
    return spell && list && list->spells &&
           std::find(list->spells, list->spells + list->numSpells, spell) != list->spells + list->numSpells;
}

bool TakeTome(RE::TESObjectBOOK *tome)
{
    auto *player = RE::PlayerCharacter::GetSingleton();
    if (!tome || !player)
        return false;
    player->RemoveItem(tome, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
    return true;
}

bool AddToActor(RE::Actor *actor, RE::SpellItem *spell)
{
    if (!actor || !spell)
        return false;
    actor->AddSpell(spell);
    return actor->HasSpell(spell);
}

bool ForgetSpell(RE::Actor *actor, RE::SpellItem *spell)
{
    if (!actor || !spell)
        return false;
    if (!spellview::Installed())
        return actor->RemoveSpell(spell);
    actor->DeselectSpell(spell);
    return true;
}

} // namespace fp::game
