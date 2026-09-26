#include "CombatStyle.h"

#include <algorithm>
#include <cmath>

namespace ft
{
namespace
{

struct FieldInfo
{
    std::string_view wire;
    bool scored; // 0 to 10 by tenths; otherwise 0 to 1 by hundredths
};

constexpr std::array<FieldInfo, kStyleFields> kFields{{
    {"offensive", false},
    {"defensive", false},
    {"melee-score", true},
    {"magic-score", true},
    {"ranged-score", true},
    {"staff-score", true},
    {"shout-score", true},
    {"unarmed-score", true},
    {"attack-staggered", true},
    {"power-attack-staggered", true},
    {"power-attack-blocking", true},
    {"bash", true},
    {"bash-recoiled", true},
    {"bash-attacking", true},
    {"bash-power-attacking", true},
    {"circle", false},
    {"fallback", false},
    {"flank-distance", false},
    {"stalk-time", false},
    {"strafe", false},
}};

const FieldInfo &Info(StyleField field)
{
    return kFields[static_cast<std::size_t>(field)];
}

float StepsPerUnit(StyleField field)
{
    return Info(field).scored ? 10.0f : 100.0f;
}

} // namespace

float StyleMax(StyleField field)
{
    return Info(field).scored ? 10.0f : 1.0f;
}

float StyleStep(StyleField field)
{
    return 1.0f / StepsPerUnit(field);
}

std::string_view WireName(StyleField field)
{
    return Info(field).wire;
}

std::optional<StyleField> StyleFieldFromWireName(std::string_view name)
{
    for (std::size_t i = 0; i < kStyleFields; ++i)
        if (kFields[i].wire == name)
            return static_cast<StyleField>(i);
    return std::nullopt;
}

float SnapStyleDelta(StyleField field, float delta)
{
    // In whole steps, divided out rather than multiplied by the step, so a
    // slider's 0.30000001 is the float nearest 0.3 and a round trip through
    // the file comes back equal.
    const float perUnit = StepsPerUnit(field);
    const float steps = std::round(delta * perUnit);
    return steps == 0.0f ? 0.0f : steps / perUnit;
}

float AdjustedStyleValue(StyleField field, float base, float delta)
{
    const float top = (std::max)(StyleMax(field), base);
    return std::clamp(base + delta, 0.0f, top);
}

std::pair<float, float> StyleDeltaRange(StyleField field, float base)
{
    const float top = (std::max)(StyleMax(field), base);
    return {-(std::max)(base, 0.0f), top - base};
}

bool AnyStyleDelta(const StyleValues &deltas)
{
    return std::any_of(deltas.begin(), deltas.end(), [](float d) { return d != 0.0f; });
}

std::string_view WireName(StyleSwitch which)
{
    return which == StyleSwitch::Flanking ? "flanking" : "dual-wield";
}

std::optional<StyleSwitch> StyleSwitchFromWireName(std::string_view name)
{
    for (std::size_t i = 0; i < kStyleSwitches; ++i)
        if (WireName(static_cast<StyleSwitch>(i)) == name)
            return static_cast<StyleSwitch>(i);
    return std::nullopt;
}

bool AnyStyleAdjustment(const StyleAdjustments &adjustments)
{
    return AnyStyleDelta(adjustments.deltas) ||
           std::any_of(adjustments.switches.begin(), adjustments.switches.end(),
                       [](const std::optional<bool> &word) { return word.has_value(); });
}

bool SwitchOn(const StyleAdjustments &adjustments, StyleSwitch which, bool record)
{
    return adjustments.switches[static_cast<std::size_t>(which)].value_or(record);
}

std::optional<bool> ToggledSwitch(const StyleAdjustments &adjustments, StyleSwitch which, bool record)
{
    const bool next = !SwitchOn(adjustments, which, record);
    return next == record ? std::nullopt : std::optional<bool>(next);
}

} // namespace ft
