#include "progression/game/Forms.h"

namespace fp::game
{

// The plugin is the one whose load-order slot the id carries, which is the
// plugin that defines the form -- not GetFile(0), the first plugin to touch
// it, which for a form injected into another plugin's range names the
// injector and does not round-trip through LookupForm.
std::optional<FormKey> KeyOf(const RE::TESForm *form)
{
    auto *data = RE::TESDataHandler::GetSingleton();
    if (!form || !data || form->IsDynamicForm())
        return std::nullopt;
    const RE::FormID id = form->GetFormID();
    const RE::TESFile *file = nullptr;
    std::uint32_t local = 0;
    if ((id >> 24) == 0xFE)
    {
        file = data->LookupLoadedLightModByIndex(static_cast<std::uint16_t>((id >> 12) & 0xFFF));
        local = id & 0xFFF;
    }
    else
    {
        file = data->LookupLoadedModByIndex(static_cast<std::uint8_t>(id >> 24));
        local = id & 0xFFFFFF;
    }
    if (!file)
        return std::nullopt;
    return FormKey{std::string(file->GetFilename()), local};
}

RE::TESForm *Lookup(const FormKey &key)
{
    auto *data = RE::TESDataHandler::GetSingleton();
    if (!data || key.Empty())
        return nullptr;
    return data->LookupForm(key.local, key.plugin);
}

std::string NameOf(const RE::TESForm *form, std::string_view fallback)
{
    if (!form)
        return std::string(fallback);
    const char *name = form->GetName();
    return (name && *name) ? std::string(name) : std::string(fallback);
}

std::string NameOf(RE::Actor *actor, std::string_view fallback)
{
    if (!actor)
        return std::string(fallback);
    if (const char *name = actor->GetDisplayFullName(); name && *name)
        return name;
    return NameOf(actor->GetActorBase(), fallback);
}

} // namespace fp::game
