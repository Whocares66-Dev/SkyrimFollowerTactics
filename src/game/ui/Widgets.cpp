// The pieces every page draws with (Widgets.h).

#include "game/ui/Widgets.h"
#include "game/ui/Panel.h"

#include "core/Breakdown.h"
#include "game/Pins.h"
#include "game/Tactics.h"
#include <SKSEMenuFramework.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ft::game::ui
{
namespace
{

// An enchanted piece's name, and its bolt.
constexpr Im::ImVec4 kEnchanted{0.70f, 0.75f, 1.00f, 1.0f};
// A spell tome's name: magic, as an enchanted piece's is.
constexpr Im::ImVec4 kSpellTome = kEnchanted;

// A Daedric artifact: light gold, over the enchanted blue; every artifact
// is enchanted, and the colour says which kind of enchanted it is. Red was
// tried on 2026-09-12 and read as a warning.
constexpr Im::ImVec4 kArtifact{0.95f, 0.85f, 0.55f, 1.0f};
// A poison on a weapon: green, as the bottle is.
constexpr Im::ImVec4 kPoison{0.55f, 0.85f, 0.45f, 1.0f};
// A stolen copy: red.
constexpr Im::ImVec4 kStolen{0.90f, 0.35f, 0.30f, 1.0f};

// The text as much of it as fits, with "..." where it does not. Plain
// ASCII dots rather than the ellipsis character: what is in the font atlas
// is whatever the panel loaded, and a glyph that is not there draws as a
// box, which is worse than the thing it replaces.
//
// Whole code points only. The strings here carry an effect's name as the
// game shows it, which for a translated or a mod's effect need not be
// ASCII, and a byte taken from the middle of one draws as a replacement
// glyph.
std::string Elide(const std::string &text, float width)
{
    if (width <= 0.0f || TextWidth(text) <= width)
        return text;
    std::string tail = "..."; // not const: it is returned, and would not move
    const float tailWidth = TextWidth(tail);
    std::size_t end = text.size();
    while (end > 0)
    {
        --end;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            --end;
        if (TextWidth(text.substr(0, end)) + tailWidth <= width)
            return text.substr(0, end) + tail;
    }
    return tail;
}

// Place text so its RIGHT edge lands on rightX. Right-aligning the labels is
// what makes a label column read as a column: left-aligned, the gap between
// each label and the thing it names is a different width on every row.
void TextRightAlignedAt(float rightX, const std::string &text)
{
    Im::SameLine((std::max)(0.0f, rightX - TextWidth(text)), -1.0f);
    Im::AlignTextToFramePadding();
    Im::Text("%s", text.c_str());
}

unsigned Codepoint(Glyph glyph)
{
    switch (glyph)
    {
    case Glyph::Cross:
        return 0xF00D; // xmark
    case Glyph::Plus:
        return 0xF067; // plus
    case Glyph::Minus:
        return 0xF068; // minus
    case Glyph::AllTheWayLeft:
        return 0xF100; // angles-left
    case Glyph::AllTheWayRight:
        return 0xF101; // angles-right
    case Glyph::Tick:
        return 0xF00C; // check
    case Glyph::Pin:
        return 0xF08D; // thumbtack
    case Glyph::Ban:
        return 0xF05E; // ban: a circle with a bar
    case Glyph::CaretRight:
        return 0xF0DA; // caret-right
    case Glyph::Up:
        return 0xF062; // arrow-up: a move, not a sort direction
    case Glyph::Down:
        return 0xF063; // arrow-down
    case Glyph::Eye:
        return 0xF06E; // eye
    case Glyph::EyeSlash:
        return 0xF070; // eye-slash
    case Glyph::Back:
    default:
        return 0xF060; // arrow-left
    }
}

// A codepoint centred in the box [lo, hi], through the draw list, for a cell
// that is not a button. `scale` is the glyph's size as a fraction of the
// font's: a font glyph fills its whole em where a letter uses two thirds of
// it, so at full size a tall glyph like the pin touches the edges of a text
// row. The pin draws at kPinScale for a margin; the tick is a short glyph
// and fits at 1, as do buttons, whose frame is taller than the glyph.
constexpr float kPinScale = 0.75f;

// Where a glyph's ink lies within its em, at that scale, relative to the
// pen: the font's own glyph rectangle. A glyph is centred in its em by its
// advance and line height, and the ink of a crown sits high in the em where
// a bolt fills it; a mark laid beside text is centred on its ink instead,
// or the crown rides above the line the bolt sits on. Zero when the font
// has no such glyph.
struct InkBox
{
    float x0{0.0f}, y0{0.0f}, x1{0.0f}, y1{0.0f};
    [[nodiscard]] bool Empty() const noexcept
    {
        return x1 <= x0 || y1 <= y0;
    }
};

InkBox InkOf(unsigned codepoint, float scale)
{
    FontAwesome::PushSolid();
    Im::ImFont *font = Im::GetFont();
    const float s = font && font->FontSize > 0.0f ? Im::GetFontSize() * scale / font->FontSize : 0.0f;
    const Im::ImFontGlyph *glyph =
        font && codepoint <= 0xFFFF ? Im::ImFontManger::FindGlyph(font, static_cast<Im::ImWchar>(codepoint)) : nullptr;
    FontAwesome::Pop();
    if (!glyph || s <= 0.0f)
        return {};
    return {glyph->X0 * s, glyph->Y0 * s, glyph->X1 * s, glyph->Y1 * s};
}

// `onInk` centres the glyph's ink in the box rather than its em: for a mark
// beside text. The buttons and cells keep the em, where every glyph of a
// kind lands the same and a tick and a cross line up.
void DrawCodepoint(Im::ImDrawList *draw, unsigned codepoint, Im::ImVec2 lo, Im::ImVec2 hi, Im::ImU32 ink,
                   float scale = 1.0f, bool onInk = false)
{
    const std::string text = Utf8(codepoint);
    FontAwesome::PushSolid();
    const Im::ImFont *font = Im::GetFont();
    const float fontSize = Im::GetFontSize() * scale;
    Im::ImVec2 extent = Im::CalcTextSize(text.c_str(), nullptr, false, -1.0f);
    FontAwesome::Pop();
    if (!font)
        return;
    extent.x *= scale;
    extent.y *= scale;
    Im::ImVec2 at{(lo.x + hi.x - extent.x) * 0.5f, (lo.y + hi.y - extent.y) * 0.5f};
    if (const InkBox box = onInk ? InkOf(codepoint, scale) : InkBox{}; !box.Empty())
        at = {(lo.x + hi.x) * 0.5f - (box.x0 + box.x1) * 0.5f, (lo.y + hi.y) * 0.5f - (box.y0 + box.y1) * 0.5f};
    Im::ImDrawListManager::AddText(draw, font, fontSize, at, ink, text.c_str());
}

// The width a codepoint's ink takes at that scale: what a mark laid beside
// text needs, where a box the font's em wide would leave a margin either
// side of a narrow glyph like the bolt. The advance, for a glyph the font
// does not have.
float CodepointWidth(unsigned codepoint, float scale)
{
    if (const InkBox box = InkOf(codepoint, scale); !box.Empty())
        return box.x1 - box.x0;
    const std::string text = Utf8(codepoint);
    FontAwesome::PushSolid();
    const float width = Im::CalcTextSize(text.c_str(), nullptr, false, -1.0f).x;
    FontAwesome::Pop();
    return width * scale;
}

// One row of a cascade, drawn with the icon font's glyphs.
//
// ImGui draws a submenu's arrow and a selected item's check in its own
// strokes, and those are the two marks on this panel that were not from the
// icon font. There is no flag to turn them off, so: a submenu row is opened
// with its label pushed transparent -- ImGui still measures it and still
// draws its arrow, invisibly -- and the label and a caret are drawn over it
// through the parent's draw list; a leaf row is opened unselected, so ImGui
// draws no check, and a check is drawn where it would have been. The rect
// comes from the cursor and the window, not from the item: once a submenu
// opens, "the last item" is the submenu window.
float CascadeIconRight()
{
    const auto *style = Im::GetStyle();
    return Im::GetWindowPos().x + Im::GetWindowWidth() - (style->WindowPadding.x);
}

void CascadeIcon(Im::ImDrawList *draw, Glyph glyph, Im::ImVec2 rowPos, float right)
{
    if (!draw)
        return;
    const float h = Im::GetTextLineHeight();
    const float w = Im::GetFontSize();
    DrawGlyph(draw, glyph, {right - w, rowPos.y}, {right, rowPos.y + h}, Im::GetColorU32(Im::ImGuiCol_Text, 1.0f));
}

// The filter the list tabs share: a box with "Filter" for its hint, and a
// cross inside its right end to clear it, shown only while there is
// something to clear. Returns whether the text changed.
bool FilterBox(const char *id, char *buffer, std::size_t size)
{
    const float width = Im::GetFontSize() * 9.0f;
    Im::SetNextItemWidth(width);
    // The cross is drawn over the box's right end, and ImGui gives the
    // hover to the item drawn first unless it allows overlap: without this
    // the cross could be seen but never clicked.
    Im::SetNextItemAllowOverlap();
    if (g_focusFilter)
        Im::SetKeyboardFocusHere();
    g_focusFilter = false;
    bool changed = Im::InputTextWithHint(id, Tr("Filter"), buffer, size);
    // One box a frame, so this frame's answer is the whole answer.
    g_filterDrawn.store(true, std::memory_order_relaxed);
    g_filterActive.store(Im::IsItemActive(), std::memory_order_relaxed);
    // Escape emptied the box as well as leaving it. AFTER the box is drawn,
    // never before: a box the cursor leaves having been TYPED IN writes the
    // text it was holding back over the buffer on that frame (ImGui parks it
    // in InputTextDeactivatedState and applies it when the box reads as
    // deactivated-after-edit), so a buffer emptied ahead of the call is
    // filled again by the call itself. Emptied before, Escape cleared a box
    // the player had only put the cursor in and left one they had typed in
    // -- the edit is the whole difference (2026-09-20). The cross below
    // clears from here, and works, for the same reason. The box's pixels are
    // one frame behind on the clearing frame; the rows below are not.
    if (g_clearFilter)
    {
        g_clearFilter = false;
        buffer[0] = '\0';
        changed = true;
    }
    if (buffer[0] == '\0')
        return changed;

    // The cross sits over the box's right end: a square button the box's
    // height, painted invisible, the glyph drawn over it.
    const Im::ImVec2 lo = Im::GetItemRectMin();
    const Im::ImVec2 hi = Im::GetItemRectMax();
    const float side = hi.y - lo.y;
    const Im::ImVec2 keep = Im::GetCursorScreenPos();
    Im::SetCursorScreenPos(Im::ImVec2(hi.x - side, lo.y));
    const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
    Im::PushStyleColor(Im::ImGuiCol_Button, invisible);
    Im::PushStyleColor(Im::ImGuiCol_ButtonHovered, invisible);
    Im::PushStyleColor(Im::ImGuiCol_ButtonActive, invisible);
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (GlyphButton(std::string(id) + "clear", side, Glyph::Cross))
    {
        buffer[0] = '\0';
        changed = true;
    }
    Im::PopStyleVar(1);
    Im::PopStyleColor(3);
    if (Im::IsItemHovered(0))
        Im::SetTooltip("%s", Tr("Clear the filter"));
    Im::SetCursorScreenPos(keep);
    return changed;
}

// The Worn column's glyphs, centred in the cell whose top-left is `pos` and
// drawn over whatever the cell already laid out. The tick for equipped,
// the pin beside it for pinned, in the word the panel uses for it:
// equipped, and kept so. A pin without a tick is a pin the AI is fighting:
// the thing is promised to the hand but not in it this instant (the sword
// the AI drew over a pinned bow, 14:48), and the pin must not read as
// cleared. Banned is the ban sign alone: off, and kept off.
void DrawTickAt(Im::ImVec2 pos, Im::ImU32 ink, bool on, bool pinned, bool banned)
{
    auto *draw = Im::GetWindowDrawList();
    if (!draw || (!on && !pinned && !banned))
        return;
    // Boxes the height of the text line the row was laid out with, and a
    // glyph's width each, so the pair sits centred with the row's own margin
    // above and below.
    const float h = Im::GetTextLineHeight();
    const float box = Im::GetFontSize();
    const float cell = Im::GetContentRegionAvail().x;
    if (banned)
    {
        const float left = pos.x + (std::max)(0.0f, (cell - box) * 0.5f);
        DrawGlyph(draw, Glyph::Ban, {left, pos.y}, {left + box, pos.y + h}, ink, kPinScale);
        return;
    }
    const float width = on && pinned ? 2.0f * box : box;
    float left = pos.x + (std::max)(0.0f, (cell - width) * 0.5f);
    if (on)
    {
        DrawGlyph(draw, Glyph::Tick, {left, pos.y}, {left + box, pos.y + h}, ink);
        left += box;
    }
    if (pinned)
        DrawGlyph(draw, Glyph::Pin, {left, pos.y}, {left + box, pos.y + h}, ink, kPinScale);
}

} // namespace

Im::ImU32 DimColor()
{
    return Im::GetColorU32(Im::ImGuiCol_TextDisabled, 1.0f);
}

void BeginDimmed(bool dim)
{
    Im::PushStyleVar(Im::ImGuiStyleVar_DisabledAlpha, 1.0f);
    Im::PushStyleColor(Im::ImGuiCol_Text, dim ? DimColor() : Im::GetColorU32(Im::ImGuiCol_Text, 1.0f));
    Im::BeginDisabled(dim);
}

void EndDimmed()
{
    Im::EndDisabled();
    Im::PopStyleColor(1);
    Im::PopStyleVar(1);
}

float WidestLabel(std::initializer_list<const char *> labels)
{
    float widest = 0.0f;
    for (const char *label : labels)
        widest = (std::max)(widest, Im::CalcTextSize(label, nullptr, false, -1.0f).x);
    return widest;
}

float TextWidth(const std::string &text)
{
    return Im::CalcTextSize(text.c_str(), nullptr, false, -1.0f).x;
}

bool HoveringLastRect()
{
    return Im::IsMouseHoveringRect(Im::GetItemRectMin(), Im::GetItemRectMax(), true);
}

void Tooltip(std::string_view text)
{
    if (!text.empty())
        Im::SetTooltip("%s", std::string(text).c_str());
}

NeedsHasBlock NeedsAndHas(const std::string &skill, int need, std::optional<int> has)
{
    return {fmt::format("{} ({})", skill, need), has ? fmt::format("{} ({})", skill, *has) : std::string()};
}

void NeedsAndHasTooltip(const std::string &skill, int need, int has, const std::string &above)
{
    Im::BeginTooltip();
    if (!above.empty())
        Im::Text("%s", above.c_str());
    NeedsAndHas(skill, need, has).Draw(Im::GetCursorPosX());
    Im::EndTooltip();
}

void DrawStatRow(const RowGeometry &g, const char *barLabel, const ft::Stat &stat, Im::ImVec4 barColour,
                 const char *statLabel, const std::function<void()> &drawValue, const ft::Breakdown &breakdown,
                 const RowControls *controls)
{
    Im::SetCursorPosX((std::max)(0.0f, g.barLabelRight - Im::CalcTextSize(barLabel).x));
    Im::AlignTextToFramePadding();
    if (controls && controls->open)
    {
        const Im::ImVec2 at = Im::GetCursorScreenPos();
        const Im::ImVec2 size(Im::CalcTextSize(barLabel).x, Im::GetFrameHeight());
        Im::PushID(barLabel);
        if (Im::InvisibleButton("label", size, 0))
            *controls->open = !*controls->open;
        Im::PopID();
        if (Im::IsItemHovered(0))
        {
            if (auto *draw = Im::GetWindowDrawList())
                Im::ImDrawListManager::AddRectFilled(draw, Im::ImVec2(at.x - 3.0f, at.y),
                                                     Im::ImVec2(at.x + size.x + 3.0f, at.y + size.y),
                                                     Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f), 3.0f, 0);
            Tooltip(*controls->open ? controls->toClose : controls->toOpen);
        }
        Im::SetCursorScreenPos(at);
        Im::AlignTextToFramePadding();
    }
    Im::Text("%s", barLabel);

    Im::SameLine(g.barLeft, -1.0f);
    Im::PushStyleColor(Im::ImGuiCol_PlotHistogram, barColour);
    // The numbers centred over the bar, drawn by hand: ImGui's own overlay
    // sits just past the filled part, so it moved with the fill and the
    // three bars' numbers did not line up.
    Im::ProgressBar(stat.Pct(), Im::ImVec2(g.barWidth, 0.0f), "");
    Im::PopStyleColor(1);
    // Where the maximum comes from, on the bar, beside the number.
    if (!breakdown.empty() && Im::IsItemHovered(0))
        BreakdownTooltip(breakdown);
    const std::string overlay =
        std::to_string(static_cast<int>(stat.current)) + " / " + std::to_string(static_cast<int>(stat.max));
    if (auto *draw = Im::GetWindowDrawList())
    {
        const Im::ImVec2 lo = Im::GetItemRectMin();
        const Im::ImVec2 hi = Im::GetItemRectMax();
        const Im::ImVec2 size = Im::CalcTextSize(overlay.c_str());
        Im::ImDrawListManager::AddText(draw,
                                       {lo.x + (hi.x - lo.x - size.x) * 0.5f, lo.y + (hi.y - lo.y - size.y) * 0.5f},
                                       Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), overlay.c_str());
    }
    if (controls && controls->open && *controls->open && controls->draw)
    {
        Im::SameLine(0.0f, kCellPadX);
        controls->draw();
    }

    // A row with nothing to say on the right leaves it empty.
    if (!statLabel)
        return;
    TextRightAlignedAt(g.statLabelRight, statLabel);

    Im::SameLine(g.valueLeft, -1.0f);
    Im::AlignTextToFramePadding();
    drawValue();
}

