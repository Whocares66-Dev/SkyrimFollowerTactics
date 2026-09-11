#include "game/Sheet.h"

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

SheetRow EquippedRow(bool pinned)
{
    SheetRow row;
    row.label = "Equipped";
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
        return "Petty";
    case RE::SOUL_LEVEL::kLesser:
        return "Lesser";
    case RE::SOUL_LEVEL::kCommon:
        return "Common";
    case RE::SOUL_LEVEL::kGreater:
        return "Greater";
    case RE::SOUL_LEVEL::kGrand:
        return "Grand";
    default:
        return "Empty";
    }
}

} // namespace ft::game
