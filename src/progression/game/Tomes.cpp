#include "progression/game/Tomes.h"

#include "progression/game/Forms.h"

namespace fp::game
{

RE::SpellItem *TomeSpell(RE::TESObjectBOOK *book)
{
    if (!book || !book->TeachesSpell())
        return nullptr;
    RE::SpellItem *spell = book->GetSpell();
    return spell && spell->GetSpellType() == RE::MagicSystem::SpellType::kSpell ? spell : nullptr;
}

SpellFacts FactsOf(RE::SpellItem *spell)
{
    SpellFacts facts;
    if (!spell)
        return facts;
    if (auto key = KeyOf(spell))
        facts.spell = std::move(*key);
    facts.name = NameOf(spell);
    return facts;
}

int CarriedCount(RE::Actor *actor, RE::TESObjectBOOK *book)
{
    if (!actor || !book)
        return 0;
    const auto counts = actor->GetInventoryCounts([book](RE::TESBoundObject &object) { return &object == book; });
    const auto it = counts.find(book);
    return it == counts.end() ? 0 : it->second;
}

} // namespace fp::game
