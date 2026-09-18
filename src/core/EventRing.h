#pragma once
// The last game events, kept in memory for the panel (dev/EVENTS.md "The
// last 1,000 in memory"). The panel can stay open with time running, so a
// Logs tab has to show an event as it happens; reading the events file back
// would mean file I/O and JSON parsing for the render thread, a half-written
// last line, a file a second behind its buffer or switched off altogether.
// This is the list it reads instead. Not thread-safe: src/game/Log.cpp holds
// it behind a mutex.

#include "LogEvent.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace ft::log
{

inline constexpr std::size_t kRecentEvents = 1000;

// One game event as it was emitted. It owns what it holds: a field's key is
// a string literal with static storage, which is what Field already asks of
// every call site, so keeping the fields is safe.
struct LoggedEvent
{
    // Never reused, and counting on past what was dropped, so a reader asks
    // for what is newer than the last number it saw however many went.
    std::uint64_t sequence{0};
    std::string timestamp;
    Level level{Level::Info};
    std::string name;
    std::uint32_t followerId{0};
    std::string followerName;
    std::string prose;
    std::vector<Field> fields;
};

class EventRing
{
  public:
    // At least one: a ring that holds nothing would hand out numbers for
    // events no one can read.
    explicit EventRing(std::size_t capacity);

    // Keeps the event, dropping the oldest past capacity, and returns the
    // sequence number it was given (whatever the event carried is replaced).
    std::uint64_t Push(LoggedEvent event);

    // What is held with a sequence number above `after`, oldest first: 0 for
    // everything held.
    [[nodiscard]] std::vector<LoggedEvent> Since(std::uint64_t after) const;

    // The last sequence number given; 0 before the first.
    [[nodiscard]] std::uint64_t Latest() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;

  private:
    std::size_t capacity_;
    std::deque<LoggedEvent> events_;
    std::uint64_t latest_{0};
};

} // namespace ft::log
