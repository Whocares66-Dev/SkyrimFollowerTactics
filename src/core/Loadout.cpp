#include "core/Loadout.h"

#include <algorithm>

namespace ft
{

Hand HandsFor(Grip grip, Hand requested) noexcept
{
    switch (grip)
    {
    case Grip::LeftOnly:
        return Hand::Left;
    case Grip::RightOnly:
        return Hand::Right;
    case Grip::Both:
        return Hand::Both;
    case Grip::Either:
        return requested == Hand::Left ? Hand::Left : Hand::Right;
    case Grip::None:
    default:
        return Hand::None;
    }
}

bool Pinnable(const Holdable &thing) noexcept
{
    return !thing.unusable;
}

bool Conflicts(const Holdable &incoming, Hand hands, const Holdable &held, Hand heldHands) noexcept
{
    if (hands != Hand::None && heldHands != Hand::None)
        return Overlap(hands, heldHands);
    if (incoming.slots != 0 && held.slots != 0)
        return (incoming.slots & held.slots) != 0;
    return incoming.ammo && held.ammo;
}

bool Competes(Grip grip, Hand pinned) noexcept
{
    switch (grip)
    {
    case Grip::LeftOnly:
        return Overlap(pinned, Hand::Left);
    case Grip::RightOnly:
        return Overlap(pinned, Hand::Right);
    case Grip::Either:
    case Grip::Both:
        return pinned != Hand::None;
    case Grip::None:
    default:
        return false;
    }
}

Hand PinnedHands(const std::vector<Pin> &pins) noexcept
{
    Hand all = Hand::None;
    for (const Pin &pin : pins)
        all = all | pin.hands;
    return all;
}

std::vector<std::uint32_t> KeepFromAI(const std::vector<Pin> &pins, const std::vector<Holdable> &things)
{
    const Hand pinned = PinnedHands(pins);
    std::vector<std::uint32_t> out;
    if (pinned == Hand::None)
        return out;
    const auto isPinned = [&](std::uint32_t form) {
        return std::any_of(pins.begin(), pins.end(), [form](const Pin &pin) { return pin.form == form; });
    };
    for (const Holdable &thing : things)
    {
        if (!isPinned(thing.form) && Competes(thing.grip, pinned))
            out.push_back(thing.form);
    }
    return out;
}

} // namespace ft
