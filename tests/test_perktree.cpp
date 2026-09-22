#include "core/PerkTree.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <numbers>
#include <vector>

using Catch::Approx;

namespace
{

// A node at `x` across whose first rank asks `level`.
ft::PerkTreeNode At(double x, float level = 0.0f)
{
    ft::PerkTreeNode node;
    node.x = x;
    node.firstRequirement = level;
    node.requirement = level;
    return node;
}

constexpr float kRing = 10.0f;
constexpr float kGap = 5.0f;
constexpr float kLine = 16.0f;

ft::TreeDrawing Draw(const std::vector<ft::PerkTreeNode> &nodes, const std::vector<float> &labels, float width,
                     float height)
{
    return ft::LayOutTree(nodes, labels, kRing, kGap, kLine, width, height);
}

constexpr float kPi = std::numbers::pi_v<float>;

} // namespace

TEST_CASE("a tree is mirrored across, and a node sits up the page by its first rank's level", "[perktree]")
{
    const std::vector<ft::PerkTreeNode> nodes{At(1.0, 0.0f), At(2.0, 50.0f), At(0.0, 100.0f)};
    const auto d = Draw(nodes, {}, 400.0f, 216.0f);
    REQUIRE(d.centres.size() == 3);
    CHECK(d.centres[1].x < d.centres[0].x); // the larger x drawn left
    CHECK(d.centres[0].x < d.centres[2].x);
    // Level 0 at the bottom, 100 at the top, a circle's reach in from each
    // edge (more than half a line here).
    CHECK(d.centres[0].y == Approx(206.0f));
    CHECK(d.centres[2].y == Approx(10.0f));
    CHECK(d.centres[1].y == Approx((d.centres[0].y + d.centres[2].y) / 2.0f));
}

TEST_CASE("a cluster in the records opens up as widely as the rest: the columns are evenly spaced", "[perktree]")
{
    // 0 and 0.3 are close in the records; 5 is far off. Three columns, one
    // gap each.
    const auto d = Draw({At(0.0), At(0.3, 50.0f), At(5.0, 100.0f)}, {}, 400.0f, 300.0f);
    const float wide = d.centres[1].x - d.centres[2].x;
    const float narrow = d.centres[0].x - d.centres[1].x;
    CHECK(wide == Approx(narrow));
    // Nodes at one place across share its column.
    const auto shared = Draw({At(1.0), At(1.0, 50.0f), At(0.0, 50.0f)}, {}, 400.0f, 300.0f);
    CHECK(shared.centres[0].x == shared.centres[1].x);
}

TEST_CASE("a perk a hair off the column beside it is drawn in that column, unless one there shares its level",
          "[perktree]")
{
    // Alchemy's Green Thumb (2.46, level 70) over Concentrated Poison (2.567,
    // level 60): straight above it. Two at one level stay apart.
    const auto d = Draw({At(2.567, 60.0f), At(2.46, 70.0f), At(0.0, 0.0f)}, {}, 400.0f, 300.0f);
    CHECK(d.centres[0].x == d.centres[1].x);
    const auto apart = Draw({At(2.567, 60.0f), At(2.46, 60.0f), At(0.0, 0.0f)}, {}, 400.0f, 300.0f);
    CHECK(apart.centres[0].x != apart.centres[1].x);
}

TEST_CASE("a tree is stretched across the page's width, its labels kept on it", "[perktree]")
{
    const std::vector<ft::PerkTreeNode> nodes{At(0.0), At(1.0, 50.0f), At(2.0, 100.0f)};
    const std::vector<float> labels{60.0f, 60.0f, 60.0f};
    const float width = 500.0f;
    const auto d = Draw(nodes, labels, width, 300.0f);
    float first = width;
    float last = 0.0f;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        first = (std::min)({first, d.centres[i].x - kRing, d.labels[i].x});
        last = (std::max)({last, d.centres[i].x + kRing, d.labels[i].x + labels[i]});
    }
    CHECK(first == Approx(0.0f).margin(0.01));
    CHECK(last == Approx(width).margin(0.01));
    // No labels: the circles alone reach the edges.
    const auto bare = Draw(nodes, {}, width, 300.0f);
    CHECK(bare.centres[2].x == Approx(kRing));
    CHECK(bare.centres[0].x == Approx(width - kRing));
}

TEST_CASE("what an actor holds never moves a node or its label", "[perktree]")
{
    std::vector<ft::PerkTreeNode> nodes{At(1.0, 0.0f), At(2.0, 30.0f), At(0.0, 60.0f)};
    const std::vector<float> labels{40.0f, 50.0f, 60.0f};
    const auto before = Draw(nodes, labels, 400.0f, 300.0f);
    nodes[1].held = 2;
    nodes[1].ranks = 3;
    nodes[1].requirement = 90.0f; // its next rank's, which is not where it sits
    nodes[2].held = 1;
    const auto after = Draw(nodes, labels, 400.0f, 300.0f);
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        CHECK(after.centres[i].x == before.centres[i].x);
        CHECK(after.centres[i].y == before.centres[i].y);
        CHECK(after.places[i] == before.places[i]);
    }
}