std::string Utf8(unsigned codepoint)
{
    return {static_cast<char>(0xE0 | (codepoint >> 12)), static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)),
            static_cast<char>(0x80 | (codepoint & 0x3F))};
}

void DrawGlyph(Im::ImDrawList *draw, Glyph glyph, Im::ImVec2 lo, Im::ImVec2 hi, Im::ImU32 ink, float scale)
{
    DrawCodepoint(draw, Codepoint(glyph), lo, hi, ink, scale);
}

bool GlyphButton(const std::string &id, float size, Glyph glyph, bool painted, float scale)
{
    const bool clicked = Im::Button(("##" + id).c_str(), Im::ImVec2(size, size));
    if (auto *draw = Im::GetWindowDrawList(); draw && painted)
        DrawGlyph(draw, glyph, Im::GetItemRectMin(), Im::GetItemRectMax(), Im::GetColorU32(Im::ImGuiCol_Text, 1.0f),
                  scale);
    return clicked;
}

bool DeleteButton(const std::string &id, float size)
{
    return GlyphButton(id, size, Glyph::Cross);
}

void PushPopupChrome()
{
    constexpr float border = 1.0f;
    const auto *style = Im::GetStyle();
    const float innerY = style->ItemInnerSpacing.y;
    Im::PushStyleVar(Im::ImGuiStyleVar_PopupBorderSize, border);
    Im::PushStyleVar(Im::ImGuiStyleVar_ChildBorderSize, border);
    Im::PushStyleVar(Im::ImGuiStyleVar_ItemInnerSpacing, Im::ImVec2(border, innerY));
    Im::PushStyleVar(Im::ImGuiStyleVar_ItemSpacing, Im::ImVec2(6.0f, 4.0f));
}

