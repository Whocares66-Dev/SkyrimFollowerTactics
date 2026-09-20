#include "core/Blows.h"

namespace ft
{

namespace
{

bool SwingsItself(Held held) noexcept
{
    return held == Held::OneHander || held == Held::TwoHander;
}

bool IsWeapon(Held held) noexcept
{
    return held == Held::OneHander || held == Held::TwoHander || held == Held::Bow || held == Held::Staff;
}

} // namespace

Swing SwingWith(Hands hands) noexcept
{
    // The right hand first, then the left, as a poison goes on: a two-hander
    // fills both hands and is the right hand's swing.
    if (hands.right == Held::TwoHander)
        return Swing::Right;
    const bool right = SwingsItself(hands.right);
    const bool left = hands.left == Held::OneHander;
    if (right && left)
        return Swing::Both;
    if (right)
        return Swing::Right;
    // Whatever the right holds that does not swing -- a spell, a staff --
    // the left's blade swings alone, as the player's left attack does.
    if (left)
        return Swing::Left;
    if (hands.right == Held::Nothing && hands.left == Held::Nothing)
        return Swing::Fists;
    return Swing::None;
}

bool BashesWith(Hands hands) noexcept
{
    if (hands.left == Held::Shield || hands.left == Held::Torch)
        return true;
    // Any weapon in the right hand blocks with the left hand empty: a
    // one-hander, a staff, and a two-hander or a bow, whose left hand is
    // described as empty.
    return IsWeapon(hands.right) && hands.left == Held::Nothing;
}

} // namespace ft

namespace ft
{

const char *PowerAttackEvent(Swing swing) noexcept
{
    switch (swing)
    {
    case Swing::Both:
        return "attackPowerStartDualWield";
    case Swing::Right:
    case Swing::Fists:
        return "attackPowerStartInPlace";
    case Swing::Left:
        return "attackPowerStartInPlaceLeftHand";
    case Swing::None:
    default:
        return nullptr;
    }
}

const char *BashEvent(bool power) noexcept
{
    return power ? "bashPowerStart" : "bashStart";
}

float PowerAttackStamina(float rightWeight, float weaponMult, float weaponBase, float penalty) noexcept
{
    return (rightWeight * weaponMult + weaponBase) * penalty;
}

Hand HandToPoison(bool rightTakes, bool rightPoisoned, bool leftTakes, bool leftPoisoned) noexcept
{
    if (rightTakes && !rightPoisoned)
        return Hand::Right;
    if (leftTakes && !leftPoisoned)
        return Hand::Left;
    return Hand::None;
}

float ChargeAfterRecharge(float charge, float maxCharge, float soul) noexcept
{
    const float added = charge + (soul > 0.0f ? soul : 0.0f);
    return added < maxCharge ? added : maxCharge;
}

std::vector<ItemStep> PlanPoison(bool weapon, bool wornCopy)
{
    if (!weapon || !wornCopy)
        return {};
    return {ItemStep::WriteDose, ItemStep::SpendPoison, ItemStep::PlaySound};
}

std::vector<ItemStep> PlanRecharge(bool weapon, bool wornCopy, bool gem, bool reusable)
{
    if (!weapon || !gem || !wornCopy)
        return {};
    return {ItemStep::WriteCharge, ItemStep::RefreshAbility, reusable ? ItemStep::EmptyGem : ItemStep::SpendGem,
            ItemStep::PlaySound};
}

} // namespace ft
