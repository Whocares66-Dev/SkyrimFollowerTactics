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

bool IsBanned(const Bans &bans, const Holdable &thing) noexcept
{
    return std::any_of(bans.begin(), bans.end(),
                       [&](const Banned &b) { return b.form == thing.form && SameVariant(b.variant, thing.variant); });
}

namespace
{
// The same entry exactly: the form's own ban is its own entry here, not a
// wildcard.
bool SameBan(const Banned &a, std::uint32_t form, const std::optional<ItemVariant> &variant) noexcept
{
    return a.form == form && a.variant.has_value() == variant.has_value() && SameVariant(a.variant, variant);
}
} // namespace

bool Ban(Bans &bans, std::uint32_t form, const std::optional<ItemVariant> &variant)
{
    if (form == 0)
        return false;
    if (std::any_of(bans.begin(), bans.end(), [&](const Banned &b) { return SameBan(b, form, variant); }))
        return false;
    if (!variant)
        std::erase_if(bans, [form](const Banned &b) { return b.form == form; });
    else if (std::any_of(bans.begin(), bans.end(), [&](const Banned &b) { return b.form == form && !b.variant; }))
        return false; // every copy is banned already, this one with them
    bans.push_back({form, variant});
    return true;
}

bool Unban(Bans &bans, std::uint32_t form, const std::optional<ItemVariant> &variant)
{
    const auto before = bans.size();
    if (!variant)
        std::erase_if(bans, [form](const Banned &b) { return b.form == form; });
    else
        std::erase_if(bans, [&](const Banned &b) { return SameBan(b, form, variant); });
    return bans.size() != before;
}

bool WouldDualWield(const Holdable &thing, const Holdable *inOtherHand) noexcept;

bool Conflicts(const Holdable &incoming, Hand hands, const Holdable &held, Hand heldHands, bool dualWield) noexcept
{
    if (hands != Hand::None && heldHands != Hand::None)
    {
        if (Overlap(hands, heldHands))
            return true;
        // Where the style forbids two, a one-hander into one hand displaces
        // a one-hander pinned in the other -- a different weapon, or a
        // second copy of the same.
        return !dualWield && incoming.form != held.form && WouldDualWield(incoming, &held);
    }
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

const Holdable *FindHoldable(const std::vector<Holdable> &things, std::uint32_t form,
                             const std::optional<ItemVariant> &variant) noexcept
{
    if (!variant)
        return FindHoldable(things, form);
    const auto it = std::find_if(things.begin(), things.end(), [&](const Holdable &t) {
        return t.form == form && t.variant && SameVariant(t.variant, variant);
    });
    return it == things.end() ? nullptr : &*it;
}

Pin *FindPin(std::vector<Pin> &pins, const Holdable &thing) noexcept
{
    const auto it =
        std::find_if(pins.begin(), pins.end(), [&thing](const Pin &p) { return SameThing(p.thing, thing); });
    return it == pins.end() ? nullptr : &*it;
}

const Pin *FindPin(const std::vector<Pin> &pins, const Holdable &thing) noexcept
{
    const auto it =
        std::find_if(pins.begin(), pins.end(), [&thing](const Pin &p) { return SameThing(p.thing, thing); });
    return it == pins.end() ? nullptr : &*it;
}

const Pin *FindPin(const std::vector<Pin> &pins, std::uint32_t form) noexcept
{
    Holdable any;
    any.form = form;
    return FindPin(pins, any);
}

const Pin *FindPin(const std::vector<Pin> &pins, std::uint32_t form, const std::optional<ItemVariant> &variant)
{
    Holdable named;
    named.form = form;
    named.variant = variant;
    return FindPin(pins, named);
}

std::vector<Displaced> MakeRoom(std::vector<Pin> &pins, const Holdable &thing, Hand hands, bool dualWield)
{
    std::vector<Displaced> out;
    for (auto it = pins.begin(); it != pins.end();)
    {
        if (SameThing(it->thing, thing) || !Conflicts(thing, hands, it->thing, it->hands, dualWield))
        {
            ++it;
            continue;
        }
        const Hand taken = Common(hands, it->hands);
        const bool partly = it->thing.grip == Grip::Either && taken != Hand::None && taken != it->hands;
        if (partly)
        {
            out.push_back({it->thing.form, it->thing.variant, taken});
            it->hands = Without(it->hands, taken);
            ++it;
            continue;
        }
        out.push_back({it->thing.form, it->thing.variant, it->hands});
        it = pins.erase(it);
    }
    return out;
}

void AddPin(std::vector<Pin> &pins, const Holdable &thing, Hand hands, bool moving)
{
    if (Pin *pin = FindPin(pins, thing))
    {
        pin->hands = thing.grip == Grip::Either && !moving ? pin->hands | hands : hands;
        // A pin on the form, whichever copy, narrowed to the variant now
        // given.
        if (!pin->thing.variant && thing.variant)
            pin->thing.variant = thing.variant;
        return;
    }
    pins.push_back({thing, hands});
}

Hand LetGo(std::vector<Pin> &pins, const Holdable &thing, Hand hands)
{
    Pin *pin = FindPin(pins, thing);
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
    const Pin *pin = FindPin(pins, thing);
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
        // and also when this is their only one: no second copy for that hand.
        if (Overlap(slot, pin->hands))
            return false;
        return thing.count < 2 || Overlap(slot, pinned);
    }
    return Overlap(slot, pinned);
}