Im::ImVec2 CellButtonOpensPopup(const char *id, const std::string &label, bool *elided)
{
    // The highlight is the TABLE's, not the button's.
    //
    // A button can only ever paint its own rectangle, so making that rectangle
    // agree with the cell means matching the cell's padding by hand -- and it
    // is off by a pixel or two forever, because the two are computed by
    // different code from different values. TableSetBgColor fills the cell
    // rect that ImGui itself derived, so it lines up by construction rather
    // than by arithmetic.
    //
    // The button is therefore painted with nothing at all: it survives purely
    // as the hit target and the popup anchor.
    Im::SetCursorPosX(Im::GetCursorPosX() - kCellPadX);
    const float width = Im::GetContentRegionAvail().x + kCellPadX;

    const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
    Im::PushStyleColor(Im::ImGuiCol_Button, invisible);
    Im::PushStyleColor(Im::ImGuiCol_ButtonHovered, invisible);
    Im::PushStyleColor(Im::ImGuiCol_ButtonActive, invisible);
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    // Left-aligned, because this is a sentence, not a caption. Centred, the
    // text drifted around as the rule was edited and the column stopped
    // reading as a list.
    Im::PushStyleVar(Im::ImGuiStyleVar_ButtonTextAlign, Im::ImVec2(0.0f, 0.5f));

    // Cut to fit. Neither of the two wide columns can hold its longest
    // phrase at any split of the width (reported in play, 2026-09-19: at
    // one ratio the action was cut, at the next the condition), so nothing
    // is lost rather than the loss being moved from one column to the
    // other -- `elided` says it was cut and the CALLER puts the whole of it
    // on the hover.
    //
    // The caller, and not this, because a tooltip set here would be a
    // WINDOW opened between the button and the caller's own IsItemHovered,
    // which asks about the last item drawn -- and the last item would no
    // longer be the button. That is how the reason on a greyed action cell
    // went silent (2026-09-20). Nothing here may come between the two.
    const auto *style = Im::GetStyle();
    const std::string shown = Elide(label, width - (style ? style->FramePadding.x * 2.0f : 0.0f));
    if (elided)
        *elided = shown != label;

    const bool clicked = Im::Button((shown + "##" + id).c_str(), Im::ImVec2(width, 0.0f));
    const bool hovered = Im::IsItemHovered(0);

    Im::PopStyleVar(2);
    Im::PopStyleColor(3);

    // Taken from the button just drawn, before anything moves the cursor.
    const Im::ImVec2 below{Im::GetItemRectMin().x, Im::GetItemRectMax().y};

    if (hovered)
        Im::TableSetBgColor(Im::ImGuiTableBgTarget_CellBg, Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f), -1);

    if (clicked)
        Im::OpenPopup(id, 0);

    return below;
}

