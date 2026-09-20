#include "core/OpenRows.h"

#include <utility>

namespace ft
{
namespace
{

// Move whatever is remembered of `from` onto `to`, leaving `from`
// untouched again. Nothing moves where nothing was remembered: an
// untouched drawer stays untouched, and so keeps whatever the default is
// rather than being pinned to what the default happened to be today.
void Carry(std::unordered_map<std::string, bool> &keys, const std::string &from, const std::string &to)
{
    const auto it = keys.find(from);
    if (it == keys.end())
    {
        keys.erase(to);
        return;
    }
    const bool open = it->second;
    keys.erase(it);
    keys[to] = open;
}

} // namespace

std::string OpenRows::Key(ActorId actor, Moment moment, std::size_t index)
{
    return "rule/" + std::to_string(actor) + (moment == Moment::Idle ? "/idle/" : "/combat/") + std::to_string(index);
}

bool OpenRows::IsOpen(const std::string &key, bool whenUntouched) const noexcept
{
    const auto it = keys_.find(key);
    return it == keys_.end() ? whenUntouched : it->second;
}

void OpenRows::Open(const std::string &key)
{
    keys_[key] = true;
}

void OpenRows::Close(const std::string &key)
{
    keys_[key] = false;
}

void OpenRows::Move(ActorId actor, Moment moment, std::size_t from, std::size_t to)
{
    const std::string fromKey = Key(actor, moment, from);
    const std::string toKey = Key(actor, moment, to);
    const auto fromIt = keys_.find(fromKey);
    const auto toIt = keys_.find(toKey);
    const bool hadFrom = fromIt != keys_.end();
    const bool hadTo = toIt != keys_.end();
    const bool fromOpen = hadFrom && fromIt->second;
    const bool toOpen = hadTo && toIt->second;
    keys_.erase(fromKey);
    keys_.erase(toKey);
    if (hadFrom)
        keys_[toKey] = fromOpen;
    if (hadTo)
        keys_[fromKey] = toOpen;
}

void OpenRows::Remove(ActorId actor, Moment moment, std::size_t at, std::size_t count)
{
    keys_.erase(Key(actor, moment, at));
    for (std::size_t i = at + 1; i < count; ++i)
        Carry(keys_, Key(actor, moment, i), Key(actor, moment, i - 1));
}

void OpenRows::Clear() noexcept
{
    keys_.clear();
}

} // namespace ft
