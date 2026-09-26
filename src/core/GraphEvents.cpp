#include "core/GraphEvents.h"

#include <algorithm>
#include <cstdio>

namespace ft
{
namespace
{

// In GraphTag's order.
constexpr std::array<std::string_view, kGraphTagCount> kNames{
    "attackStop",
    "PowerAttackStop",
    "PowerAttack_Start_end",
    "preHitFrame",
    "weaponSwing",
    "HitFrame",
    "blockStartOut",
    "blockStop",
    "bashStop",
    "bashExit",
    "BeginCastLeft",
    "BeginCastRight",
    "BeginCastVoice",
    "MLh_SpellFire_Event",
    "MRh_SpellFire_Event",
    "Voice_SpellFire_Event",
    "CastStop",
    "shoutStop",
    "Magic_Equip_Out",
};

constexpr char Fold(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The graph's own comparison is case-blind.
bool SameName(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Fold(x) == Fold(y); });
}

bool Matches(const OwnFire &fire, GraphTag tag, std::uint32_t form) noexcept
{
    return fire.tag == tag && (!fire.form || *fire.form == form);
}

} // namespace

std::optional<GraphTag> GraphTagOf(std::string_view name) noexcept
{
    for (std::size_t i = 0; i < kNames.size(); ++i)
        if (SameName(name, kNames[i]))
            return static_cast<GraphTag>(i);
    return std::nullopt;
}

std::string_view GraphTagName(GraphTag tag) noexcept
{
    const auto i = static_cast<std::size_t>(tag);
    return i < kNames.size() ? kNames[i] : std::string_view{};
}

std::string StepAt(double now)
{
    char text[48]{};
    std::snprintf(text, sizeof(text), "step at %.3f: ", now);
    return text;
}

bool IsFire(GraphTag tag) noexcept
{
    return tag == GraphTag::SpellFireLeft || tag == GraphTag::SpellFireRight || tag == GraphTag::SpellFireVoice;
}

GraphWatch::GraphWatch(GraphWatch &&other) noexcept : owner_(other.owner_), id_(other.id_)
{
    other.owner_ = nullptr;
    other.id_ = 0;
}

GraphWatch &GraphWatch::operator=(GraphWatch &&other) noexcept
{
    if (this != &other)
    {
        Close();
        owner_ = other.owner_;
        id_ = other.id_;
        other.owner_ = nullptr;
        other.id_ = 0;
    }
    return *this;
}

GraphWatch::~GraphWatch()
{
    Close();
}

Heard GraphWatch::HeardSoFar() const
{
    return owner_ ? owner_->HeardBy(id_) : Heard{};
}

void GraphWatch::SetWakes(GraphTags wakes) const
{
    if (owner_)
        owner_->SetWakes(id_, wakes);
}

void GraphWatch::Close() noexcept
{
    if (owner_)
        owner_->Close(id_);
    owner_ = nullptr;
    id_ = 0;
}

GraphWatch GraphWatches::Open(std::uint32_t actor, GraphTags wakes, std::span<const OwnFire> fires)
{
    std::scoped_lock lock(mutex_);
    Watch watch;
    watch.id = next_++;
    watch.actor = actor;
    watch.wakes = wakes;
    watch.fires.assign(fires.begin(), fires.end());
    watches_.push_back(std::move(watch));
    open_.store(watches_.size(), std::memory_order_relaxed);
    return GraphWatch(this, watches_.back().id);
}

Recorded GraphWatches::Record(std::uint32_t actor, GraphTag tag, std::uint32_t form)
{
    Recorded recorded;
    const auto index = static_cast<std::size_t>(tag);
    if (index >= kGraphTagCount)
        return recorded;
    std::scoped_lock lock(mutex_);
    for (Watch &watch : watches_)
    {
        if (watch.actor != actor)
            continue;
        recorded.watched = true;
        Heard &heard = watch.heard;
        if (tag == GraphTag::CastStop && heard.ownFires > 0)
            ++heard.stopsAfterOwnFire;
        ++heard.counts[index];
        if (std::any_of(watch.fires.begin(), watch.fires.end(),
                        [&](const OwnFire &fire) { return Matches(fire, tag, form); }))
        {
            ++heard.ownFires;
            recorded.ownFire = true;
        }
        if (watch.wakes.Has(tag))
            recorded.wakes = true;
    }
    return recorded;
}

bool GraphWatches::Watching(std::uint32_t actor) const
{
    std::scoped_lock lock(mutex_);
    return std::any_of(watches_.begin(), watches_.end(), [actor](const Watch &watch) { return watch.actor == actor; });
}

Heard GraphWatches::HeardBy(std::uint64_t id) const
{
    std::scoped_lock lock(mutex_);
    const auto it = std::find_if(watches_.begin(), watches_.end(), [id](const Watch &watch) { return watch.id == id; });
    return it == watches_.end() ? Heard{} : it->heard;
}

void GraphWatches::SetWakes(std::uint64_t id, GraphTags wakes)
{
    std::scoped_lock lock(mutex_);
    const auto it = std::find_if(watches_.begin(), watches_.end(), [id](const Watch &watch) { return watch.id == id; });
    if (it != watches_.end())
        it->wakes = wakes;
}

void GraphWatches::Close(std::uint64_t id) noexcept
{
    std::scoped_lock lock(mutex_);
    std::erase_if(watches_, [id](const Watch &watch) { return watch.id == id; });
    open_.store(watches_.size(), std::memory_order_relaxed);
}

} // namespace ft
