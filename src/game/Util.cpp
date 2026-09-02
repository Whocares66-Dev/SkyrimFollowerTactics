#include "game/Util.h"

namespace ft::game
{

std::string Describe(RE::Actor *actor)
{
    if (!actor)
        return "<null actor>";

    const char *name = actor->GetDisplayFullName();
    return fmt::format("{} ({:08X})", (name && *name) ? name : "<unnamed>", actor->GetFormID());
}

} // namespace ft::game
