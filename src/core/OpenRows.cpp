#include "core/OpenRows.h"

namespace ft
{

std::string OpenRows::Key(ActorId actor, Moment moment, std::size_t index)
{
    return "rule/" + std::to_string(actor) + (moment == Moment::Idle ? "/idle/" : "/combat/") + std::to_string(index);
}

bool OpenRows::IsOpen(const std::string &key) const noexcept
{
    return keys_.contains(key);
}

void OpenRows::Open(const std::string &key)
{
    keys_.insert(key);
}

void OpenRows::Close(const std::string &key)
{
    keys_.erase(key);
}

void OpenRows::Move(ActorId actor, Moment moment, std::size_t from, std::size_t to)
{
    const bool fromOpen = keys_.erase(Key(actor, moment, from)) > 0;
    const bool toOpen = keys_.erase(Key(actor, moment, to)) > 0;
    if (fromOpen)
        keys_.insert(Key(actor, moment, to));
    if (toOpen)
        keys_.insert(Key(actor, moment, from));
}

void OpenRows::Remove(ActorId actor, Moment moment, std::size_t at, std::size_t count)
{
    keys_.erase(Key(actor, moment, at));
    for (std::size_t i = at + 1; i < count; ++i)
        if (keys_.erase(Key(actor, moment, i)) > 0)
            keys_.insert(Key(actor, moment, i - 1));
}

void OpenRows::Clear() noexcept
{
    keys_.clear();
}

} // namespace ft
