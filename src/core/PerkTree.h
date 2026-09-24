#pragma once
// A skill's perk tree as the perk menu draws it, for the skill's page on the
// Skills tab: where each node goes, what the actor holds of it, and the
// geometry of drawing it -- the layout, and the part of a circle a node's
// held ranks fill. The game side reads the tree (game/Sensors.h,
// BuildPerkTrees); the panel draws it (game/UI.cpp). No Skyrim.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ft
{

struct PerkTreeNode
{
    std::string name;
    // Where the menu puts it, in the record's own units: the grid column plus
    // the node's offset within it, and the same up the tree. The menu
    // mirrors the first (TreeOrder, core/CustomSkills.h): a larger x is
    // drawn further left.
    double x{0.0};
    double y{0.0};
    int ranks{1};
    int held{0}; // of `ranks`, as the engine answers for the actor
    // A rank of it is on the actor's own record: chosen for them in advance,
    // held or given back (Progression, "Innate perks").
    bool theirs{false};
    // The skill level its first rank asks, 0 for none: how far up the page
    // it sits in a tree whose perks ask levels, which the ranks taken never
    // change.
    float firstRequirement{0.0f};
    // Of the next rank to take, or of the last when every rank is held.
    float requirement{0.0f}; // the skill level it asks; 0 for none
    std::string description;
    // The top rank held, whose page the Skills tab has; 0 when none is held.
    std::uint32_t form{0};
    // The first rank's form, used to identify an unlearned node.
    std::uint32_t firstForm{0};
    std::vector<std::size_t> children; // indices into the tree's nodes
};

struct PerkTreeView
{
    std::uint32_t key{0}; // what the skill's row names it by (SheetRow::tree)
    [[nodiscard]] std::uint32_t Key() const noexcept
    {
        return key;
    }
    std::string name; // "One-Handed"
    // The skill's level as a requirement reads it -- without fortify
    // effects: the base for the player, the menu's own measure; the base
    // and the permanent modifier for a follower, where Progression keeps
    // what they have learned -- as text and as a number; and as the row
    // shows it, with every effect on it.
    std::string value;
    float level{0.0f};
    float current{0.0f};
    std::vector<PerkTreeNode> nodes;
};

struct TreePoint
{
    float x{0.0f};
    float y{0.0f};
};

// Which side of its circle each node's label goes by default: the outer
// side, so the labels spread from the tree rather than cross it -- the left
// for a node in the left half as the menu draws it (mirrored), the right for
// one in the right half or on the middle line. True for the left.
[[nodiscard]] std::vector<bool> LabelsLeft(const std::vector<PerkTreeNode> &nodes);

// Where a label may go about its circle: beside it, at a corner, or over or
// under it.
enum class LabelPlace : std::uint8_t
{
    Right,
    Left,
    UpRight,
    UpLeft,
    DownRight,
    DownLeft,
    Up,
    Down,
};

// A link from one node to another as the page draws it: from one circle's
// edge to the other's, as a quadratic curve through `control`. A link with
// nothing in its way is straight, and its control is the middle, where the
// curve is the line; one that would otherwise run through a third node's
// circle bows past it, so a link that passes a node is seen to pass it
// rather than to meet it.
struct TreeLink
{
    std::size_t from{0}; // indices into the tree's nodes
    std::size_t to{0};
    TreePoint a;
    TreePoint b;
    TreePoint control;
    bool bowed{false};
};

// A tree as the page draws it, in a box `width` by `height` from its
// top-left corner: each node's centre, where its label goes -- the place,
// and the label's own top-left corner -- and the links between the nodes.
struct TreeDrawing
{
    std::vector<TreePoint> centres;
    std::vector<LabelPlace> places;
    std::vector<TreePoint> labels;
    std::vector<TreeLink> links;
};

// A tree is placed one of two ways. One whose perks ask levels of the skill
// is placed by them: up, the level the node's first rank asks, 0 at the
// bottom and 100 at the top; across, its columns as the menu orders them
// (mirrored), evenly spaced -- every distinct place across is a column, so a
// cluster in the records opens up as widely as the rest -- a node near a
// column joining it unless their circles would meet. One whose perks all ask
// alike (Vampire Lord's ask nothing) is placed as the menu draws it, at the
// records' own places, mirrored across; circles that would meet there are
// pushed apart until a label's gap lies between them. Either way the tree is
// stretched across to the widest that keeps every circle and label in the
// box. Both ways are the records' alone, so nothing an actor holds moves a
// node. A circle reaches `ring`
// from its centre; a label, `labelWidth[i]` wide and `lineHeight` tall,
// sits `gap` past it. Each label takes the place about its circle that
// runs into the least: off the page worst, then another node's circle or
// label, then a link drawn through it; the outer side (LabelsLeft) first
// among equals. When no stretch keeps the labels in, the circles alone are
// fitted and the labels spill. The links come with the places they run
// through, bowed clear of any node between their ends; a link whose circles
// touch is left out, having nowhere to be drawn.
[[nodiscard]] TreeDrawing LayOutTree(const std::vector<PerkTreeNode> &nodes, const std::vector<float> &labelWidth,
                                     float ring, float gap, float lineHeight, float width, float height);

// The part of a node's circle its held ranks fill, as arcs in radians on the
// screen (y down, so a growing angle turns clockwise), starting at the top:
// none for no rank held, a whole turn for every rank, and in between that
// share of one. No arc is over half a turn, so each is a convex slice to
// fill. A node of no ranks fills nothing.
struct Arc
{
    float from{0.0f};
    float to{0.0f};
};
[[nodiscard]] std::vector<Arc> HeldArcs(int held, int ranks);

} // namespace ft
