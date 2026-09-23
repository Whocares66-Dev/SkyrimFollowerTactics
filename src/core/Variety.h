#pragma once
// Variety in a follower's attack spells: the combat AI's own choice, made
// less predictable. The engine rescores every entry of its combat
// inventory once a second and takes the best of each category, with
// nothing random on the path (dev/COMBAT_AI.md), so the same attack spell
// wins every time. Two terms change that, both applied to the score the
// engine asks for, so its own "best first" does the choosing:
//
//   a recency penalty    a spell cast lately scores less, by how many
//                        attack casts ago, fading geometrically: the
//                        language model's frequency penalty, counted in
//                        casts rather than seconds, so it is the same
//                        for a quick caster and a slow one
//   a held random draw   each entry's score times exp(T * (g - gamma)),
//                        g a Gumbel(0, 1) draw: the Gumbel-max trick, so
//                        the best of the adjusted scores is entry i with
//                        probability proportional to score_i ^ (1 / T) --
//                        softmax over log scores at temperature T
//
// The draw is scale-free (every score times a constant changes no
// probability), so a follower at level 5 and one at 50 need no tuning
// apart. A score of 0 or less is left alone: the engine never queues it,
// which is how a pin, a ban and a spell it cannot use stay out.
//
// The draws are held, not redrawn at each rescore: a fresh draw a second
// would swap the spell whenever the engine's minimum equip time let it,
// which reads as flicker. Each entry keeps its draw until something says
// it has had its turn or cannot have one: ITS spell cast, and the draw
// at least `minHoldSeconds` old, the engine's own minimum equip time; the
// entry unusable (Release: a score of 0, an enemy it cannot touch); the
// enemy changed; the fight over. No timer beyond the engine's: until
// 2026-09-23 a draw with no cast went after 10 s, a guess at when "never
// cast" was meant. Until the same day one cast redrew every entry:
// Serana's Ice Storm won a draw, was put in the right hand, and lost its
// draw to the left hand's Blood Javelin being cast before it ever was.
//
// The game side feeds it the engine's score, the clock and the casts
// (game/AiScore.cpp); what is made of them is here, where it is tested.
// No Skyrim.

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ft
{

struct VarietySettings
{
    // The draw's spread. 0 is the engine's own order; 1 picks in
    // proportion to score; higher is nearer uniform.
    double temperature{1.0};
    // What a spell loses for each of the last attack casts that was it:
    // the last cast takes `recentPenalty` of its score, the one before
    // that times `recencyDecay`, and so on, each cast of it multiplying
    // in -- the last three casts A, B, A leave A x0.5 x0.875. The last
    // `recencyMemory` casts are kept; past eight the penalty is below half
    // a percent.
    double recentPenalty{0.5};
    double recencyDecay{0.5};
    std::size_t recencyMemory{8};
    // No new draw for a cast sooner than this after the last: the engine's
    // own minimum equip time for magic.
    double minHoldSeconds{3.0};
    // The draw's factor is kept within this many times either way. The
    // Gumbel's tail is long -- factors past a thousand were seen in play
    // (2026-09-22) -- and the engine reads the same answer beyond picking
    // (dev/COMBAT_AI.md 3: the range a loadout fights at, the 10% rule on a
    // short-reach attack), where a score a thousand times its worth shuts
    // everything else out. At 10 the odds move about a point: 3 against 1
    // wins 75.5% of the time, not 75%.
    double maxDrawFactor{10.0};
};

class Variety
{
  public:
    // One score, and the two factors that made it from the engine's: for
    // the log, which says why an entry won. `drawn` is which draw it is,
    // counted over the fight; `fresh`, that it was made just now.
    struct Varied
    {
        float score{0.0f};
        float recency{1.0f};
        float draw{1.0f};
        std::uint32_t drawn{0};
        bool fresh{false};
    };

    explicit Variety(std::uint64_t seed, VarietySettings settings = {});

    // A new fight: every draw and every cast forgotten, the hold begins
    // now.
    void Reset(double now);

    // The engine's score for one entry, varied, against `target`, the
    // enemy the controller is fighting: a draw is for one enemy, and a new
    // one draws anew. `entry` names the entry -- the engine lists an
    // either-hand spell once per hand, and each hand has a draw of its own
    // -- and `spell` the spell, so a cast from either hand counts against
    // both. A score of 0 or less is the entry unusable: its draw goes
    // (Release), and the score is returned as it was.
    [[nodiscard]] Varied Adjust(std::uint64_t entry, std::uint32_t spell, float score, double now,
                                std::uint32_t target = 0);

    // The entry cannot be used now -- an enemy its spell cannot touch, a
    // score the engine answers 0 -- so its draw goes, and it draws anew
    // when it can be.
    void Release(std::uint64_t entry);

    // A spell was cast. An attack spell -- one scored here -- goes to the
    // front of the casts its penalty is counted in, and its entries'
    // draws may go; anything else (a heal, a buff) neither counts nor
    // ages the others.
    void NoteCast(std::uint32_t spell);

  private:
    struct Draw
    {
        float factor{1.0f};
        double at{0.0};
        std::uint32_t number{0};
        std::uint64_t castsAt{0}; // attack casts counted when it was drawn
        std::uint32_t target{0};
    };
    [[nodiscard]] bool Spent(const Draw &draw, std::uint32_t spell, double now, std::uint32_t target) const;
    [[nodiscard]] float Recency(std::uint32_t spell) const noexcept;
    [[nodiscard]] double NextUniform() noexcept;

    VarietySettings settings_;
    std::uint64_t rng_;
    std::uint32_t drawn_{0};
    std::unordered_map<std::uint64_t, Draw> draws_;
    // The attack spells, the ones scored here; the last attack casts,
    // newest first; how many there have been, and at which count each
    // spell was last cast.
    std::unordered_set<std::uint32_t> attacks_;
    std::vector<std::uint32_t> recent_;
    std::uint64_t casts_{0};
    std::unordered_map<std::uint32_t, std::uint64_t> lastCast_;
};

} // namespace ft