bool BeginCascade(const char *label, const char *tooltip)
{
    auto *draw = Im::GetWindowDrawList();
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const float right = CascadeIconRight();
    Im::PushStyleColor(Im::ImGuiCol_Text, Im::ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    const bool open = Im::BeginMenu(label, true);
    const bool hovered = Im::IsItemHovered(0);
    Im::PopStyleColor(1);
    if (tooltip && *tooltip && hovered)
        Im::SetTooltip("%s", tooltip);
    if (draw)
        Im::ImDrawListManager::AddText(draw, pos, Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), label);
    CascadeIcon(draw, Glyph::CaretRight, pos, right);
    return open;
}

bool CascadeItem(const char *label, bool selected, const Im::ImVec4 *textColour)
{
    auto *draw = Im::GetWindowDrawList();
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    const float right = CascadeIconRight();
    if (textColour)
        Im::PushStyleColor(Im::ImGuiCol_Text, *textColour);
    const bool clicked = Im::MenuItem(label, nullptr, false, true);
    if (textColour)
        Im::PopStyleColor(1);
    if (selected)
        CascadeIcon(draw, Glyph::Tick, pos, right);
    return clicked;
}

void PlainHeaderRow(const std::vector<const char *> &labels)
{
    Im::TableNextRow(Im::ImGuiTableRowFlags_Headers, 0.0f);
    int column = 0;
    for (const char *label : labels)
    {
        Im::TableSetColumnIndex(column++);
        Im::Text("%s", label);
    }
}

