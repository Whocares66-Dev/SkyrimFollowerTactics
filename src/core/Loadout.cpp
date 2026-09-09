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

namespace
{
// A weapon that takes one hand and could take either: a sword, an axe, a
// dagger, a mace. A shield or a torch is LeftOnly and a bow takes both.
bool OneHander(const Holdable &thing) noexcept
{
    return thing.kind == Kind::Weapon && thing.grip == Grip::Either;
}
} // namespace

bool WouldDualWield(const Holdable &thing, const Holdable *inOtherHand) noexcept
{
    if (!inOtherHand || !OneHander(thing) || !OneHander(*inOtherHand))
        return false;
    if (inOtherHand->form == thing.form && thing.count < 2)
        return false; // the only one, moving to the other hand
    return true;
}

bool IsBanned(const Bans &bans, std::uint32_t form) noexcept
{
    return std::find(bans.begin(), bans.end(), form) != bans.end();
}

bool Ban(Bans &bans, std::uint32_t form)
{
    if (form == 0 || IsBanned(bans, form))
        return false;
    bans.push_back(form);
    return true;
}

bool Unban(Bans &bans, std::uint32_t form)
{
    const auto before = bans.size();
    std::erase(bans, form);
    return bans.size() != before;
}

bool Conflicts(const Holdable &incoming, Hand hands, const Holdable &held, Hand heldHands) noexcept
{
    if (hands != Hand::None && heldHands != Hand::None)
        return Overlap(hands, heldHands);
    if (incoming.slots != 0 && held.slots != 0)
        return (incoming.slots & held.slots) != 0;
    return (incoming.IsAmmo() && held.IsAmmo()) || (incoming.IsVoice() && held.IsVoice());
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

const Holdable *FindHoldable(const std::vector<Holdable> &things, std::uint32_t form) noexcept
{
    const auto it = std::find_if(things.begin(), things.end(), [form](const Holdable &t) { return t.form == form; });
    return it == things.end() ? nullptr : &*it;
}

Pin *FindPin(std::vector<Pin> &pins, std::uint32_t form) noexcept
{
    const auto it = std::find_if(pins.begin(), pins.end(), [form](const Pin &p) { return p.thing.form == form; });
    return it == pins.end() ? nullptr : &*it;
}

const Pin *FindPin(const std::vector<Pin> &pins, std::uint32_t form) noexcept
{
    const auto it = std::find_if(pins.begin(), pins.end(), [form](const Pin &p) { return p.thing.form == form; });
    return it == pins.end() ? nullptr : &*it;
}

std::vector<Displaced> MakeRoom(std::vector<Pin> &pins, const Holdable &thing, Hand hands)
{
    std::vector<Displaced> out;
    for (auto it = pins.begin(); it != pins.end();)
    {
        if (it->thing.form == thing.form || !Conflicts(thing, hands, it->thing, it->hands))
        {
            ++it;
            continue;
        }
        const Hand taken = Common(hands, it->hands);
        const bool partly = it->thing.grip == Grip::Either && taken != Hand::None && taken != it->hands;
        if (partly)
        {
            out.push_back({it->thing.form, taken});
            it->hands = Without(it->hands, taken);
            ++it;
            continue;
        }
        out.push_back({it->thing.form, it->hands});
        it = pins.erase(it);
    }
    return out;
}

void AddPin(std::vector<Pin> &pins, const Holdable &thing, Hand hands, bool moving)
{
    if (Pin *pin = FindPin(pins, thing.form))
    {
        pin->hands = thing.grip == Grip::Either && !moving ? pin->hands | hands : hands;
        return;
    }
    pins.push_back({thing, hands});
}

Hand LetGo(std::vector<Pin> &pins, const Holdable &thing, Hand hands)
{
    Pin *pin = FindPin(pins, thing.form);
    if (!pin)
        return hands;
    if (hands == Hand::None)
    {
        const Hand whole = pin->hands;
        pins.erase(pins.begin() + (pin - pins.data()));
        return whole;
    }
    pin->hands = Without(pin->hands, hands);
    if (pin->hands == Hand::None)
        pins.erase(pins.begin() + (pin - pins.data()));
    return hands;
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
    const Pin *pin = FindPin(pins, thing.form);
    // The voice: the AI's shout entries carry no hand, and one is kept from
    // it while another power or shout is pinned there.
    if (thing.IsVoice())
        return pin == nullptr && std::any_of(pins.begin(), pins.end(), [](const Pin &p) { return p.thing.IsVoice(); });
    const Hand pinned = PinnedHands(pins);
    if (pinned == Hand::None)
        return false;
    if (slot == Hand::None)
        return pin == nullptr && Competes(thing.grip, pinned);
    if (pin != nullptr)
    {
        // Its own hand is the pin. Another hand is off when a pin holds it,
        // and also when this is her only one: no second copy for that hand.
        if (Overlap(slot, pin->hands))
            return false;
        return thing.count < 2 || Overlap(slot, pinned);
    }
    return Overlap(slot, pinned);
}

// A pin with no hand holding the place a thing would take: a body slot
// they share, the quiver, or the voice.
bool HoldsPlaceOf(const Pin &pin, const Holdable &thing) noexcept
{
    if (pin.thing.form == thing.form)
        return false;
    return (pin.thing.slots & thing.slots) != 0 || (pin.thing.IsAmmo() && thing.IsAmmo()) ||
           (pin.thing.IsVoice() && thing.IsVoice());
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
        return std::any_of(pins.begin(), pins.end(), [&](const Pin &pin) { return HoldsPlaceOf(pin, thing); });
    }
}

std::vector<Pin> Shadowing(const std::vector<Pin> &pins, const Holdable &thing)
{
    std::vector<Pin> out;
    if (!SetAside(pins, thing))
        return out;
    const Hand reach = Reach(thing.grip);
    for (const Pin &pin : pins)
    {
        if (pin.thing.form == thing.form)
            continue;
        if (const Hand taken = Common(pin.hands, reach); taken != Hand::None)
            out.push_back({pin.thing, taken});
        else if (HoldsPlaceOf(pin, thing))
            out.push_back(pin);
    }
    return out;
}

namespace
{
bool SamePin(const Pin &a, const Pin &b) noexcept
{
    return a.thing.form == b.thing.form && a.hands == b.hands;
}

bool Holds(const std::vector<Pin> &pins, const Pin &pin) noexcept
{
    return std::any_of(pins.begin(), pins.end(), [&](const Pin &p) { return SamePin(p, pin); });
}
} // namespace

bool PutBackNow(const Pin &pin, bool on, bool fighting, bool castInProgress) noexcept
{
    if (on)
        return false;
    if (pin.hands == Hand::None)
        return true;
    return !(fighting && castInProgress);
}

AfterFight SettleAfterFight(const std::vector<Pin> &now, const std::vector<Pin> &before)
{
    AfterFight out;
    for (const Pin &pin : now)
    {
        if (Holds(before, pin))
            continue;
        const bool displaced = std::any_of(before.begin(), before.end(), [&](const Pin &saved) {
            return saved.thing.form != pin.thing.form && Conflicts(pin.thing, pin.hands, saved.thing, saved.hands);
        });
        out.released.push_back({pin.thing.form, pin.hands, displaced});
    }
    for (const Pin &pin : before)
    {
        if (!Holds(now, pin))
            out.restored.push_back(pin);
    }
    return out;
}

} // namespace ft