Refusal RefusesEngineEquip(const std::vector<Pin> &pins, const Bans &bans, const Holdable &thing, Hand into,
                           bool dualWield) noexcept
{
    if (IsBanned(bans, thing))
        return {Refusal::Why::Banned, nullptr};
    if (pins.empty())
        return {};
    if (const Pin *own = FindPin(pins, thing))
    {
        if (into == Hand::None || own->hands == Hand::None)
            return {};
        // The pinned variant itself into a hand its pin does not hold, with
        // no second copy of the variant for it: one copy cannot be in two
        // hands. Another variant of the form is another thing, below.
        if (thing.variant && own->thing.variant && thing.count < 2 && !Overlap(into, own->hands))
            return {Refusal::Why::OneCopy, own};
        if (!KeptFromAI(pins, thing, into))
            return {};
        return {thing.count < 2 ? Refusal::Why::OneCopy : Refusal::Why::OtherPin, own};
    }
    if (thing.grip == Grip::None && thing.slots == 0 && !thing.IsAmmo() && !thing.IsVoice())
        return {}; // a potion, a scroll: no hand, no slot, nothing a pin holds
    const Hand hands = HandsFor(thing.grip, into);
    for (const Pin &pin : pins)
        if (Conflicts(thing, hands, pin.thing, pin.hands, dualWield))
            return {Refusal::Why::Conflict, &pin};
    return {};
}

std::vector<Displaced> ApplyRequest(std::vector<Pin> &pins, PinRequest request, const Holdable &thing, Hand hands,
                                    bool moving, bool dualWield)
{
    switch (request)
    {
    case PinRequest::Pin: {
        std::vector<Displaced> displaced = MakeRoom(pins, thing, hands, dualWield);
        AddPin(pins, thing, hands, moving);
        return displaced;
    }
    case PinRequest::Equip: {
        // The AI's to change afterwards; but a pin in the way would put its
        // thing straight back, so the request lets that pin go. The only
        // copy of a weapon changing hands takes its own pin with it: a pin
        // on the hand it is leaving would stand over an empty hand.
        std::vector<Displaced> displaced = MakeRoom(pins, thing, hands, dualWield);
        if (moving) [[maybe_unused]]
            const Hand left = LetGo(pins, thing, Without(Hand::Both, hands));
        return displaced;
    }
    case PinRequest::Ban:
        [[maybe_unused]] const Hand let = LetGo(pins, thing, Hand::None);
        return {};
    }
    return {};
}

// A pin with no hand holding the place a thing would take: a body slot
// they share, the quiver, or the voice.
bool HoldsPlaceOf(const Pin &pin, const Holdable &thing) noexcept
{
    if (SameThing(pin.thing, thing))
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
        if (SameThing(pin.thing, thing))
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
    return a.thing.form == b.thing.form && a.thing.variant.has_value() == b.thing.variant.has_value() &&
           SameVariant(a.thing.variant, b.thing.variant) && a.hands == b.hands;
}

bool Holds(const std::vector<Pin> &pins, const Pin &pin) noexcept
{
    return std::any_of(pins.begin(), pins.end(), [&](const Pin &p) { return SamePin(p, pin); });
}
} // namespace

bool AllRowsBanned(const Bans &bans, std::uint32_t form, const std::vector<ItemVariant> &rows)
{
    Holdable any;
    any.form = form;
    if (!IsBanned(bans, any))
        return false;
    if (std::any_of(bans.begin(), bans.end(), [form](const Banned &b) { return b.form == form && !b.variant; }))
        return true;
    if (rows.empty())
        return false; // bans on variants of a form with no rows in the bag ban nothing that is there
    return std::all_of(rows.begin(), rows.end(), [&](const ItemVariant &variant) {
        Holdable row;
        row.form = form;
        row.variant = variant;
        return IsBanned(bans, row);
    });
}

Shadow ShadowOf(const std::vector<Pin> &pins, const Bans &bans, const Holdable &thing, Hand slot,
                const std::vector<ItemVariant> &rows, bool heldInOtherHand)
{
    // A pin on the form is the override: the thing is scored as if unbanned.
    const bool pinned = FindPin(pins, thing.form) != nullptr;
    if (!pinned && AllRowsBanned(bans, thing.form, rows))
        return Shadow::Banned;
    if (thing.kind == Kind::Weapon && thing.count < 2 && (slot == Hand::Left || slot == Hand::Right) && heldInOtherHand)
        return Shadow::OnlyOneInOtherHand;
    return KeptFromAI(pins, thing, slot) ? Shadow::PinnedAgainst : Shadow::None;
}

std::optional<std::size_t> EnginePick(const std::vector<VariantInBag> &copies, const Bans &bans, std::uint32_t form)
{
    Holdable thing;
    thing.form = form;
    const auto allowed = [&](const VariantInBag &c) {
        thing.variant = c.variant;
        return !c.worn && !IsBanned(bans, thing);
    };
    for (std::size_t i = 0; i < copies.size(); ++i)
        if (copies[i].plainToEngine && allowed(copies[i]))
            return i;
    for (std::size_t i = 0; i < copies.size(); ++i)
        if (allowed(copies[i]))
            return i;
    return std::nullopt;
}

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
            return !SameThing(saved.thing, pin.thing) && Conflicts(pin.thing, pin.hands, saved.thing, saved.hands);
        });
        out.released.push_back({pin.thing.form, pin.thing.variant, pin.hands, displaced});
    }
    for (const Pin &pin : before)
    {
        if (!Holds(now, pin))
            out.restored.push_back(pin);
    }
    return out;
}

} // namespace ft
