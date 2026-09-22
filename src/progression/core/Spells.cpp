#include "progression/core/Spells.h"

namespace fp
{

SpellButton LearnButton(std::string_view spell, bool known)
{
    if (known)
        return {false, "Already knows " + std::string(spell)};
    return {true, "Click to learn " + std::string(spell)};
}

SpellButton ForgetButton(std::string_view spell)
{
    return {true, "Click to forget " + std::string(spell)};
}

} // namespace fp
