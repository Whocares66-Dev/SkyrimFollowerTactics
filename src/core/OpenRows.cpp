#include "core/OpenRows.h"

namespace ft
{

std::string OpenRows::Key(ActorId actor, std::size_t index)
{
    return "rule/" + std::to_string(actor) + "/" + std::to_string(index);
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

void OpenRows::Move(ActorId actor, std::size_t from, std::size_t to)
{
    const bool fromOpen = keys_.erase(Key(actor, from)) > 0;
    const bool toOpen = keys_.erase(Key(actor, to)) > 0;
    if (fromOpen)
        keys_.insert(Key(actor, to));
    if (toOpen)
        keys_.insert(Key(actor, from));
}

void OpenRows::Remove(ActorId actor, std::size_t at, std::size_t count)
{
    keys_.erase(Key(actor, at));
    for (std::size_t i = at + 1; i < count; ++i)
        if (keys_.erase(Key(actor, i)) > 0)
            keys_.insert(Key(actor, i - 1));
}

void OpenRows::Clear() noexcept
{
    keys_.clear();
}

} // namespace ft
