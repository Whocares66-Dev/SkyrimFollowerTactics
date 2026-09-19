#pragma once
// The followers' entries in the panel's menu: a fixed number of slots,
// each an entry the framework can add but neither remove nor move once
// added. Who gets which slot, when a newcomer is added and when a slot
// is freed is decided here, with the clock handed in; the panel adds and
// deletes the entries (game/UI.cpp, SyncFollowers). No Skyrim, no menu
// framework.

#include "Snapshot.h"

#include <cstddef>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace ft
{

struct MenuSlot
{
    ActorId id{0}; // 0: free
    std::string name;
};

class MenuSlots
{
  public:
    explicit MenuSlots(std::size_t count);

    [[nodiscard]] std::size_t Count() const noexcept
    {
        return slots_.size();
    }
    // Whose the slot is, 0 for free or none such.
    [[nodiscard]] ActorId OwnerOf(std::size_t slot) const noexcept;
    [[nodiscard]] const std::string &NameOf(std::size_t slot) const noexcept;

    // The taken slots whose actor is not among `present`: dismissed. The
    // panel deletes each entry, and frees the slot where the framework
    // allowed it; where it did not, the slot stays theirs, so the entry
    // still reads as their page if they are recruited again.
    [[nodiscard]] std::vector<std::size_t> Gone(std::span<const ActorId> present) const;
    void Free(std::size_t slot);

    // The newcomers among `present` -- not in any slot -- held until none
    // new has appeared for `settle` seconds, then taken as one batch in
    // name order into the first free slots, so the followers who appear
    // together list alphabetically. A load reveals the party over a few
    // ticks, so the batch waits for it. A newcomer with no slot left is
    // returned with `slot` == Count(), for the panel to say so.
    struct Placed
    {
        std::size_t slot{0};
        MenuSlot who;
    };
    [[nodiscard]] std::vector<Placed> Arrivals(std::span<const MenuSlot> present, double now, double settle);

  private:
    std::vector<MenuSlot> slots_;
    std::unordered_set<ActorId> pending_;
    double lastArrival_{0.0};
};

} // namespace ft
