#pragma once
// The sheet's tables: a page's sections of rows, and the perk and
// condition tables in a row's drawer.

#include "core/I18n.h"
#include "core/Views.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ft::game::ui
{

// The drawer an open skill row reveals: its perks, name and description,
// set in from both edges of the parent table and given air above and below.
// A table of perks: Perk, Rank, Description, the name a link to the perk's
// page. The drawer under a skill and the Other section are both this.
void DrawPerkTable(const std::string &id, const std::vector<SheetRow> &perks, float width,
                   const std::function<void(std::uint32_t)> &onLink);

void DrawConditionDrawer(const SheetRow &row, const std::string &key, float left, float right);

// A run of headed sections, each a bordered table in the style of the rule
// table. The name and value columns are FIXED, measured across every section
// in the run so the tables line up down the page, and sized to their
// contents so a value sits beside its name rather than at the far edge of
// the panel. A Modifiers column, when asked for, takes the rest. Plain
// headings rather than collapsing ones: nothing here is long enough to want
// hiding, and a heading that can be clicked invites clicking to see what
// happens. The wording was done on the game thread; this only lays it out.
//
// No header row for the name and value columns. What they are is obvious
// from a glance at any row, and a label saying so is one more line of chrome
// per table. The Modifiers column keeps its heading because that one is not
// obvious.
//
// A row with perks opens like a drawer, and ImGui has no such thing: a table
// is a grid with no spanning. So the table is drawn in PIECES. It is closed
// at an open row, the drawer is drawn beneath, and a fresh table with the
// same column widths is opened for the rows that follow. Three things make
// the seam invisible: no vertical spacing between pieces, so the bottom
// border of one is the row line above the drawer and the top border of the
// next is the row line below it; the striping is counted across pieces
// rather than restarted by each; and the outer left and right borders are
// drawn by hand across the drawer's height, so the frame is continuous.
// What an open row reveals, given the row, its key and the parent table's
// edges. The perk drawer by default.
using RowDrawer = std::function<void(const SheetRow &, const std::string &, float, float)>;

// A column a table may carry after the value: its heading, and each row's
// text in it. Left out of the table altogether when every row's text is
// empty -- a Remaining column on a page of effects that never end says
// nothing. `glyph`: the text is only whether to draw the tick. `link`:
// the cell is a link to the row's form, where the table has an onLink.
struct ExtraColumn
{
    const char *heading{""};
    std::function<std::string(const SheetRow &)> text;
    bool glyph{false};
    bool link{false};
    bool wrap{false}; // prose: wrapped to the column, which takes the rest of the table
};

// The columns an effect table carries after Name and Effect. Each drawn
// only where some row has it: School and Level only where an effect has a
// school, and a level above none. An item's or a spell's page ends with the
// effects' descriptions, wrapped; the effect's own page ends with Source.
inline const std::vector<ExtraColumn> kEffectColumns{
    {N_("School"), [](const SheetRow &r) { return r.school; }},
    {N_("Level"), [](const SheetRow &r) { return r.level; }},
    {N_("Remaining"), [](const SheetRow &r) { return r.remaining; }},
    {N_("Duration"), [](const SheetRow &r) { return r.extra; }},
    {N_("Hidden"), [](const SheetRow &r) { return std::string(r.mark != 0 ? "x" : ""); }, true},
};

std::vector<ExtraColumn> WithDescription();

// `modifiers`: a third column, headed `third`, carrying each row's
// modifiers text or its mark glyph -- none at all when `third` is null;
// `first` and `second` head the name and value columns then, where the
// table has a header row at all; and `wanted` are the columns after it,
// of which those with anything in them are drawn, the last column taking
// the rest of the table. Every heading comes in English and is translated
// here.
void DrawSections(const std::vector<SheetSection> &sections, bool modifiers,
                  const std::function<void(std::uint32_t)> &onLink = {}, const char *third = N_("Modifiers"),
                  const RowDrawer &drawer = {}, const char *first = "", const char *second = "",
                  const std::vector<ExtraColumn> &wanted = {},
                  const std::function<void(const SheetRow &)> &onTree = {});

} // namespace ft::game::ui