float DisclosureWidth()
{
    return Im::GetTextLineHeight() * 0.5f + kCellPadX;
}

void DrawDisclosure(Im::ImVec2 pos, bool open)
{
    auto *draw = Im::GetWindowDrawList();
    if (!draw)
        return;
    const float h = Im::GetTextLineHeight();
    const float s = h * 0.5f;
    const float top = pos.y + (h - s) * 0.5f;
    const auto ink = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);
    if (open)
        Im::ImDrawListManager::AddTriangleFilled(draw, {pos.x, top}, {pos.x + s, top},
                                                 {pos.x + s * 0.5f, top + s * 0.8f}, ink);
    else
        Im::ImDrawListManager::AddTriangleFilled(draw, {pos.x, top}, {pos.x + s * 0.8f, top + s * 0.5f},
                                                 {pos.x, top + s}, ink);
}

const Im::ImVec4 *NameTint(const InventoryItem &item)
{
    return item.artifact ? &kArtifact : item.enchanted ? &kEnchanted : item.spellTome ? &kSpellTome : nullptr;
}

bool Badged(const InventoryItem &item)
{
    return item.artifact || item.enchanted || !item.poison.rows.empty() || item.stolen;
}

float DrawNameBadges(Im::ImDrawList *draw, const InventoryItem &item, Im::ImVec2 at, bool dim)
{
    const float h = Im::GetTextLineHeight();
    float x = at.x;
    const auto ink = [dim](const Im::ImVec4 &colour) {
        Im::PushStyleColor(Im::ImGuiCol_Text, dim ? Im::GetStyle()->Colors[Im::ImGuiCol_TextDisabled] : colour);
        const Im::ImU32 u32 = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);
        Im::PopStyleColor(1);
        return u32;
    };
    const auto badge = [&](unsigned codepoint, const Im::ImVec4 &colour) {
        // A box the glyph's own width, so the mark sits where the text ends
        // and not a margin past it.
        const float box = CodepointWidth(codepoint, kPinScale);
        if (draw)
            DrawCodepoint(draw, codepoint, {x, at.y}, {x + box, at.y + h}, ink(colour), kPinScale, true);
        x += box + kBadgeGap;
    };
    if (item.artifact)
        badge(0xF521, kArtifact); // crown
    else if (item.enchanted)
        badge(0xF0E7, kEnchanted); // bolt
    if (!item.poison.rows.empty())
    {
        const Im::ImVec2 from{x, at.y};
        badge(0xF714, kPoison); // skull-crossbones
        if (draw && Im::IsMouseHoveringRect(from, {x - kBadgeGap, at.y + h}, true))
            Tooltip(item.poison.rows.front().label);
    }
    if (item.stolen)
        badge(0xF256, kStolen); // hand
    return x - at.x;
}

