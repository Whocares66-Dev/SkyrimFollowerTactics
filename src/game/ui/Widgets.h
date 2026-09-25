#pragma once
// The pieces every page of the panel draws with: the palette, text and
// tooltips, glyphs, cells and ticks, popups and cascade menus, chips,
// filters, badges, and the heading and buttons of a detail view.

#include "core/Breakdown.h"
#include "core/I18n.h"
#include "core/Loadout.h"
#include "core/Marks.h"
#include "core/Rows.h"
#include "core/Snapshot.h"
#include "core/Table.h"
#include "core/Views.h"
#include "game/Pins.h"
#include "game/Tactics.h"
#include "game/ui/Panel.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ft::game::ui
{

namespace Im = ImGuiMCP;
using ft::i18n::Tr;
using ft::i18n::TrFormat;

// The one grey for everything set aside: a shadowed row, a banned row, an
// off rule. The theme's disabled text colour, so it follows the theme.
Im::ImU32 DimColor();

// A region that is off: greyed in the same grey as a shadowed row, and
// taking no input. ImGui's own disabled look only fades the text, which
// reads brighter than the rows'; this paints it the rows' grey.
void BeginDimmed(bool dim);

void EndDimmed();

// The same grey over what follows, to the end of the scope, and nothing
// else: unlike BeginDimmed's region, what is drawn stays live. For a row
// set aside, for a cell greyed with its row but still answering, for a
// perk whose conditions fail. One guard, so every greyed-but-live thing
// on the panel is greyed by the same line.
class DimText
{
  public:
    explicit DimText(bool dim) : dim_(dim)
    {
        if (dim_)
            Im::PushStyleColor(Im::ImGuiCol_Text, DimColor());
    }
    ~DimText()
    {
        if (dim_)
            Im::PopStyleColor(1);
    }
    DimText(const DimText &) = delete;
    DimText &operator=(const DimText &) = delete;

  private:
    bool dim_;
};

// Every colour the panel names, in one place: the bars', the names' tints,
// the marks'. The theme's own -- text, disabled text, frames -- are read
// from the style where they are drawn.

// What needs seeing to: a bag past its capacity.
inline constexpr Im::ImVec4 kAlarm{0.95f, 0.45f, 0.40f, 1.0f};
// The character sheet's bars, and the experience beside the level in a
// muted gold rather than a yellow.
inline constexpr Im::ImVec4 kHealth{0.75f, 0.25f, 0.25f, 1.0f};
inline constexpr Im::ImVec4 kStamina{0.30f, 0.65f, 0.35f, 1.0f};
inline constexpr Im::ImVec4 kMagicka{0.25f, 0.40f, 0.80f, 1.0f};
inline constexpr Im::ImVec4 kExperience{0.76f, 0.60f, 0.28f, 1.0f};
// The sheet's status while they fight.
inline constexpr Im::ImVec4 kFighting{0.95f, 0.65f, 0.35f, 1.0f};

// A perk on the follower's own record, chosen for them in advance: its ring
// in amber, of a piece with the gold beside their level; faded while they
// have given it back.
inline constexpr Im::ImVec4 kTheirOwn{0.82f, 0.62f, 0.30f, 1.0f};

// Horizontal breathing room inside every table cell.
//
// Zero here made a full-width button reach the cell border, which is what the
// If/Then cells want -- but it also stripped the left margin off the header
// row and the number column, which is not what those want. So the padding
// stays, and the two button cells opt out of it themselves.
inline constexpr float kCellPadX = 6.0f;
// The gap between an item's name and its first mark, and between marks:
// tighter than a cell's padding, since the marks belong to the name.
inline constexpr float kBadgeGap = 3.0f;
// Vertical padding of the inventory and magic tables' cells.
inline constexpr float kCellPadY = 4.0f;
// A drawer's or a sheet's inner tables sit this far inside their cell.
inline constexpr float kTablePad = 2.0f * kCellPadX + 8.0f;

float WidestLabel(std::initializer_list<const char *> labels);

float TextWidth(const std::string &text);

// Whether the cursor is over the item just drawn, asked by RECT rather
// than by id. ImGui tracks hover by id, and Im::Dummy adds its item with
// none -- it is a spacer that happens to reserve a rect -- so
// IsItemHovered is false over one however the cursor sits. The dead cells
// of a set-aside row are Dummies, drawn dead precisely because they must
// not be clicked, and their tooltips never fired (reported in play,
// 2026-09-20: the reason a row was greyed could not be read anywhere).
bool HoveringLastRect();

// A tooltip, and nothing at all when there is nothing to say. Several of
// the strings that reach a tooltip are optional -- an action whose name
// says it all has no note (`ft::Describe` gives ""), an item nobody set
// aside has no reason -- and ImGui draws the frame whether or not there is
// text in it, so an empty one is a little grey box that follows the cursor
// and says nothing (reported in play, 2026-09-19). Every tooltip whose
// text is a value rather than a literal goes through this.
void Tooltip(std::string_view text);

// A requirement and what is had of it, as every greyed hover says it: the
// two labels right-aligned to one edge, so the skill and the numbers line
// up beneath each other. The one layout for a spell above skill, an effect
// above skill and a perk's requirement.
//
//   Needs: Archery (30)
//     Has: Archery (25)
struct NeedsHasBlock
{
    std::string needs;
    std::string has; // empty: the Needs line alone

    [[nodiscard]] float LabelWidth() const
    {
        return (std::max)(TextWidth(Tr("Needs:")), TextWidth(Tr("Has:")));
    }
    [[nodiscard]] float Width() const
    {
        return LabelWidth() + Im::GetStyle()->ItemSpacing.x + (std::max)(TextWidth(needs), TextWidth(has));
    }
    // At `x` in the window; the first line on the current one when
    // `sameLine`, as a perk's sits beside its name.
    void Draw(float x, bool sameLine = false) const
    {
        const float labels = LabelWidth();
        const auto line = [&](const char *label, const std::string &value) {
            Im::SetCursorPosX(x + labels - TextWidth(label));
            Im::Text("%s", label);
            Im::SameLine(0.0f, Im::GetStyle()->ItemSpacing.x);
            Im::Text("%s", value.c_str());
        };
        if (sameLine)
            Im::SameLine(0.0f, 0.0f);
        line(Tr("Needs:"), needs);
        if (!has.empty())
            line(Tr("Has:"), has);
    }
};

NeedsHasBlock NeedsAndHas(const std::string &skill, int need, std::optional<int> has);

// The block as a hover of its own, under any other reason for the grey.
void NeedsAndHasTooltip(const std::string &skill, int need, int has, const std::string &above = {});

// One row: a labelled bar on the left, a labelled stat on the right.
//
// Both halves are drawn on the SAME ImGui line, and that is the point. Drawn as
// two independent groups they drift apart vertically, because a progress bar is
// frame-height and a line of text is not -- which is exactly what went wrong
// before. Sharing a line makes them share a baseline by construction.
struct RowGeometry
{
    float barLabelRight{0.0f};
    float barLeft{0.0f};
    float barWidth{240.0f};
    float statLabelRight{0.0f};
    float valueLeft{0.0f};
};

// Controls a stat row can carry: its label a click target that opens them,
// lit under the cursor as a link is, and what they draw after the bar.
struct RowControls
{
    bool *open{nullptr};
    const char *toOpen{""};
    const char *toClose{""};
    std::function<void()> draw;
};

void DrawStatRow(const RowGeometry &g, const char *barLabel, const ft::Stat &stat, Im::ImVec4 barColour,
                 const char *statLabel, const std::function<void()> &drawValue, const ft::Breakdown &breakdown = {},
                 const RowControls *controls = nullptr);

// The Dragon Age cascade: subject, then condition, then value.
//
// This needs BeginMenu inside BeginPopup, NOT BeginCombo. Combos are a flat
// list and do not nest -- an earlier version of this editor used three
// side-by-side combos for exactly that reason, which works but reads nothing
// like the game it is copying.
//
// The validity matrix does real work here: a subject only offers predicates it
// can actually answer, so an impossible pair is not merely discouraged, it is
// unreachable. Verdict::InvalidCondition then only happens to a hand-edited
// profile.
// A button filling its table cell, with no border of its own, whose popup opens
// ANCHORED BENEATH IT rather than at the mouse.
//
// Both details matter. The cell already has a border, so a bordered button
// inside it is a box within a box. And an unanchored popup appears detached
// from the thing it belongs to -- it reads as a floating menu rather than as
// this row's condition being edited.
// Every icon on a row -- tick, double tick, plus, cross, the order carets,
// the back arrow -- is a Font Awesome glyph from the solid face the
// framework ships and loads (the inventory's category row proved it
// renders). Strokes through the draw list were tried first and read jagged
// beside ImGui's own checkbox tick; a font glyph is hinted and anti-aliased
// for free, and one em size keeps every icon on a row the same size.
enum class Glyph
{
    Cross,
    Plus,
    Minus,
    AllTheWayLeft,
    AllTheWayRight,
    Tick,
    Pin,
    Ban,
    CaretRight,
    Up,
    Down,
    Back,
    Eye,
    EyeSlash
};

// A codepoint as UTF-8. Every Font Awesome icon sits in the U+F000 block, so
// the three-byte form is the only case.
std::string Utf8(unsigned codepoint);

// The eye and the struck-through eye are wider than their em (1.125 and
// 1.25 of it), so at full size they fill a square button that the cross
// and the arrows sit inside with room to spare.
inline constexpr float kWideGlyphScale = 0.8f;

void DrawGlyph(Im::ImDrawList *draw, Glyph glyph, Im::ImVec2 lo, Im::ImVec2 hi, Im::ImU32 ink, float scale = 1.0f);

// A square button with the glyph painted over it, centred on the button's
// own rect the way the On cell's tick is. Not as the button's label: ImGui
// centres a label only when it fits inside the frame padding, and the icon
// font's glyph is taller than the text font's line, so the plus sat up and
// to the left. The text colour carries the disabled dimming.
// `painted` false leaves the square empty: a switch that is off shows no
// tick, as the rule rows' On cells do, rather than a ghost of one.
bool GlyphButton(const std::string &id, float size, Glyph glyph, bool painted = true, float scale = 1.0f);

bool DeleteButton(const std::string &id, float size);

// Chrome for the cascade popups, shared by the condition and action menus.
//
// A thin border on every level. The root popup takes PopupBorderSize, but a
// menu opened from inside a menu is flagged as a CHILD window by ImGui
// (BeginMenuEx adds ImGuiWindowFlags_ChildWindow when its parent is itself a
// child menu) and takes ChildBorderSize instead -- which is why thinning only
// the popup border left the deeper levels at the theme's 3 px. Both are
// pushed, to the same value.
//
// ImGui places a submenu overlapping its parent, and the amount is
// ItemInnerSpacing.x -- not ItemSpacing, whatever the comment in imgui.cpp
// says it is (FindBestWindowPosForPopup reads ItemInnerSpacing). Setting it
// to the border width makes the child's left border land exactly on the
// parent's right border, so the two boxes share one line instead of
// colliding a few pixels apart.
//
// Pushed BEFORE BeginPopup: a popup takes its padding and border when the
// window is created, not while it is being filled. Popped on every path out,
// the closed one included.
void PushPopupChrome();

inline constexpr int kPopupChromeVars = 4;

// Returns where the popup opens, the cell's bottom-left, for the caller to
// set just before BeginPopup: set here, it would place whatever window opens
// next instead, and a tooltip raised on the cell is one.
Im::ImVec2 CellButtonOpensPopup(const char *id, const std::string &label, bool *elided = nullptr);

// `tooltip` belongs to the HEADING, and is read here rather than by the
// caller: with the submenu open the last item is the popup's, not this
// entry's, so a hover test after the call answers about the wrong thing.
bool BeginCascade(const char *label, const char *tooltip = nullptr);

// `textColour` tints the label and nothing else: the tick at the right is
// the menu's, not the item's, and keeps the plain colour.
bool CascadeItem(const char *label, bool selected, const Im::ImVec4 *textColour = nullptr);

// A header row that is only a header. TableHeadersRow() draws each heading
// as a widget: it lights up under the mouse and opens a column menu on a
// right click, both of which promise something the sheet does not offer. A
// row flagged as a header gets the header background; plain text on it gets
// the look without the behaviour.
void PlainHeaderRow(const std::vector<const char *> &labels);

// The open/closed marker before a skill that has perks: a small triangle,
// pointing right when closed and down when open, drawn rather than typed
// for the reason DeleteButton gives. Drawn at `pos`, the top-left of the
// text line it sits beside, and vertically centred on that line.
float DisclosureWidth();

void DrawDisclosure(Im::ImVec2 pos, bool open);

// The colour an item's name is drawn in wherever it appears -- the
// Inventory tab's rows and pages, the rule pickers' leaves -- so two rows
// of one name read apart the same way everywhere: gold for a Daedric
// artifact, the enchanted tint for an enchanted copy, null for the plain
// text colour. Two copies alike in name and colour are told apart by
// their order alone, which is stable.
const Im::ImVec4 *NameTint(const InventoryItem &item);

// The marks after an item's name, wherever the name is drawn: a crown for a
// Daedric artifact, else a bolt for an enchanted piece (an artifact is
// always enchanted, and the crown says so), then a skull for a poison on
// it, then a hand for a stolen copy. Each in its own colour -- the crown and
// the bolt the name's tint, the skull green, the hand red -- or the row's disabled colour when the row is dimmed, or
// the marks would light up a greyed row. Drawn at kPinScale like the pin: a
// font glyph fills its em and reads too big beside text at full size.
// Returns the width drawn, so a caller laying the marks out by hand (a menu
// leaf) can advance past them.
bool Badged(const InventoryItem &item);

// With no draw list, measures only: the width the marks would take.
float DrawNameBadges(Im::ImDrawList *draw, const InventoryItem &item, Im::ImVec2 at, bool dim);

// The marks after the name just drawn, on the same line. `framed` for a
// name drawn with AlignTextToFramePadding beside a button, as the item
// page's heading is: the text sits a frame padding below the line's top,
// and the marks go down with it, or they ride high beside it.
void NameBadges(const InventoryItem &item, bool dim, bool framed = false);

// Ban every row the filter leaves, or unban them when every one is banned
// already: search for what to set aside, then one click. The ban sign,
// lit while all of them are banned -- in the theme's colour for a thing
// selected, as an open drawer and a chosen menu item are, where the
// button-pressed colour was left at ImGui's own blue. Nothing to do with
// no rows.
struct BanAll
{
    // The rows the filter leaves, as the game side takes them, and whether
    // each is banned. Read when drawn, so it answers this frame's text.
    std::function<std::vector<std::pair<WearTarget, bool>>()> rows;
    ft::ActorId who{0};
};

void BanAllButton(const char *id, const BanAll &banAll);

// A list's filter box, the list's own buttons beside it -- the ban-all,
// the hidden effects' switch -- and on its line against the right edge how
// many of the list's rows the filter leaves: "12 items", "3 of 12 items" --
// `noun` translated by the caller. Above the table, not under it, where a
// long list pushed the count out of sight; counted after the box, so the
// number answers this frame's text.
void FilterRow(const char *id, char *buffer, std::size_t size, const std::function<std::size_t()> &shown,
               std::size_t total, const char *noun, const std::function<void()> &beside = {});

// Text flush with the right edge of the current table cell, for a column of
// numbers: the ones and the tens then line up.
void TextRightInCell(const std::string &text);

// A sheet row's hover text, a line at a time. Plain text: the notes are
// prose, and a table that split each line at its colon set "- Higher:" apart
// from the rest of its sentence (2026-09-15). Numbers with sources have
// their own tooltip, the breakdown's.
void NoteTooltip(const std::string &note);

// A number written out as the calculation that made it: the lines in
// two columns, the amounts right-aligned, a rule, then the total -- the
// same shape everywhere a value has sources, so the eye can check the
// arithmetic. A line's detail sits indented beneath it.
void BreakdownTooltip(const ft::Breakdown &b);

// An invisible, cell-filling click target, lit through the cell background
// while hovered for the reason DrawSections gives. Draws nothing itself: the
// caller puts the cursor back and draws the cell's content over it.
bool CellClicked(const char *id, float height = 0.0f);

void CentredHeading(const char *title);

// A row of chips: a strip of small tabs, each an icon and a word, flowing
// onto a second line when the panel is narrow. Chips are
// selectables with no label of their own; the icon and the word are painted
// over them through the draw list, so the layout cursor stays on the chip's
// full width and the next one lands beside it, not beside the text.
struct Chip
{
    std::string label;
    unsigned icon; // Font Awesome codepoint
    int id;
};

void DrawChips(const std::vector<Chip> &chips, int &selected);

// The category strip a list draws above its table: All, then every category
// this page has something in. Empty categories are left out -- a tab
// promising nothing is noise -- and a category with nothing in it *here*,
// the last potion drunk or a follower with no keys, falls back to All for
// this page while the choice itself stands for the pages that do have it.
// Returns the category to draw by: the one picked, or -1 for All.
//
// `name` and `icon` word a chip for a category's index. They are passed
// rather than called by name because the two callers' categories are
// different enums with their own overloads, and one of those overloads is
// declared further down this file than this template.
template <std::size_t N, typename Name, typename Icon>
int DrawCategoryChips(const std::array<int, N> &counts, unsigned allIcon, ListView &shared, Name name, Icon icon)
{
    std::vector<Chip> chips{{Tr("All"), allIcon, -1}};
    for (std::size_t i = 0; i < counts.size(); ++i)
    {
        if (counts[i] == 0)
            continue;
        chips.push_back({name(i), icon(i), static_cast<int>(i)});
    }

    const int picked =
        shared.category >= 0 && counts[static_cast<std::size_t>(shared.category)] > 0 ? shared.category : -1;
    int chosen = picked;
    DrawChips(chips, chosen);
    if (chosen != picked)
        shared.category = chosen;
    return chosen;
}

// The inventory table's columns, by id rather than by position: which of
// them a list shows depends on the category, and a sort spec names the
// column by this id.
// The columns, and the order each list takes in one, are core's
// (core/Rows.h, tested); the table's header carries the column's number
// and the direction.
using ft::Column;

// A cell that says whether something is on -- in a hand, or worn -- with a
// tick, a pin beside it if we are keeping it there, and, when clickable, a
// click that rounds the three states: off -> kept on -> theirs to change ->
// off. The request goes to the game thread and the cell answers when the
// view comes back.
// A diagonal across the current cell, corner to corner: this cell does not
// apply -- a right hand for a shield, a hand for a cuirass. The corners are
// the table's own: the rectangle it fills a cell's background in, from the
// column's borders and the row's. Not rebuilt from the cursor, the text
// height and a padding constant: the rule table's rows are a frame high
// and padded by 2, and that guess ran the slash short of both corners.
//
// The rectangle's bottom is where the row has got to so far, so call this
// once the cells that set the row's height are drawn. The equip cells come
// after a row's text; the rule table's switch is its first column, and the
// row comes back to it at the end.
void SlashCell();

// One equip cell's state: what the cell draws, and what its column sorts
// by. The two read the SAME struct, from the same reader, so a cell cannot
// sort otherwise than it looks -- a spell above the follower's skill was
// drawn slashed and sorted among the unequipped once, the sort re-deriving
// the state on its own and reading less of it (2026-09-10). The cell is
// core's (EquipCell, core/Marks.h), with its rank and its next request.

// The readers are core's (core/Rows.h, tested): a cell of a row, and
// whether the row is dim.
using ft::Dimmed;
using ft::LeftCell;
using ft::RightCell;
using ft::VoiceCell;
using ft::VoiceEntry;
using ft::WornCell;

// A click walks the cell round: unequipped, equipped, pinned, banned, and
// back to unequipped. Each state is one request to the game thread.
void OnCell(const char *id, const CharacterView &view, std::uint32_t form, const EquipCell &cell, Hand hand,
            bool clickable, const std::optional<ft::ItemVariant> &variant = std::nullopt, const void *row = nullptr);

// Put the rows in the order the table's header asks for: the column and
// the direction are read off the table's sort spec, the order itself is
// core's (core/Table.h, SortRows, tested). A table whose header has not
// been clicked yet has no spec, and its rows are left in the order they
// came.
template <typename Row, typename Compared> void SortRows(std::vector<const Row *> &rows, const Compared &compare)
{
    const auto *specs = Im::TableGetSortSpecs();
    if (!specs || specs->SpecsCount < 1 || !specs->Specs)
        return;
    const auto &spec = specs->Specs[0];
    ft::SortRows(rows, compare, static_cast<Column>(spec.ColumnUserID),
                 spec.SortDirection != Im::ImGuiSortDirection_Descending);
}

// After an action on the follower whose page is shown -- a perk learned, a
// level moved, a spell learned or forgotten -- the page is rebuilt once it
// is done: queued behind it on the game thread, where tasks run in order.
// The page is not rebuilt on a beat (Tactics.h, RefreshShownPage), so
// without this the change showed only after the panel was closed and
// opened.
void RefreshAfterAction();

// The game's own sounds, by their descriptors' editor ids (Skyrim.esm):
// the perk menu's for a perk taken (the engine plays it from 52521), the
// skills menu's step back for one given back, the one the game plays for
// a spell learned (0ECF93), the enchanting table's for an item
// disenchanted (0C8C76) -- magic taken apart -- for a spell forgotten, and
// the one a failed activation makes for a click that cannot be answered.
inline constexpr const char *kPerkTakenSound = "UISkillsPerkSelect2D";
inline constexpr const char *kPerkReturnedSound = "UISkillsBackwardSD";
inline constexpr const char *kSpellLearnedSound = "UISpellLearned";
inline constexpr const char *kSpellForgottenSound = "UIEnchantingItemDestroy";
inline constexpr const char *kRefusedSound = "UIActivateFail";

// Played on the game thread, where the descriptor is looked up.
void PlayGameSound(const char *id);

// An action at the right of a page's head that asks once -- Reset perks,
// Learn, Forget -- as a button, then Confirm and Cancel in its place. It is
// greyed where it cannot act, and its hover, on the button and on Confirm
// alike, says what a click does or why it cannot. `asking` is the page's
// own. True the frame Confirm is clicked.
float AskedActionWidth(const char *label, bool asking);

bool AskedAction(const char *label, bool can, const std::string &hover, bool &asking);

// The same, on the line just drawn, at its right edge: `lineRight` is where
// the line ends, taken at its start.
bool AskedActionAtRight(const char *label, bool can, const std::string &hover, bool &asking, float lineRight);

// An action at the right of the line just drawn that asks nothing -- Charge
// -- greyed where it cannot act, its hover saying what a click does or why
// it cannot. True the frame it is clicked.
bool ActionAtRight(const char *label, bool can, const char *hover, float lineRight);

// The head of a detail page, which every tab that has one draws the same
// way: a back arrow, borderless as the panel's other glyph buttons, the
// name beside it, and a word after the name where the page has one.
//
// The arrow only reports the click. Where it goes back TO is the tab's own
// business -- the list, the sheet it was opened from, or a whole state
// cleared -- and no two of them answer that the same way.
bool BackButton();

// `tint` colours the name where the row's own colour says something about
// it, and is null for a name drawn plainly.
void DetailName(const std::string &name, const Im::ImVec4 *tint = nullptr);

// The word after the name: what kind of thing the page is about.
void DetailSubtitle(const std::string &text);

} // namespace ft::game::ui
