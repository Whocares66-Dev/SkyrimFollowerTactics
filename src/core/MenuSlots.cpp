#include "core/MenuSlots.h"

#include <algorithm>

namespace ft
{

MenuSlots::MenuSlots(std::size_t count) : slots_(count)
{
}

ActorId MenuSlots::OwnerOf(std::size_t slot) const noexcept
{
    return slot < slots_.size() ? slots_[slot].id : 0;
}

const std::string &MenuSlots::NameOf(std::size_t slot) const noexcept
{
    static const std::string none;
    return slot < slots_.size() ? slots_[slot].name : none;
}

std::vector<std::size_t> MenuSlots::Gone(std::span<const ActorId> present) const
{
    std::vector<std::size_t> gone;
    for (std::size_t i = 0; i < slots_.size(); ++i)
    {
        if (slots_[i].id == 0)
            continue;
        if (std::find(present.begin(), present.end(), slots_[i].id) == present.end())
            gone.push_back(i);
    }
    return gone;
}

void MenuSlots::Free(std::size_t slot)
{
    if (slot < slots_.size())
        slots_[slot] = {};
}

std::vector<MenuSlots::Placed> MenuSlots::Arrivals(std::span<const MenuSlot> present, double now, double settle)
{
    std::vector<const MenuSlot *> arriving;
    std::unordered_set<ActorId> ids;
    for (const MenuSlot &who : present)
    {
        const bool known = std::any_of(slots_.begin(), slots_.end(), [&](const MenuSlot &s) { return s.id == who.id; });
        if (!known)
        {
            arriving.push_back(&who);
            ids.insert(who.id);
        }
    }
    if (ids != pending_)
    {
        pending_ = std::move(ids);
        lastArrival_ = now;
    }
    if (pending_.empty() || now - lastArrival_ < settle)
        return {};
    pending_.clear();

    std::sort(arriving.begin(), arriving.end(), [](const MenuSlot *a, const MenuSlot *b) { return a->name < b->name; });
    std::vector<Placed> placed;
    for (const MenuSlot *who : arriving)
    {
        std::size_t index = slots_.size();
        for (std::size_t i = 0; i < slots_.size(); ++i)
        {
            if (slots_[i].id == 0)
            {
                index = i;
                slots_[i] = *who;
                break;
            }
        }
        placed.push_back({index, *who});
    }
    return placed;
}

} // namespace ft
