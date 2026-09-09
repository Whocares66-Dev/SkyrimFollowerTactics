#include "game/Forms.h"

namespace ft::game
{
namespace
{

// Move a fresh form from the engine's dynamic ID to ours. The constructor
// registered it under the dynamic one; SetFormID takes it out of the map and
// puts it back under the new ID.
bool Place(RE::TESForm *form, std::uint32_t localID, const char *what)
{
    const RE::FormID id = kRuntimeFormBase | localID;
    if (auto *taken = RE::TESForm::LookupByID(id))
    {
        logger::error("forms: {:08X} is already taken by a {} -- {} not made", id,
                      static_cast<int>(taken->GetFormType()), what);
        return false;
    }
    const auto born = form->GetFormID();
    form->SetFormID(id, /*updateFile*/ false);
    const bool ok = form->GetFormID() == id && RE::TESForm::LookupByID(id) == form;
    logger::info("forms: {} {:08X} (born {:08X}){}", what, form->GetFormID(), born, ok ? "" : " -- NOT registered");
    return ok;
}

} // namespace

RE::TESPackage *ClonePackage(RE::TESPackage *source, std::uint32_t localID)
{
    if (!source || !source->data)
        return nullptr;

    // kPackage is the type of an instance made from a template, and is what
    // the constructor sets anyway; the call is for the allocation.
    // TESPackage::CreatePackage(type): allocate a package and give it the
    // data object its type needs. Its AE ID was checked against 1.6.1170 by
    // reading the function (docs/MAGIC.md "Forms at runtime").
    auto *pkg = RE::TESPackage::CreatePackage(RE::PACKAGE_PROCEDURE_TYPE::kPackage);
    if (!pkg)
    {
        logger::error("forms: CreatePackage returned nothing");
        return nullptr;
    }
    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    auto *srcCustom = skyrim_cast<RE::TESCustomPackageData *>(source->data);
    if (!custom || !srcCustom)
    {
        logger::error("forms: package data is not TESCustomPackageData (ours {}, source {})",
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
        logger::error("forms: copy of {:08X} incomplete: {} of {} inputs, template {} vs {}, name map {}",
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

    if (!Place(pkg, localID, "package"))
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

RE::TESWordOfPower *CreateWord(std::uint32_t localID, const char *name)
{
    auto *factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESWordOfPower>();
    auto *word = factory ? factory->Create() : nullptr;
    if (!word)
    {
        logger::error("forms: no factory or no word of power");
        return nullptr;
    }
    word->fullName = name;
    word->translation = "power";
    return Place(word, localID, "word") ? word : nullptr;
}

RE::TESShout *CreateShout(std::uint32_t localID, RE::TESWordOfPower *word, RE::TESForm *spell, const char *name)
{
    auto *factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::TESShout>();
    auto *shout = factory ? factory->Create() : nullptr;
    if (!shout)
    {
        logger::error("forms: no factory or no shout");
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
    return Place(shout, localID, "shout") ? shout : nullptr;
}

} // namespace ft::game
