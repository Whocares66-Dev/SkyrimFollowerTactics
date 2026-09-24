#pragma once
// Which rules' drawers the panel has open, by actor and rule index, and
// how that follows the rules when they are moved or removed: a drawer
// must not stay behind at an index another rule has taken. The panel
// draws and clicks (game/UI.cpp); the book-keeping is here, tested. No
// Skyrim, no ImGui.

#include "Rule.h"
#include "Snapshot.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace ft
{

class OpenRows
{
  public:
    // The key the panel names a rule's row by: the actor, the list the
    // rule is in and its place in it, "rule/<actor>/combat/<index>". The
    // list is part of it because an actor has two, and a drawer open on
    // the combat list is not the idle list's: keyed by index alone, rule
    // zero of one list opened and deleted rule zero of the other.
    [[nodiscard]] static std::string Key(ActorId actor, Moment moment, std::size_t index);

    // Whether a drawer is open. `whenUntouched` is what one the player has
    // not clicked is: a rule's actions start EXPANDED, because a rule with
    // three actions folded away reads as one action and the other two are
    // the part you came to check (reported in play, 2026-09-19); a sheet's
    // extra rows start folded, being detail asked for rather than the
    // thing itself. Only a drawer the player has actually clicked is
    // remembered, so changing a default moves every drawer that was left
    // alone and none that was not.
    [[nodiscard]] bool IsOpen(const std::string &key, bool whenUntouched) const noexcept;
    void Open(const std::string &key);
    void Close(const std::string &key);

    // A rule of one list moved from `from` to `to`, the rules between
    // shifting a place towards where it was: their drawers go with them.
    // One step is a swap.
    void Move(ActorId actor, Moment moment, std::size_t from, std::size_t to);
    // A rule removed from a list of `count`: its drawer goes, and the
    // drawers of the rules after it in that list move up one.
    void Remove(ActorId actor, Moment moment, std::size_t at, std::size_t count);
    void Clear() noexcept;

  private:
    // Only the drawers the player has clicked, and which way. Absent is
    // "untouched", which the caller answers for.
    std::unordered_map<std::string, bool> keys_;
};

// Where a rule dragged from `from` lands, dropped before row `before` (the
// list's length for after the last): its own place is out of the list
// while it is carried, so a drop below it lands one higher.
[[nodiscard]] constexpr std::size_t DroppedAt(std::size_t from, std::size_t before) noexcept
{
    return before > from ? before - 1 : before;
}

// The item at `from` moved to `to`, the ones between shifting a place: the
// list's half of OpenRows::Move.
template <class T> void MoveItem(std::vector<T> &items, std::size_t from, std::size_t to)
{
    if (from >= items.size() || to >= items.size() || from == to)
        return;
    const auto at = items.begin();
    if (from < to)
        std::rotate(at + static_cast<std::ptrdiff_t>(from), at + static_cast<std::ptrdiff_t>(from) + 1,
                    at + static_cast<std::ptrdiff_t>(to) + 1);
    else
        std::rotate(at + static_cast<std::ptrdiff_t>(to), at + static_cast<std::ptrdiff_t>(from),
                    at + static_cast<std::ptrdiff_t>(from) + 1);
}

} // namespace ft