void NameBadges(const InventoryItem &item, bool dim, bool framed)
{
    if (!Badged(item))
        return;
    Im::SameLine(0.0f, kBadgeGap);
    Im::ImVec2 at = Im::GetCursorScreenPos();
    if (framed)
        at.y += Im::GetStyle()->FramePadding.y;
    const float width = DrawNameBadges(Im::GetWindowDrawList(), item, at, dim);
    Im::Dummy({width, framed ? Im::GetFrameHeight() : Im::GetTextLineHeight()});
}

void BanAllButton(const char *id, const BanAll &banAll)
{
    const auto rows = banAll.rows();
    const bool every =
        !rows.empty() && std::all_of(rows.begin(), rows.end(), [](const auto &row) { return row.second; });
    if (every)
        Im::PushStyleColor(Im::ImGuiCol_Button, Im::GetStyle()->Colors[Im::ImGuiCol_Header]);
    const bool clicked = GlyphButton(std::string(id) + "banall", Im::GetFrameHeight(), Glyph::Ban);
    if (every)
        Im::PopStyleColor(1);
    if (Im::IsItemHovered(0))
        Im::SetTooltip("%s", every ? Tr("Click to unban all visible rows") : Tr("Click to ban all visible rows"));
    if (!clicked || rows.empty())
        return;
    std::vector<WearTarget> targets;
    for (const auto &[target, banned] : rows)
        if (banned == every)
            targets.push_back(target);
    RequestWearAll(banAll.who, std::move(targets), every ? WearRequest::Unban : WearRequest::Ban);
}

void FilterRow(const char *id, char *buffer, std::size_t size, const std::function<std::size_t()> &shown,
               std::size_t total, const char *noun, const std::function<void()> &beside)
{
    const float right = Im::GetCursorPosX() + Im::GetContentRegionAvail().x - Im::GetStyle()->ItemSpacing.x;
    FilterBox(id, buffer, size);
    if (beside)
    {
        Im::SameLine(0.0f, -1.0f);
        beside();
    }
    const std::size_t count = shown();
    const std::string text =
        count == total ? TrFormat("{} {}", total, noun) : TrFormat("{} of {} {}", count, total, noun);
    Im::SameLine((std::max)(0.0f, right - TextWidth(text)), -1.0f);
    Im::AlignTextToFramePadding();
    Im::TextDisabled("%s", text.c_str());
}

void TextRightInCell(const std::string &text)
{
    const float slack = Im::GetContentRegionAvail().x - TextWidth(text);
    if (slack > 0.0f)
        Im::SetCursorPosX(Im::GetCursorPosX() + slack);
    Im::Text("%s", text.c_str());
}

void NoteTooltip(const std::string &note)
{
    Im::BeginTooltip();
    std::size_t from = 0;
    while (from <= note.size())
    {
        const std::size_t end = note.find('\n', from);
        const std::string line = note.substr(from, end == std::string::npos ? std::string::npos : end - from);
        from = end == std::string::npos ? note.size() + 1 : end + 1;
        if (!line.empty())
            Im::Text("%s", line.c_str());
    }
    Im::EndTooltip();
}

