#pragma once
// Which actors a view manages, as plain atomics: the check every hook makes
// first. The hooks run for every NPC, from any thread, and loading a
// published std::shared_ptr is a spin lock and two reference counts in
// MSVC's library -- so only the few managed actors pay for that. Past
// kMax ids the check says yes to everyone rather than answer wrong.
//
// A reader racing Set sees a moment of mixed lists, which the hooks
// tolerate: an id listed without a view falls through to the engine, and
// one briefly missing is answered from its record for that one call.

#include <array>
#include <atomic>
#include <cstddef>

namespace fp::game
{

class FastIds
{
  public:
    static constexpr std::size_t kMax = 128;

    [[nodiscard]] bool Maybe(RE::FormID id) const noexcept
    {
        if (overflow_.load(std::memory_order_relaxed))
            return true;
        const std::size_t n = count_.load(std::memory_order_acquire);
        for (std::size_t i = 0; i < n; ++i)
            if (ids_[i].load(std::memory_order_relaxed) == id)
                return true;
        return false;
    }

    // Game thread: the keys of a map keyed by RE::FormID.
    template <typename Map> void Set(const Map &views) noexcept
    {
        std::size_t n = 0;
        for (const auto &entry : views)
            if (n < kMax)
                ids_[n++].store(entry.first, std::memory_order_relaxed);
        overflow_.store(views.size() > kMax, std::memory_order_relaxed);
        count_.store(n, std::memory_order_release);
    }

    void Clear() noexcept
    {
        count_.store(0, std::memory_order_release);
        overflow_.store(false, std::memory_order_relaxed);
    }

  private:
    std::array<std::atomic<RE::FormID>, kMax> ids_{};
    std::atomic<std::size_t> count_{0};
    std::atomic<bool> overflow_{false};
};

} // namespace fp::game
