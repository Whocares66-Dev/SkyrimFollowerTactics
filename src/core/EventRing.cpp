#include "EventRing.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace ft::log
{

EventRing::EventRing(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1))
{
}

std::uint64_t EventRing::Push(LoggedEvent event)
{
    event.sequence = ++latest_;
    events_.push_back(std::move(event));
    if (events_.size() > capacity_)
        events_.pop_front();
    return latest_;
}

std::vector<LoggedEvent> EventRing::Since(std::uint64_t after) const
{
    if (events_.empty() || after >= latest_)
        return {};
    // The numbers held are consecutive, so the first one wanted is found by
    // arithmetic rather than by a search.
    const std::uint64_t oldest = events_.front().sequence;
    const std::uint64_t skip = after >= oldest ? after - oldest + 1 : 0;
    return {std::next(events_.begin(), static_cast<std::ptrdiff_t>(skip)), events_.end()};
}

std::uint64_t EventRing::Latest() const noexcept
{
    return latest_;
}

std::size_t EventRing::Size() const noexcept
{
    return events_.size();
}

} // namespace ft::log
