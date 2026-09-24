#include "PerkTree.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>

namespace ft
{
namespace
{

constexpr double kUnbounded = std::numeric_limits<double>::infinity();

// The scales at which the drawings along one axis fit in `room`: `at` is
// each node's distance along it from the tree's first edge, in record units,
// and `before` and `after` how far its drawing reaches either way. Every
// pair fits when scale * (at_i - at_j) + after_i + before_j <= room, a node
// with itself included: an upper bound on the scale where i is the further
// along. (Where i is the nearer, the pair only asks for a larger scale, and
// only when their drawings alone overflow; the largest scale that fits the
// rest answers that too, or nothing does.)
double MostScale(const std::vector<double> &at, const std::vector<float> &before, const std::vector<float> &after,
                 double room)
{
    double most = kUnbounded;
    for (std::size_t i = 0; i < at.size(); ++i)
        for (std::size_t j = 0; j < at.size(); ++j)
        {
            const double over = static_cast<double>(after[i]) + before[j] - room;
            const double apart = at[i] - at[j];
            if (apart > 0.0)
                most = (std::min)(most, -over / apart);
            else if (apart == 0.0 && over > 0.0)
                most = (std::min)(most, 0.0); // two drawings in one place, wider than the box
        }
    return most;
}

} // namespace

std::vector<bool> LabelsLeft(const std::vector<PerkTreeNode> &nodes)
{
    std::vector<bool> out(nodes.size(), false);
    if (nodes.empty())
        return out;
    const auto [lo, hi] = std::minmax_element(nodes.begin(), nodes.end(),
                                              [](const PerkTreeNode &a, const PerkTreeNode &b) { return a.x < b.x; });
    const double span = hi->x - lo->x;
    if (span <= 0.0)
        return out;
    // The menu mirrors x: a node with more than the middle is drawn left of it.
    const double middle = (lo->x + hi->x) / 2.0;
    for (std::size_t i = 0; i < nodes.size(); ++i)
        out[i] = nodes[i].x > middle + span * 0.01;
    return out;
}

namespace
{

struct Box
{
    float left{0.0f};
    float right{0.0f};
    float top{0.0f};
    float bottom{0.0f};
    [[nodiscard]] bool Meets(const Box &other) const
    {
        return left < other.right && other.left < right && top < other.bottom && other.top < bottom;
    }
    [[nodiscard]] bool Inside(float width, float height) const
    {
        return left >= 0.0f && right <= width && top >= 0.0f && bottom <= height;
    }
    [[nodiscard]] bool Holds(TreePoint p) const
    {
        return p.x > left && p.x < right && p.y > top && p.y < bottom;
    }
};

// Whether the segment from `a` to `b` passes through the box: an end inside
// it, or a crossing of one of its sides.
bool Crosses(TreePoint a, TreePoint b, const Box &box)
{
    if (box.Holds(a) || box.Holds(b))
        return true;
    const auto cross = [](TreePoint p, TreePoint q, TreePoint r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    const auto meet = [&](TreePoint p, TreePoint q, TreePoint r, TreePoint t) {
        const float d1 = cross(p, q, r);
        const float d2 = cross(p, q, t);
        const float d3 = cross(r, t, p);
        const float d4 = cross(r, t, q);
        return ((d1 > 0.0f) != (d2 > 0.0f)) && ((d3 > 0.0f) != (d4 > 0.0f));
    };
    const TreePoint tl{box.left, box.top};
    const TreePoint tr{box.right, box.top};
    const TreePoint bl{box.left, box.bottom};
    const TreePoint br{box.right, box.bottom};
    return meet(a, b, tl, tr) || meet(a, b, tr, br) || meet(a, b, br, bl) || meet(a, b, bl, tl);
}

// A point on a link's curve, `t` of the way along it.
TreePoint Along(const TreeLink &link, float t)
{
    const float u = 1.0f - t;
    return {u * u * link.a.x + 2.0f * u * t * link.control.x + t * t * link.b.x,
            u * u * link.a.y + 2.0f * u * t * link.control.y + t * t * link.b.y};
}

// The link between two nodes, drawn clear of every other circle between
// them. The curve stands 2(1-t)t of its control's offset off the straight
// line at `t`, so a node standing `t` of the way along, `across` from the
// line, is cleared by pushing the control `(want +- across) / 2(1-t)t` the
// other way. The side asking the smaller push wins. The push is capped: a
// node nearly on one end stands where the curve has barely left the line,
// and would otherwise throw the control across the page. Nothing is drawn
// between two circles that touch.
std::optional<TreeLink> LinkBetween(const std::vector<TreePoint> &centres, std::size_t from, std::size_t to, float ring,
                                    float clear)
{
    const TreePoint a = centres[from];
    const TreePoint b = centres[to];
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 2.0f * ring)
        return std::nullopt;
    const float alongX = dx / length;
    const float alongY = dy / length;
    const float asideX = -alongY;
    const float asideY = alongX;
    const float want = ring + clear;
    float push[2]{0.0f, 0.0f}; // the way `aside` points, and the other way
    for (std::size_t j = 0; j < centres.size(); ++j)
    {
        if (j == from || j == to)
            continue;
        const float toX = centres[j].x - a.x;
        const float toY = centres[j].y - a.y;
        const float along = (toX * alongX + toY * alongY) / length;
        if (along <= 0.0f || along >= 1.0f)
            continue; // past an end: the ends' own circles are where the link starts
        const float across = toX * asideX + toY * asideY;
        if (std::abs(across) >= want)
            continue; // the link already passes wide of it
        const float share = 2.0f * (1.0f - along) * along;
        push[0] = (std::max)(push[0], (want + across) / share);
        push[1] = (std::max)(push[1], (want - across) / share);
    }
    TreeLink link;
    link.from = from;
    link.to = to;
    link.control = {(a.x + b.x) / 2.0f, (a.y + b.y) / 2.0f};
    if (push[0] > 0.0f || push[1] > 0.0f)
    {
        constexpr float kMostBow = 4.0f; // of what a circle asks
        const bool aside = push[0] <= push[1];
        const float bow = (std::min)(aside ? push[0] : push[1], kMostBow * want);
        const float side = aside ? bow : -bow;
        link.control = {link.control.x + asideX * side, link.control.y + asideY * side};
        link.bowed = true;
    }
    // From one circle's edge to the other's, each along the way the curve
    // leaves it -- toward the control, which is the middle when straight.
    const auto edge = [&](TreePoint p) {
        const float toX = link.control.x - p.x;
        const float toY = link.control.y - p.y;
        const float reach = std::sqrt(toX * toX + toY * toY);
        return reach > 0.0f ? TreePoint{p.x + toX / reach * ring, p.y + toY / reach * ring} : p;
    };
    link.a = edge(a);
    link.b = edge(b);
    return link;
}

// The places a label is tried in, from its outer side round.
constexpr LabelPlace kFromLeft[] = {LabelPlace::Left, LabelPlace::UpLeft, LabelPlace::DownLeft, LabelPlace::Up,
                                    LabelPlace::Down, LabelPlace::Right,  LabelPlace::UpRight,  LabelPlace::DownRight};
constexpr LabelPlace kFromRight[] = {LabelPlace::Right,  LabelPlace::UpRight, LabelPlace::DownRight,
                                     LabelPlace::Up,     LabelPlace::Down,    LabelPlace::Left,
                                     LabelPlace::UpLeft, LabelPlace::DownLeft};

// Where a strategy stands the nodes before the page is fitted: across in
// its own units, which the fitting stretches to the box, and up already on
// the page, between `top` and `bottom`.
struct Placement
{
    std::vector<double> across;
    std::vector<float> rise;
};

// A tree whose perks ask levels of the skill. Up, the first rank's level, 0
// at the bottom and 100 at the top. Across, the columns in the menu's order
// (mirrored: the largest x leftmost), evenly spaced: every distinct place
// across is a column, so a cluster in the records opens up as widely as the
// rest. A place within kNear of the column beside it joins it -- the records
// set a perk a hair off the one it grows from, and it is drawn straight
// above -- unless its circle would meet one already there.
Placement ByLevel(const std::vector<PerkTreeNode> &nodes, float ring, float top, float bottom)
{
    const std::size_t n = nodes.size();
    Placement out{std::vector<double>(n, 0.0), std::vector<float>(n)};
    for (std::size_t i = 0; i < n; ++i)
        out.rise[i] = bottom - (std::clamp)(nodes[i].firstRequirement, 0.0f, 100.0f) / 100.0f * (bottom - top);

    constexpr double kNear = 0.15;
    std::vector<std::size_t> byX(n);
    for (std::size_t i = 0; i < n; ++i)
        byX[i] = i;
    std::stable_sort(byX.begin(), byX.end(), [&](std::size_t a, std::size_t b) { return nodes[a].x > nodes[b].x; });
    std::size_t column = 0;
    double previous = nodes[byX.front()].x;
    std::vector<float> heights{out.rise[byX.front()]};
    for (std::size_t k = 1; k < n; ++k)
    {
        const std::size_t i = byX[k];
        const bool taken = std::any_of(heights.begin(), heights.end(),
                                       [&](float y) { return std::abs(y - out.rise[i]) < 2.0f * ring; });
        if (previous - nodes[i].x > kNear || taken)
        {
            ++column;
            heights.clear();
        }
        previous = nodes[i].x;
        heights.push_back(out.rise[i]);
        out.across[i] = static_cast<double>(column);
    }
    return out;
}

// A tree whose perks all ask alike -- nothing at all, as Vampire Lord's and
// Werewolf's -- has no height in its levels, and stands as the menu draws
// it: at the records' own places, mirrored across, the lowest at the bottom
// and the highest at the top. Only for a tree whose heights differ
// (PlacedByLevel).
Placement ByRecord(const std::vector<PerkTreeNode> &nodes, float top, float bottom)
{
    const auto byX = [](const PerkTreeNode &a, const PerkTreeNode &b) { return a.x < b.x; };
    const auto byY = [](const PerkTreeNode &a, const PerkTreeNode &b) { return a.y < b.y; };
    const double mostX = std::max_element(nodes.begin(), nodes.end(), byX)->x;
    const auto [lowY, highY] = std::minmax_element(nodes.begin(), nodes.end(), byY);
    const double span = highY->y - lowY->y;
    Placement out;
    for (const PerkTreeNode &node : nodes)
    {
        out.across.push_back(mostX - node.x);
        out.rise.push_back(bottom - static_cast<float>((node.y - lowY->y) / span) * (bottom - top));
    }
    return out;
}

// A tree is placed by its levels unless they place nothing: every perk asks
// the same, and the records' heights differ.
bool PlacedByLevel(const std::vector<PerkTreeNode> &nodes)
{
    const auto [low, high] =
        std::minmax_element(nodes.begin(), nodes.end(), [](const PerkTreeNode &a, const PerkTreeNode &b) {
            return a.firstRequirement < b.firstRequirement;
        });
    const auto [lowY, highY] = std::minmax_element(
        nodes.begin(), nodes.end(), [](const PerkTreeNode &a, const PerkTreeNode &b) { return a.y < b.y; });
    return low->firstRequirement != high->firstRequirement || highY->y <= lowY->y;
}

// Circles nearer than `apart`, centre to centre, pushed away from each other
// along the line between them, half the shortfall each, and kept in the box;
// a pass at a time, in the nodes' order so the drawing is the same every
// time, until none is too near or kPasses have run. Two on one spot part
// across, the earlier to the left.
void Separate(std::vector<TreePoint> &centres, float apart, float left, float right, float top, float bottom)
{
    constexpr int kPasses = 64;
    const auto keep = [&](TreePoint &p) {
        p.x = (std::clamp)(p.x, left, (std::max)(left, right));
        p.y = (std::clamp)(p.y, top, (std::max)(top, bottom));
    };
    for (int pass = 0; pass < kPasses; ++pass)
    {
        bool moved = false;
        for (std::size_t i = 0; i < centres.size(); ++i)
            for (std::size_t j = i + 1; j < centres.size(); ++j)
            {
                const float dx = centres[j].x - centres[i].x;
                const float dy = centres[j].y - centres[i].y;
                const float distance = std::sqrt(dx * dx + dy * dy);
                if (distance >= apart)
                    continue;
                const float push = (apart - distance) / 2.0f;
                const float ux = distance > 0.0f ? dx / distance : 1.0f;
                const float uy = distance > 0.0f ? dy / distance : 0.0f;
                centres[i] = {centres[i].x - ux * push, centres[i].y - uy * push};
                centres[j] = {centres[j].x + ux * push, centres[j].y + uy * push};
                keep(centres[i]);
                keep(centres[j]);
                moved = true;
            }
        if (!moved)
            return;
    }
}

} // namespace

TreeDrawing LayOutTree(const std::vector<PerkTreeNode> &nodes, const std::vector<float> &labelWidth, float ring,
                       float gap, float lineHeight, float width, float height)
{
    TreeDrawing out;
    if (nodes.empty())
        return out;
    const std::size_t n = nodes.size();
    const std::vector<bool> outerLeft = LabelsLeft(nodes);
    const auto labelOf = [&](std::size_t i) { return i < labelWidth.size() ? labelWidth[i] : 0.0f; };

    // Up is inside the box by half a line.
    const float half = (std::max)(ring, lineHeight / 2.0f);
    float top = half;
    float bottom = height - half;
    if (bottom < top)
        top = bottom = height / 2.0f;
    const bool byLevel = PlacedByLevel(nodes);
    const Placement place = byLevel ? ByLevel(nodes, ring, top, bottom) : ByRecord(nodes, top, bottom);
    const std::vector<double> &across = place.across;

    // Across, stretched to the widest that keeps every circle and label in
    // the box.
    std::vector<float> left(n);
    std::vector<float> right(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const float reach = labelOf(i) > 0.0f ? ring + gap + labelOf(i) : ring;
        left[i] = outerLeft[i] ? reach : ring;
        right[i] = outerLeft[i] ? ring : reach;
    }
    double scale = MostScale(across, left, right, width);
    if (scale <= 0.0 && n > 1)
    {
        // No stretch keeps the labels in: they spill, and the circles alone
        // are fitted, so the tree is still drawn rather than a column.
        std::vector<float> circle(n, ring);
        scale = MostScale(across, circle, circle, width);
        left = circle;
        right = circle;
    }
    if (!std::isfinite(scale))
        scale = 0.0; // every node in one column
    scale = (std::max)(scale, 0.0);
    double first = kUnbounded;
    double last = -kUnbounded;
    for (std::size_t i = 0; i < n; ++i)
    {
        first = (std::min)(first, scale * across[i] - left[i]);
        last = (std::max)(last, scale * across[i] + right[i]);
    }
    const double offset = (width - (last - first)) / 2.0 - first;

    out.centres.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
        out.centres.push_back({static_cast<float>(offset + scale * across[i]), place.rise[i]});
    // A tree at the records' places keeps their crowds, and a crowd's
    // circles are parted with a label's air between them. A tree by its
    // levels has no crowds: its columns part them.
    if (!byLevel)
        Separate(out.centres, 2.0f * ring + gap, ring, width - ring, top, bottom);

    // The links, each bowed past any node standing between its ends, so a
    // link that passes a node is not read as one that meets it. A node is
    // cleared by `gap`, the same air a label is given.
    for (std::size_t i = 0; i < n; ++i)
        for (const std::size_t child : nodes[i].children)
            if (child < n && child != i)
                if (const auto link = LinkBetween(out.centres, i, child, ring, gap))
                    out.links.push_back(*link);

    // The labels: each at the place about its circle that runs into the
    // least, given where the others are; three passes, in the nodes' order,
    // so the drawing is the same every time.
    const float corner = ring * 0.75f; // a corner clears the circle itself
    const auto labelBox = [&](std::size_t i, LabelPlace place) {
        const TreePoint c = out.centres[i];
        const float w = labelOf(i);
        switch (place)
        {
        case LabelPlace::Left:
            return Box{c.x - ring - gap - w, c.x - ring - gap, c.y - lineHeight / 2.0f, c.y + lineHeight / 2.0f};
        case LabelPlace::UpRight:
            return Box{c.x + corner, c.x + corner + w, c.y - corner - lineHeight, c.y - corner};
        case LabelPlace::UpLeft:
            return Box{c.x - corner - w, c.x - corner, c.y - corner - lineHeight, c.y - corner};
        case LabelPlace::DownRight:
            return Box{c.x + corner, c.x + corner + w, c.y + corner, c.y + corner + lineHeight};
        case LabelPlace::DownLeft:
            return Box{c.x - corner - w, c.x - corner, c.y + corner, c.y + corner + lineHeight};
        case LabelPlace::Up:
            return Box{c.x - w / 2.0f, c.x + w / 2.0f, c.y - ring - gap / 2.0f - lineHeight, c.y - ring - gap / 2.0f};
        case LabelPlace::Down:
            return Box{c.x - w / 2.0f, c.x + w / 2.0f, c.y + ring + gap / 2.0f, c.y + ring + gap / 2.0f + lineHeight};
        case LabelPlace::Right:
        default:
            return Box{c.x + ring + gap, c.x + ring + gap + w, c.y - lineHeight / 2.0f, c.y + lineHeight / 2.0f};
        }
    };
    const auto circleOf = [&](std::size_t i) {
        const TreePoint c = out.centres[i];
        return Box{c.x - ring, c.x + ring, c.y - ring, c.y + ring};
    };
    out.places.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        out.places[i] = outerLeft[i] ? LabelPlace::Left : LabelPlace::Right;
    // A link as drawn: the straight ones as they are, a bowed one in pieces
    // of its curve, which is near enough the curve for a label's sake.
    const auto crosses = [&](const TreeLink &link, const Box &box) {
        if (!link.bowed)
            return Crosses(link.a, link.b, box);
        constexpr int kPieces = 8;
        TreePoint previous = link.a;
        for (int k = 1; k <= kPieces; ++k)
        {
            const TreePoint at = Along(link, static_cast<float>(k) / static_cast<float>(kPieces));
            if (Crosses(previous, at, box))
                return true;
            previous = at;
        }
        return false;
    };
    const auto cost = [&](std::size_t i, LabelPlace place, int preference) {
        if (labelOf(i) <= 0.0f)
            return preference;
        const Box mine = labelBox(i, place);
        int total = preference;
        if (!mine.Inside(width, height))
            total += 1000;
        for (std::size_t j = 0; j < n; ++j)
        {
            if (j != i)
            {
                total += mine.Meets(circleOf(j)) ? 100 : 0;
                total += labelOf(j) > 0.0f && mine.Meets(labelBox(j, out.places[j])) ? 100 : 0;
            }
        }
        // Every link, this node's own among them: a label over the line
        // into it hides the line as much as any other.
        for (const TreeLink &link : out.links)
            total += crosses(link, mine) ? 30 : 0;
        return total;
    };
    for (int pass = 0; pass < 3; ++pass)
        for (std::size_t i = 0; i < n; ++i)
        {
            const auto &order = outerLeft[i] ? kFromLeft : kFromRight;
            int best = -1;
            for (int k = 0; k < 8; ++k)
            {
                const int c = cost(i, order[k], k);
                if (best < 0 || c < best)
                {
                    best = c;
                    out.places[i] = order[k];
                }
            }
        }
    out.labels.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const Box box = labelBox(i, out.places[i]);
        out.labels.push_back({box.left, box.top});
    }
    return out;
}

std::vector<Arc> HeldArcs(int held, int ranks)
{
    std::vector<Arc> out;
    if (ranks <= 0 || held <= 0)
        return out;
    constexpr float kTop = -std::numbers::pi_v<float> / 2.0f;
    constexpr float kHalf = std::numbers::pi_v<float>;
    const float share = static_cast<float>((std::min)(held, ranks)) / static_cast<float>(ranks);
    const float turn = 2.0f * kHalf * share;
    // At most a whole turn, so at most two slices.
    if (turn <= kHalf)
        out.push_back({kTop, kTop + turn});
    else
    {
        out.push_back({kTop, kTop + kHalf});
        out.push_back({kTop + kHalf, kTop + turn});
    }
    return out;
}

} // namespace ft
