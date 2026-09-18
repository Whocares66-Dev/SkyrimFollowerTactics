#include "game/Forms.h"

#include "game/Log.h"

#include <algorithm>
#include <vector>

namespace ft::game
{
namespace
{

// TESPackage::CreatePackage(type): allocate a package and give it the data
// object its type needs. The type is the record's PKDT type (PACKAGE_TYPE:
// kPackage is 18, an instance made from a template, what the constructor
// sets anyway). The library declares the same function with
// PACKAGE_PROCEDURE_TYPE, whose kPackage is 46, and given 46 the engine
// returns a package with no data at all (found 2026-09-09: every cast
// rule "unsupported" since the migration). The declaration is the
// library's bug; this one takes the type the engine takes, so the call
// is made with the enum and no cast. The AE ID was checked against
// 1.6.1170 by reading the function (dev/MAGIC.md "Forms at runtime").
RE::TESPackage *CreatePackage(RE::PACKAGE_TYPE type)
{
    using func_t = RE::TESPackage *(*)(RE::PACKAGE_TYPE);
    static REL::Relocation<func_t> func{RELOCATION_ID(28732, 29496)};
    return func(type);
}

// Every ID Place has given a form, ascending, since the walk only counts up.
std::vector<std::uint32_t> g_made;

// Move a fresh form from the engine's dynamic ID to the next free one of
// ours. The constructor registered it under the dynamic one; SetFormID takes
// it out of the map and puts it back under the new ID. Our forms are never
// deleted, so an ID the walk has passed stays passed. Game thread only.
bool Place(RE::TESForm *form, const char *what)
{
    static RE::FormID next = kFirstFormId;
    auto *save = RE::BGSSaveLoadGame::GetSingleton();
    for (; next <= kLastFormId; ++next)
    {
        auto *taken = RE::TESForm::LookupByID(next);
        if (!taken && !(save && save->IsFormIDInUse(next)))
            break;
        log::forms.debug("{:08X} is taken ({}) -- skipped", next,
                         taken ? fmt::format("a form of type {}", static_cast<int>(taken->GetFormType()))
                               : std::string("by the loaded save"));
    }
    if (next > kLastFormId)
    {
        log::forms.error("no free id left above {:08X} -- {} not made", kFirstFormId, what);
        return false;
    }
    const RE::FormID id = next++;
    const auto born = form->GetFormID();
    form->SetFormID(id, /*updateFile*/ false);
    const bool ok = form->GetFormID() == id && RE::TESForm::LookupByID(id) == form;
    if (!ok)
    {
        log::forms.error("{} {:08X} (born {:08X}) -- NOT registered", what, form->GetFormID(), born);
        return false;
    }
    g_made.push_back(id);
    log::forms.debug("{} {:08X} (born {:08X})", what, form->GetFormID(), born);
    return ok;
}

} // namespace

bool MadeByUs(std::uint32_t formId)
{
    return std::binary_search(g_made.begin(), g_made.end(), formId);
}

RE::TESPackage *ClonePackage(RE::TESPackage *source)
{
    if (!source || !source->data)
        return nullptr;

    auto *pkg = CreatePackage(RE::PACKAGE_TYPE::kPackage);
    if (!pkg)
    {
        log::forms.error("CreatePackage returned nothing");
        return nullptr;
    }
    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    auto *srcCustom = skyrim_cast<RE::TESCustomPackageData *>(source->data);
    if (!custom || !srcCustom)
    {
        log::forms.error("package data of {:08X} is not TESCustomPackageData (ours {}, source {})", source->GetFormID(),
                         static_cast<const void *>(custom), static_cast<const void *>(srcCustom));
        return nullptr;
    }

    // The engine's own copy: every input recreated through the type registry
    // and assigned from the source; the template link, its name map and a
    // copy of its procedure tree come along.
    pkg->data->Copy(source->data, pkg);

    if (custom->data.dataSize != srcCustom->data.dataSize || custom->templateParent != srcCustom->templateParent ||
        !custom->nameMap)
    {
        log::forms.error("copy of {:08X} incomplete: {} of {} inputs, template {} vs {}, name map {}",
                         source->GetFormID(), custom->data.dataSize, srcCustom->data.dataSize,
                         static_cast<const void *>(custom->templateParent),
                         static_cast<const void *>(srcCustom->templateParent), custom->nameMap ? "present" : "ABSENT");
        return nullptr;
    }

    // What the ESP-era records carried in PKDT, byte for byte: IgnoreCombat,
    // no interrupt override (0 in the file, whatever the header names it),
    // Run, no interrupt flags. Mercer's own record says Combat override; the
    // proven records said none, and proven wins.
    pkg->packData.packFlags = RE::PACKAGE_DATA::GeneralFlag::kIgnoreCombat;
    pkg->packData.interruptOverrideType = static_cast<RE::PACK_INTERRUPT_TARGET>(0);
    pkg->packData.maxSpeed = RE::PACKAGE_DATA::PreferredSpeed::kRun;
    pkg->packData.foBehaviorFlags = RE::PACKAGE_DATA::InterruptFlag::kNone;
    pkg->packData.packageSpecificFlags = 0;
    pkg->combatStyle = nullptr;
    pkg->ownerQuest = nullptr;

    if (!Place(pkg, "package"))
        return nullptr;
    return pkg;
}

RE::TESConditionItem *AddIsReferenceCondition(RE::TESPackage *pkg)
{
    if (!pkg)
        return nullptr;
    // TES_HEAP_REDEFINE_NEW: allocated where the engine would allocate it.
    auto *item = new RE::TESConditionItem();
    item->next = nullptr;
    item->data.functionData.function = RE::FUNCTION_DATA::FunctionID::kGetIsReference;
    item->data.functionData.params[0] = nullptr;
    item->data.functionData.params[1] = nullptr;
    item->data.comparisonValue.f = 1.0f;
    item->data.flags.opCode = RE::CONDITION_ITEM_DATA::OpCode::kEqualTo;
    item->data.object = RE::CONDITIONITEMOBJECT::kSelf;
    pkg->packConditions.head = item;
    return item;
}

RE::TESWordOfPower *CreateWord(const char *name)
{
    auto *factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESWordOfPower>();
    auto *word = factory ? factory->Create() : nullptr;
    if (!word)
    {
        log::forms.error("no factory or no word of power");
        return nullptr;
    }
    word->fullName = name;
    word->translation = "power";
    return Place(word, "word") ? word : nullptr;
}

RE::TESShout *CreateShout(RE::TESWordOfPower *word, RE::TESForm *spell, const char *name)
{
    auto *factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESShout>();
    auto *shout = factory ? factory->Create() : nullptr;
    if (!shout)
    {
        log::forms.error("no factory or no shout");
        return nullptr;
    }
    shout->fullName = name;
    shout->variations[0].word = word;
    shout->variations[0].spell = spell ? spell->As<RE::SpellItem>() : nullptr;
    shout->variations[0].recoveryTime = 1.0f;
    for (std::size_t w = 1; w < RE::TESShout::VariationIDs::kTotal; ++w)
    {
        shout->variations[w].word = nullptr;
        shout->variations[w].spell = nullptr;
        shout->variations[w].recoveryTime = 0.0f;
    }
    return Place(shout, "shout") ? shout : nullptr;
}

} // namespace ft::game
