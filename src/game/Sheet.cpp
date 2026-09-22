#include "game/Sheet.h"

#include "core/I18n.h"

#include <cstdio>

namespace ft::game
{

std::string Fmt(const char *fmt, double value)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), fmt, value);
    return buf;
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

std::string NameOr(RE::TESObjectREFR *ref, const char *fallback)
{
    const char *name = ref ? ref->GetDisplayFullName() : nullptr;
    return name && *name ? name : fallback;
}

SheetRow EquippedRow(bool pinned)
{
    SheetRow row;
    row.label = ft::i18n::Tr("Equipped");
    row.icon = kGlyphTick;
    if (pinned)
        row.icon2 = kGlyphPin;
    row.equipped = true;
    return row;
}

const char *SoulName(RE::SOUL_LEVEL level)
{
    switch (level)
    {
    case RE::SOUL_LEVEL::kPetty:
        return ft::i18n::Tr("Petty");
    case RE::SOUL_LEVEL::kLesser:
        return ft::i18n::Tr("Lesser");
    case RE::SOUL_LEVEL::kCommon:
        return ft::i18n::Tr("Common");
    case RE::SOUL_LEVEL::kGreater:
        return ft::i18n::Tr("Greater");
    case RE::SOUL_LEVEL::kGrand:
        return ft::i18n::Tr("Grand");
    default:
        return ft::i18n::Tr("Empty");
    }
}

} // namespace ft::game
