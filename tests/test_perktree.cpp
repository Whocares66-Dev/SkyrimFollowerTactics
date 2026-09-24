#include "core/PerkTree.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

// A node at `x` across and `y` up in the records, whose perk asks no level.
ft::PerkTreeNode Placed(double x, double y)
{
    ft::PerkTreeNode node = At(x);
    node.y = y;
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

TEST_CASE("a perk a hair off the column beside it is drawn in that column, unless their circles would meet",
          "[perktree]")
{
    // Alchemy's Green Thumb (2.46, level 70) over Concentrated Poison (2.567,
    // level 60): straight above it. Two at one level stay apart, and two a
    // few levels apart, whose circles would overlap.
    const auto d = Draw({At(2.567, 60.0f), At(2.46, 70.0f), At(0.0, 0.0f)}, {}, 400.0f, 300.0f);
    CHECK(d.centres[0].x == d.centres[1].x);
    const auto apart = Draw({At(2.567, 60.0f), At(2.46, 60.0f), At(0.0, 0.0f)}, {}, 400.0f, 300.0f);
    CHECK(apart.centres[0].x != apart.centres[1].x);
    const auto near = Draw({At(2.567, 60.0f), At(2.46, 63.0f), At(0.0, 0.0f)}, {}, 400.0f, 300.0f);
    CHECK(near.centres[0].x != near.centres[1].x);
}

TEST_CASE("a tree whose perks ask no level sits by the records' own heights", "[perktree]")
{
    const auto d = Draw({Placed(1.0, 0.5), Placed(2.0, 2.0), Placed(0.0, 3.5)}, {}, 400.0f, 216.0f);
    CHECK(d.centres[0].y == Approx(206.0f));
    CHECK(d.centres[2].y == Approx(10.0f));
    CHECK(d.centres[1].y == Approx((d.centres[0].y + d.centres[2].y) / 2.0f));
    // Perks that all ask one level have no height in it either.
    std::vector<ft::PerkTreeNode> alike{Placed(1.0, 0.5), Placed(2.0, 2.0), Placed(0.0, 3.5)};
    for (auto &node : alike)
        node.firstRequirement = 20.0f;
    const auto same = Draw(alike, {}, 400.0f, 216.0f);
    for (std::size_t i = 0; i < alike.size(); ++i)
        CHECK(same.centres[i].y == d.centres[i].y);
    // A tree whose perks ask levels keeps them, whatever the records' heights.
    std::vector<ft::PerkTreeNode> levelled = alike;
    levelled[0].firstRequirement = 100.0f;
    const auto byLevel = Draw(levelled, {}, 400.0f, 216.0f);
    CHECK(byLevel.centres[0].y == Approx(10.0f));
    CHECK(byLevel.centres[1].y == byLevel.centres[2].y);
}

TEST_CASE("a tree at the records' places keeps their spacing across", "[perktree]")
{
    // Mirrored: 4 leftmost, three units from 1, which is one from 0. Drawn
    // three times as far apart, not as evenly spaced columns.
    const auto d = Draw({Placed(0.0, 0.0), Placed(1.0, 1.0), Placed(4.0, 2.0)}, {}, 400.0f, 300.0f);
    CHECK(d.centres[2].x < d.centres[1].x);
    CHECK(d.centres[1].x < d.centres[0].x);
    CHECK(d.centres[1].x - d.centres[2].x == Approx(3.0f * (d.centres[0].x - d.centres[1].x)));
}

TEST_CASE("circles that would meet at the records' places are pushed apart", "[perktree]")
{
    const auto d = Draw({Placed(1.0, 1.0), Placed(1.02, 1.01), Placed(0.0, 0.0), Placed(3.0, 2.0)}, {}, 400.0f, 300.0f);
    CHECK(std::hypot(d.centres[0].x - d.centres[1].x, d.centres[0].y - d.centres[1].y) >= Approx(2.0f * kRing + kGap));
    // Two on one spot part too, and stay in the box.
    const auto same =
        Draw({Placed(1.0, 1.0), Placed(1.0, 1.0), Placed(0.0, 0.0), Placed(3.0, 2.0)}, {}, 400.0f, 300.0f);
    CHECK(std::hypot(same.centres[0].x - same.centres[1].x, same.centres[0].y - same.centres[1].y) >=
          Approx(2.0f * kRing + kGap));
    for (const ft::TreePoint &c : same.centres)
    {
        CHECK(c.x >= kRing);
        CHECK(c.x <= 400.0f - kRing);
    }
}

TEST_CASE("Vampire Lord's tree, which asks no levels, is drawn with no two circles meeting", "[perktree]")
{
    // The perks' places in the records (Scion.esp's tree, 2026-09-24): the
    // grid cell plus the offset in it, across and up.
    const std::vector<ft::PerkTreeNode> nodes{
        Placed(4.029, 0.280), Placed(2.786, 0.457), Placed(1.543, 0.871), Placed(0.143, 2.000), Placed(3.014, 1.800),
        Placed(4.029, 2.740), Placed(4.029, 3.680), Placed(5.057, 1.840), Placed(5.257, 0.486), Placed(6.586, 0.900),
        Placed(8.043, 2.000), Placed(5.414, 1.114), Placed(6.329, 2.014), Placed(2.657, 1.086), Placed(1.829, 2.057)};
    const auto d = Draw(nodes, {}, 900.0f, 600.0f);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        for (std::size_t j = i + 1; j < nodes.size(); ++j)
            CHECK(std::hypot(d.centres[i].x - d.centres[j].x, d.centres[i].y - d.centres[j].y) >= 2.0f * kRing);
    // Not one row: the lowest at the bottom, the highest at the top.
    CHECK(d.centres[0].y == Approx(600.0f - kRing));
    CHECK(d.centres[6].y == Approx(kRing));
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

TEST_CASE("a label keeps off its own node's link when another place is clear", "[perktree]")
{
    // Four columns; the third from the right (x 2) is in the left half, so
    // its label would go left -- along the link to its child (x 3), in the
    // column beside it and barely higher.
    std::vector<ft::PerkTreeNode> nodes{At(0.0), At(1.0), At(2.0), At(3.0, 5.0f)};
    const std::vector<float> labels{60.0f, 60.0f, 60.0f, 60.0f};
    const auto unlinked = Draw(nodes, labels, 1000.0f, 300.0f);
    REQUIRE(unlinked.places[2] == ft::LabelPlace::Left);

    nodes[2].children = {3};
    const auto d = Draw(nodes, labels, 1000.0f, 300.0f);
    CHECK(d.places[2] != ft::LabelPlace::Left);
    // Nothing of the link, from ring to ring, runs through the label.
    const ft::TreePoint a = d.centres[2];
    const ft::TreePoint b = d.centres[3];
    const float length = std::hypot(b.x - a.x, b.y - a.y);
    for (int step = 0; step <= 100; ++step)
    {
        const float along = kRing + (length - 2.0f * kRing) * static_cast<float>(step) / 100.0f;
        const float x = a.x + (b.x - a.x) * along / length;
        const float y = a.y + (b.y - a.y) * along / length;
        const bool inside =
            x > d.labels[2].x && x < d.labels[2].x + labels[2] && y > d.labels[2].y && y < d.labels[2].y + kLine;
        CHECK_FALSE(inside);
    }
    // The node itself stays where it was.
    CHECK(d.centres[2].x == Approx(unlinked.centres[2].x));
    CHECK(d.centres[2].y == Approx(unlinked.centres[2].y));
}

TEST_CASE("a link runs from one circle's edge to the other's, and none is drawn between circles that touch",
          "[perktree]")
{
    std::vector<ft::PerkTreeNode> nodes{At(0.0, 0.0f), At(0.0, 100.0f)};
    nodes[0].children = {1};
    const auto d = Draw(nodes, {}, 400.0f, 216.0f);
    REQUIRE(d.links.size() == 1);
    const ft::TreeLink &link = d.links.front();
    CHECK(link.from == 0);
    CHECK(link.to == 1);
    CHECK_FALSE(link.bowed); // nothing stands in its way
    // Straight up the column, a ring's reach in from each centre.
    CHECK(link.a.x == Approx(d.centres[0].x));
    CHECK(link.a.y == Approx(d.centres[0].y - kRing));
    CHECK(link.b.y == Approx(d.centres[1].y + kRing));

    // In a box so small that the two circles touch, there is nowhere to
    // draw it. (Short alone is not enough: two whose circles would meet
    // are not given one column.)
    nodes[1].x = 1.0;
    const auto squeezed = Draw(nodes, {}, 2.0f * kRing + 4.0f, 2.0f * kRing + 4.0f);
    REQUIRE(std::hypot(squeezed.centres[0].x - squeezed.centres[1].x, squeezed.centres[0].y - squeezed.centres[1].y) <
            2.0f * kRing);
    CHECK(squeezed.links.empty());
}

TEST_CASE("a link bows clear of a node standing between its ends", "[perktree]")
{
    // Three in one column: the lowest links to the highest, and the middle
    // one is on the line between them, as Adamant's Alteration tree has it.
    std::vector<ft::PerkTreeNode> nodes{At(0.0, 0.0f), At(0.0, 50.0f), At(0.0, 100.0f)};
    nodes[0].children = {2};
    const auto d = Draw(nodes, {}, 400.0f, 216.0f);
    REQUIRE(d.links.size() == 1);
    const ft::TreeLink &link = d.links.front();
    CHECK(link.bowed);
    // Every point of the curve keeps clear of the circle it passes.
    const auto along = [&](float t) {
        const float u = 1.0f - t;
        return ft::TreePoint{u * u * link.a.x + 2.0f * u * t * link.control.x + t * t * link.b.x,
                             u * u * link.a.y + 2.0f * u * t * link.control.y + t * t * link.b.y};
    };
    float nearest = 1e9f;
    for (int step = 0; step <= 100; ++step)
    {
        const ft::TreePoint at = along(static_cast<float>(step) / 100.0f);
        nearest = (std::min)(nearest, std::hypot(at.x - d.centres[1].x, at.y - d.centres[1].y));
    }
    CHECK(nearest >= kRing);
    // It still starts and ends on its own circles.
    CHECK(std::hypot(link.a.x - d.centres[0].x, link.a.y - d.centres[0].y) == Approx(kRing));
    CHECK(std::hypot(link.b.x - d.centres[2].x, link.b.y - d.centres[2].y) == Approx(kRing));
    // And the middle one's own links, to nobody, are not drawn.
    CHECK(d.links.size() == 1);
}

TEST_CASE("a node beside the line between two others leaves their link straight", "[perktree]")
{
    // The same three, the middle one a column over: nothing to bow around.
    std::vector<ft::PerkTreeNode> nodes{At(0.0, 0.0f), At(1.0, 50.0f), At(0.0, 100.0f)};
    nodes[0].children = {2};
    const auto d = Draw(nodes, {}, 400.0f, 216.0f);
    REQUIRE(d.links.size() == 1);
    CHECK_FALSE(d.links.front().bowed);
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