void BreakdownTooltip(const ft::Breakdown &b)
{
    Im::BeginTooltip();
    // A detail line's amount goes in a column of its own, inset from the
    // lines' amounts: lined up with them, a multiplier's Base 1.00 read as
    // another term of the weapon's sum (2026-09-14). Two columns where no
    // line opens.
    const bool opens =
        std::any_of(b.lines.begin(), b.lines.end(), [](const ft::BreakdownLine &line) { return !line.detail.empty(); });
    const int columns = opens ? 3 : 2;
    const int amounts = columns - 1;
    if (Im::BeginTable("breakdown", columns, Im::ImGuiTableFlags_SizingFixedFit, Im::ImVec2(0.0f, 0.0f), 0.0f))
    {
        const auto lines = [&](const auto &self, const std::vector<ft::BreakdownLine> &list, int depth) -> void {
            for (const ft::BreakdownLine &line : list)
            {
                Im::TableNextRow(0, 0.0f);
                Im::TableSetColumnIndex(0);
                // Detail is indented and nothing more: dimmed, it read as
                // inactive, which is what dimming means on the perk page.
                std::string label(static_cast<std::size_t>(depth) * 3, ' ');
                label += line.label;
                Im::Text("%s", label.c_str());
                Im::TableSetColumnIndex(depth == 0 ? amounts : 1);
                TextRightInCell(ft::AmountText(b, line));
                self(self, line.detail, depth + 1);
            }
        };
        lines(lines, b.lines, 0);
        // The rule under the last line, then the total: the sum as a
        // schoolbook writes it. Not for a base alone, which is the sum.
        const bool onlyBase = b.lines.size() == 1 && b.lines.front().op == ft::Op::Start;
        if (!onlyBase)
        {
            Im::TableNextRow(0, 0.0f);
            for (int column = 0; column < columns; ++column)
            {
                Im::TableSetColumnIndex(column);
                Im::Separator();
            }
            Im::TableNextRow(0, 0.0f);
            Im::TableSetColumnIndex(0);
            Im::Text("%s", b.totalLabel.c_str());
            Im::TableSetColumnIndex(amounts);
            TextRightInCell(ft::TotalText(b));
        }
        Im::EndTable();
    }
    Im::EndTooltip();
}

bool CellClicked(const char *id, float height)
{
    const Im::ImVec4 invisible{0.0f, 0.0f, 0.0f, 0.0f};
    Im::PushStyleColor(Im::ImGuiCol_Header, invisible);
    Im::PushStyleColor(Im::ImGuiCol_HeaderHovered, invisible);
    Im::PushStyleColor(Im::ImGuiCol_HeaderActive, invisible);
    const bool clicked = Im::Selectable(id, false, 0, Im::ImVec2(0.0f, height));
    Im::PopStyleColor(3);
    if (Im::IsItemHovered(0))
        Im::TableSetBgColor(Im::ImGuiTableBgTarget_CellBg, Im::GetColorU32(Im::ImGuiCol_ButtonHovered, 1.0f), -1);
    return clicked;
}

void CentredHeading(const char *title)
{
    Im::PushStyleVar(Im::ImGuiStyleVar_SeparatorTextAlign, Im::ImVec2(0.5f, 0.5f));
    Im::SeparatorText(title);
    Im::PopStyleVar(1);
}

void DrawChips(const std::vector<Chip> &chips, int &selected)
{
    // Said before the strip is drawn, so a body that ends without one puts
    // the keys back on the tabs. One strip a tab, so this says it for the
    // whole body.
    g_chipsDrawn = true;
    // A and D walk the strip, wrapping at both ends: the row is short and
    // reading it as a ring costs a press fewer than turning back at the end.
    // By id and not by index -- a chip's id is its category, or the summon's
    // reference -- so the walk starts from whatever is selected now.
    if (g_chipStep != 0 && !chips.empty())
    {
        const auto at = std::find_if(chips.begin(), chips.end(), [&](const Chip &c) { return c.id == selected; });
        const int count = static_cast<int>(chips.size());
        const int from = at == chips.end() ? 0 : static_cast<int>(at - chips.begin());
        selected = chips[static_cast<std::size_t>((from + g_chipStep + count) % count)].id;
        g_chipStep = 0;
    }

    const auto *style = Im::GetStyle();
    const float padX = style->FramePadding.x;
    const float spacing = style->ItemSpacing.x;
    const float gap = padX;
    const float right = Im::GetCursorPosX() + Im::GetContentRegionAvail().x;
    auto *draw = Im::GetWindowDrawList();

    // Hovered and active are left alone: the mouse answers the same whether
    // the keys are here or not.
    const bool resting = g_keyRow != KeyRow::Chips;
    if (resting)
    {
        Im::ImVec4 header = style->Colors[Im::ImGuiCol_Header];
        header.w *= kRestingChip;
        Im::PushStyleColor(Im::ImGuiCol_Header, header);
    }

    FontAwesome::PushSolid();
    const Im::ImFont *iconFont = Im::GetFont();
    const float iconSize = Im::GetFontSize();
    FontAwesome::Pop();

    bool first = true;
    for (const Chip &chip : chips)
    {
        const std::string icon = Utf8(chip.icon);
        float iconWidth = 0.0f;
        if (iconFont)
        {
            FontAwesome::PushSolid();
            iconWidth = TextWidth(icon);
            FontAwesome::Pop();
        }
        const float width = padX + iconWidth + gap + TextWidth(chip.label) + padX;

        if (!first)
        {
            Im::SameLine(0.0f, spacing);
            if (Im::GetCursorPosX() + width > right)
                Im::NewLine();
        }
        first = false;

        const Im::ImVec2 pos = Im::GetCursorScreenPos();
        // By id, not label: two summons of one creature share a name, and
        // one ImGui id for both left the second unclickable.
        if (Im::Selectable(("##chip" + std::to_string(chip.id)).c_str(), selected == chip.id, 0,
                           Im::ImVec2(width, 0.0f)))
            selected = chip.id;

        if (!draw)
            continue;
        const auto ink = Im::GetColorU32(Im::ImGuiCol_Text, 1.0f);
        if (iconFont)
            Im::ImDrawListManager::AddText(draw, iconFont, iconSize, {pos.x + padX, pos.y}, ink, icon.c_str());
        Im::ImDrawListManager::AddText(draw, {pos.x + padX + iconWidth + gap, pos.y}, ink, chip.label.c_str());
    }

    if (resting)
        Im::PopStyleColor(1);
}

