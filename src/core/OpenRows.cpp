#include "core/OpenRows.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

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
    if (from == to)
        return;
    // What is remembered of each rule in the span, by where it goes: the
    // moved one to `to`, each between it and there one place towards
    // `from`. Taken out whole and put back, so no move overwrites another.
    const std::size_t lo = (std::min)(from, to);
    const std::size_t hi = (std::max)(from, to);
    std::vector<std::pair<std::size_t, std::optional<bool>>> moved;
    for (std::size_t i = lo; i <= hi; ++i)
    {
        const auto it = keys_.find(Key(actor, moment, i));
        const std::size_t dest = i == from ? to : (from < to ? i - 1 : i + 1);
        moved.emplace_back(dest, it == keys_.end() ? std::nullopt : std::optional<bool>(it->second));
        if (it != keys_.end())
            keys_.erase(it);
    }
    for (const auto &[dest, open] : moved)
        if (open)
            keys_[Key(actor, moment, dest)] = *open;
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
