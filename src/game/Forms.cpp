#include "game/Forms.h"

#include "game/Log.h"

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
// 1.6.1170 by reading the function (docs/MAGIC.md "Forms at runtime").
RE::TESPackage *CreatePackage(RE::PACKAGE_TYPE type)
{
    using func_t = RE::TESPackage *(*)(RE::PACKAGE_TYPE);
    static REL::Relocation<func_t> func{RELOCATION_ID(28732, 29496)};
    return func(type);
}

// Move a fresh form from the engine's dynamic ID to ours. The constructor
// registered it under the dynamic one; SetFormID takes it out of the map and
// puts it back under the new ID.
bool Place(RE::TESForm *form, std::uint32_t localID, const char *what)
{
    const RE::FormID id = kRuntimeFormBase | localID;
    if (auto *taken = RE::TESForm::LookupByID(id))
    {
        log::forms.event(log::Level::Error, "form.error",
                         {{"requestedFormId", log::Id(id)},
                          {"kind", what},
                          {"reason", "id already taken"},
                          {"takenByFormType", static_cast<int>(taken->GetFormType())}},
                         "{:08X} is already taken by a {} -- {} not made", id,
                         static_cast<int>(taken->GetFormType()), what);
        return false;
    }
    const auto born = form->GetFormID();
    form->SetFormID(id, /*updateFile*/ false);
    const bool ok = form->GetFormID() == id && RE::TESForm::LookupByID(id) == form;
    if (!ok)
    {
        log::forms.event(log::Level::Error, "form.error",
                         {{"requestedFormId", log::Id(id)}, {"kind", what}, {"reason", "not registered after SetFormID"}},
                         "{} {:08X} (born {:08X}) -- NOT registered", what, form->GetFormID(), born);
        return false;
    }
    log::forms.debug("{} {:08X} (born {:08X})", what, form->GetFormID(), born);
    return ok;
}

} // namespace

RE::TESPackage *ClonePackage(RE::TESPackage *source, std::uint32_t localID)
{
    if (!source || !source->data)
        return nullptr;

    auto *pkg = CreatePackage(RE::PACKAGE_TYPE::kPackage);
    if (!pkg)
    {
        log::forms.event(log::Level::Error, "form.error",
                         {{"kind", "package"}, {"reason", "CreatePackage returned nothing"}},
                         "CreatePackage returned nothing");
        return nullptr;
    }
    auto *custom = skyrim_cast<RE::TESCustomPackageData *>(pkg->data);
    auto *srcCustom = skyrim_cast<RE::TESCustomPackageData *>(source->data);
    if (!custom || !srcCustom)
    {
        log::forms.event(log::Level::Error, "form.error",
                         {{"requestedFormId", log::Id(source->GetFormID())},
                          {"kind", "package"},
                          {"reason", "package data is not TESCustomPackageData"}},
                         "package data is not TESCustomPackageData (ours {}, source {})",
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
        log::forms.event(log::Level::Error, "form.error",
                         {{"requestedFormId", log::Id(source->GetFormID())},
                          {"kind", "package"},
                          {"reason", "copy incomplete"},
                          {"inputs", custom->data.dataSize},
                          {"sourceInputs", srcCustom->data.dataSize},
                          {"nameMap", custom->nameMap != nullptr}},
                         "copy of {:08X} incomplete: {} of {} inputs, template {} vs {}, name map {}",
                         source->GetFormID(), custom->data.dataSize, srcCustom->data.dataSize,
                         static_cast<const void *>(custom->templateParent),
                         static_cast<const void *>(srcCustom->templateParent),
                         custom->nameMap ? "present" : "ABSENT");
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
        log::forms.event(log::Level::Error, "form.error", {{"kind", "word"}, {"reason", "no factory or no word of power"}},
                         "no factory or no word of power");
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
        log::forms.event(log::Level::Error, "form.error", {{"kind", "shout"}, {"reason", "no factory or no shout"}},
                         "no factory or no shout");
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