TEST_CASE("labels in a crowded row find places clear of each other and of the circles", "[perktree]")
{
    // Four at one level, labels wider than the columns are apart: beside
    // their circles they would run into each other; above and below, not.
    const std::vector<ft::PerkTreeNode> nodes{At(3.0, 50.0f), At(2.0, 50.0f), At(1.0, 50.0f), At(0.0, 50.0f)};
    const std::vector<float> labels{120.0f, 120.0f, 120.0f, 120.0f};
    const auto d = Draw(nodes, labels, 500.0f, 300.0f);
    const auto labelBox = [&](std::size_t i) {
        return std::array<float, 4>{d.labels[i].x, d.labels[i].x + labels[i], d.labels[i].y, d.labels[i].y + kLine};
    };
    const auto circleBox = [&](std::size_t i) {
        return std::array<float, 4>{d.centres[i].x - kRing, d.centres[i].x + kRing, d.centres[i].y - kRing,
                                    d.centres[i].y + kRing};
    };
    const auto meet = [](const std::array<float, 4> &a, const std::array<float, 4> &b) {
        return a[0] < b[1] && b[0] < a[1] && a[2] < b[3] && b[2] < a[3];
    };
    for (std::size_t i = 0; i < nodes.size(); ++i)
        for (std::size_t j = 0; j < nodes.size(); ++j)
            if (i != j)
            {
                CHECK_FALSE(meet(labelBox(i), circleBox(j)));
                CHECK_FALSE(meet(labelBox(i), labelBox(j)));
            }
    // The outer ones keep their outer sides.
    CHECK(d.places.front() == ft::LabelPlace::Left);
    CHECK(d.places.back() == ft::LabelPlace::Right);
}

TEST_CASE("a tree one node wide, or of one node, sits in the middle", "[perktree]")
{
    const auto column = Draw({At(1.5, 0.0f), At(1.5, 100.0f)}, {}, 100.0f, 200.0f);
    CHECK(column.centres[0].x == Approx(50.0f));
    CHECK(column.centres[1].x == Approx(50.0f));
    CHECK(column.centres[1].y < column.centres[0].y);

    const auto one = Draw({At(4.0, 50.0f)}, {}, 100.0f, 60.0f);
    CHECK(one.centres[0].x == Approx(50.0f));
    CHECK(one.centres[0].y == Approx(30.0f));

    CHECK(Draw({}, {}, 100.0f, 100.0f).centres.empty());
}

TEST_CASE("labels too wide for the page spill, and the circles still fit", "[perktree]")
{
    const auto d = Draw({At(0.0), At(1.0)}, {500.0f, 500.0f}, 100.0f, 100.0f);
    CHECK(d.centres[1].x == Approx(kRing));
    CHECK(d.centres[0].x == Approx(100.0f - kRing));
}

TEST_CASE("a label goes on its node's outer side by default", "[perktree]")
{
    // Mirrored: the largest x is drawn at the left.
    CHECK(ft::LabelsLeft({At(2.0), At(1.0), At(0.0)}) == std::vector<bool>{true, false, false});
    CHECK(ft::LabelsLeft({At(1.0), At(1.0)}) == std::vector<bool>{false, false});
    CHECK(ft::LabelsLeft({}).empty());
}

TEST_CASE("held ranks fill that share of the circle, from the top, in convex slices", "[perktree]")
{
    CHECK(ft::HeldArcs(0, 3).empty());
    CHECK(ft::HeldArcs(2, 0).empty());

    const auto one = ft::HeldArcs(1, 3);
    REQUIRE(one.size() == 1);
    CHECK(one[0].from == Approx(-kPi / 2.0f));
    CHECK(one[0].to - one[0].from == Approx(2.0f * kPi / 3.0f));

    const auto two = ft::HeldArcs(2, 3); // past half a turn: two slices
    REQUIRE(two.size() == 2);
    CHECK(two[0].to - two[0].from == Approx(kPi));
    CHECK(two[1].from == Approx(two[0].to));
    CHECK(two[1].to - two[0].from == Approx(4.0f * kPi / 3.0f));

    for (const int held : {5, 7}) // every rank, or more than there are
    {
        const auto full = ft::HeldArcs(held, 5);
        REQUIRE(full.size() == 2);
        CHECK(full.back().to - full.front().from == Approx(2.0f * kPi));
        for (const auto &arc : full)
            CHECK(arc.to - arc.from <= kPi + 1e-5f);
    }
}
