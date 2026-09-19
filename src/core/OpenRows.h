#pragma once
// Which rules' drawers the panel has open, by actor and rule index, and
// how that follows the rules when they are moved or removed: a drawer
// must not stay behind at an index another rule has taken. The panel
// draws and clicks (game/UI.cpp); the book-keeping is here, tested. No
// Skyrim, no ImGui.

#include "Snapshot.h"

#include <cstddef>
#include <string>
#include <unordered_set>

namespace ft
{

class OpenRows
{
  public:
    // The key the panel names a rule's row by: "rule/<actor>/<index>".
    [[nodiscard]] static std::string Key(ActorId actor, std::size_t index);

    [[nodiscard]] bool IsOpen(const std::string &key) const noexcept;
    void Open(const std::string &key);
    void Close(const std::string &key);

    // Two rules swapped: their drawers swap with them.
    void Move(ActorId actor, std::size_t from, std::size_t to);
    // A rule removed from a list of `count`: its drawer goes, and the
    // drawers of the rules after it move up one.
    void Remove(ActorId actor, std::size_t at, std::size_t count);
    void Clear() noexcept;

  private:
    std::unordered_set<std::string> keys_;
};

} // namespace ft