void SlashCell()
{
    auto *draw = Im::GetWindowDrawList();
    const Im::ImGuiTable *table = Im::GetCurrentTable();
    if (!draw || !table)
        return;
    const Im::ImRect cell = Im::TableGetCellBgRect(table, Im::TableGetColumnIndex());
    Im::ImDrawListManager::AddLine(draw, {cell.Min.x, cell.Max.y}, {cell.Max.x, cell.Min.y},
                                   Im::GetColorU32(Im::ImGuiCol_TableBorderStrong, 1.0f), 1.0f);
}

void OnCell(const char *id, const CharacterView &view, std::uint32_t form, const EquipCell &cell, Hand hand,
            bool clickable, const std::optional<ft::ItemVariant> &variant, const void *row)
{
    const Im::ImVec2 pos = Im::GetCursorScreenPos();
    if (!cell.allowed)
    {
        SlashCell();
        return;
    }
    if (clickable)
    {
        const WearRequest next = ft::NextWearRequest(cell, view.player);
        if (CellClicked(id))
            RequestWear(view.id, form, next, hand, variant, row);
        if (Im::IsItemHovered(0))
            Im::SetTooltip("%s", cell.banned              ? Tr("Banned. Click to unban.")
                                 : cell.pinned            ? Tr("Pinned. Click to ban.")
                                 : cell.on && view.player ? Tr("Equipped. Click to unequip.")
                                 : cell.on                ? Tr("Equipped. Click to pin.")
                                                          : Tr("Unequipped. Click to equip."));
    }
    DrawTickAt(pos, Im::GetColorU32(Im::ImGuiCol_Text, 1.0f), cell.on, cell.pinned, cell.banned);
}

void RefreshAfterAction()
{
    if (auto *task = SKSE::GetTaskInterface())
        task->AddTask([] { RefreshShownPage(); });
}

void PlayGameSound(const char *id)
{
    if (auto *task = SKSE::GetTaskInterface())
        task->AddTask([id] { RE::PlaySound(id); });
}

float AskedActionWidth(const char *label, bool asking)
{
    const float pad = 2.0f * Im::GetStyle()->FramePadding.x;
    return asking ? TextWidth(Tr("Confirm")) + pad + kCellPadX + TextWidth(Tr("Cancel")) + pad : TextWidth(label) + pad;
}

bool AskedAction(const char *label, bool can, const std::string &hover, bool &asking)
{
    asking = asking && can;
    if (!asking)
    {
        Im::BeginDisabled(!can);
        if (Im::Button(label, Im::ImVec2(0.0f, 0.0f)))
            asking = true;
        Im::EndDisabled();
        if (Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
            Tooltip(hover);
        return false;
    }
    bool confirmed = false;
    if (Im::Button(Tr("Confirm"), Im::ImVec2(0.0f, 0.0f)))
    {
        confirmed = true;
        asking = false;
    }
    if (Im::IsItemHovered(0))
        Tooltip(hover);
    Im::SameLine(0.0f, kCellPadX);
    if (Im::Button(Tr("Cancel"), Im::ImVec2(0.0f, 0.0f)))
        asking = false;
    return confirmed;
}

bool AskedActionAtRight(const char *label, bool can, const std::string &hover, bool &asking, float lineRight)
{
    Im::SameLine(0.0f, 0.0f);
    Im::SetCursorPosX((std::max)(Im::GetCursorPosX() + kCellPadX, lineRight - AskedActionWidth(label, asking && can)));
    return AskedAction(label, can, hover, asking);
}

bool ActionAtRight(const char *label, bool can, const char *hover, float lineRight)
{
    Im::SameLine(0.0f, 0.0f);
    Im::SetCursorPosX((std::max)(Im::GetCursorPosX() + kCellPadX, lineRight - AskedActionWidth(label, false)));
    Im::BeginDisabled(!can);
    const bool clicked = Im::Button(label, Im::ImVec2(0.0f, 0.0f));
    Im::EndDisabled();
    if (Im::IsItemHovered(Im::ImGuiHoveredFlags_AllowWhenDisabled))
        Tooltip(hover);
    return clicked;
}

bool BackButton()
{
    Im::PushStyleVar(Im::ImGuiStyleVar_FrameBorderSize, 0.0f);
    const bool clicked = GlyphButton("back", Im::GetFrameHeight(), Glyph::Back);
    Im::PopStyleVar(1);
    return clicked;
}

void DetailName(const std::string &name, const Im::ImVec4 *tint)
{
    Im::SameLine(0.0f, kCellPadX);
    Im::AlignTextToFramePadding();
    if (tint)
        Im::TextColored(*tint, "%s", name.c_str());
    else
        Im::Text("%s", name.c_str());
}

void DetailSubtitle(const std::string &text)
{
    Im::SameLine(0.0f, kCellPadX * 2.0f);
    Im::AlignTextToFramePadding();
    Im::TextDisabled("%s", text.c_str());
}

} // namespace ft::game::ui
