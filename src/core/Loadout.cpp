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

bool KeptFromAI(const std::vector<Pin> &pins, const Holdable &thing, Hand slot) noexcept
{
    const Hand pinned = PinnedHands(pins);
    if (pinned == Hand::None)
        return false;
    const auto pin = std::find_if(pins.begin(), pins.end(), [&](const Pin &p) { return p.form == thing.form; });
    if (slot == Hand::None)
        return pin == pins.end() && Competes(thing.grip, pinned);
    if (pin != pins.end())
        return !Overlap(slot, pin->hands);
    return Overlap(slot, pinned);
}

bool SetAside(const std::vector<Pin> &pins, const Holdable &thing) noexcept
{
    switch (thing.grip)
    {
    case Grip::LeftOnly:
        return KeptFromAI(pins, thing, Hand::Left);
    case Grip::RightOnly:
        return KeptFromAI(pins, thing, Hand::Right);
    case Grip::Both:
        return KeptFromAI(pins, thing, Hand::Both);
    case Grip::Either:
        return KeptFromAI(pins, thing, Hand::Left) && KeptFromAI(pins, thing, Hand::Right);
    case Grip::None:
    default:
        return false;
    }
}

} // namespace ft
