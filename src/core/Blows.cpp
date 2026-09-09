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
    // A two-hander fills both hands; it is the right hand's swing.
    if (hands.right == Held::TwoHander)
        return Swing::Right;
    const bool right = SwingsItself(hands.right);
    const bool left = hands.left == Held::OneHander;
    if (right && left)
        return Swing::Both;
    if (right)
        return Swing::Right;
    if (left && hands.right == Held::Nothing)
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
