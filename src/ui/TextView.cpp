// The text viewer: a file view showing a text file -- source, a script,
// JSON, a log -- in the fixed-width face with line numbers. Drag to select
// and copy; double-click takes a word, triple-click a line. Find lights up
// every match; Go to line jumps. An edit made elsewhere shows at once, and
// a view scrolled to the end stays there, so a growing log can be watched.
//
// Lines are drawn here rather than in an InputTextMultiline: a frame
// touches only the lines in view, so a log of a million lines scrolls as
// freely as a script of twenty.
#include "ui/MainWindow.h"
#include "ui/Fonts.h"
#include "ui/IconsMaterialDesign.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include "app/Format.h"
#include "app/TextFile.h"
#include "platform/Shell.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace ui
{

namespace text = app::text;

struct TextViewUi
{
    std::string    loadedPath;
    text::Document doc;
    std::string    error;
    uint64_t       fileBytes = 0;
    bool           truncated = false;   // longer than kMaxBytes; only its start is shown
    std::filesystem::file_time_type modified{};
    double         lastCheck = 0;       // when the file was last looked at for changes
    double         checkEvery = 1.0;    // longer for a file slow to read

    // The selection, as byte offsets into doc.text; the anchor is where the
    // drag began and the cursor where it is now, in either order.
    size_t selAnchor = 0;
    size_t selCursor = 0;
    bool   dragging = false;

    float scrollTo = -1;                // a position for the next frame
    float scrollXTo = -1;
    bool  scrollToEnd = false;
    bool  atEnd = false;                // last frame's view showed the last line
    float scrollY = 0;
    float widest = 0;                   // the widest line drawn yet, for the scroll range

    bool                findOpen = false;
    bool                findWantFocus = false;
    std::string         findText;
    std::string         findDoneFor;    // the text `hits` is for
    std::vector<size_t> hits;           // byte offsets, in order
    int                 currentHit = -1;
    bool                scrollToHit = false;

    bool        gotoOpen = false;
    bool        gotoWantFocus = false;
    std::string gotoText;

    size_t SelStart() const { return std::min(selAnchor, selCursor); }
    size_t SelEnd() const { return std::max(selAnchor, selCursor); }
    bool   HasSelection() const { return selAnchor != selCursor; }
};

namespace
{

constexpr uint64_t kMaxBytes = 128ull * 1024 * 1024;
constexpr size_t   kMaxHits = 100000;
constexpr double   kReloadCheckSeconds = 1.0;

std::filesystem::path U8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// ---- Loading -----------------------------------------------------------------

void Load(TextViewUi& t, const std::string& path, bool keepPlace)
{
    const auto began = std::chrono::steady_clock::now();
    t.loadedPath = path;
    t.error.clear();
    t.doc = text::Document{};
    t.truncated = false;
    t.findDoneFor.clear();   // find runs again on the new text
    t.widest = 0;
    t.dragging = false;
    if (!keepPlace)
        {
        t.scrollTo = 0;
        t.scrollXTo = 0;
        t.selAnchor = t.selCursor = 0;
        }
    t.lastCheck = ImGui::GetTime();

    std::error_code ec;
    t.modified = std::filesystem::last_write_time(U8(path), ec);
    t.fileBytes = 0;
    const auto size = std::filesystem::file_size(U8(path), ec);
    if (!ec) t.fileBytes = (uint64_t)size;
    std::ifstream stream(U8(path), std::ios::binary);
    if (!stream)
        {
        t.error = "The file could not be opened.";
        return;
        }
    std::string bytes((size_t)std::min(t.fileBytes, kMaxBytes), '\0');
    stream.read(bytes.data(), (std::streamsize)bytes.size());
    bytes.resize((size_t)stream.gcount());
    t.truncated = t.fileBytes > kMaxBytes;
    if (t.truncated) text::TrimToCharacter(bytes);
    t.doc = text::Decode(bytes);
    t.selAnchor = std::min(t.selAnchor, t.doc.text.size());
    t.selCursor = std::min(t.selCursor, t.doc.text.size());

    // A big log takes a while to read; looking for changes every second
    // would then be most of what the app does.
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    t.checkEvery = std::max(kReloadCheckSeconds, took * 10.0);
}

// Notices an edit made in another program and shows it, keeping the place,
// or keeping to the end when the view was there.
void ReloadIfChanged(TextViewUi& t)
{
    const double now = ImGui::GetTime();
    if (now - t.lastCheck < t.checkEvery) return;
    t.lastCheck = now;
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(U8(t.loadedPath), ec);
    if (ec || modified == t.modified) return;
    const bool follow = t.atEnd;
    Load(t, t.loadedPath, true);
    t.scrollToEnd = follow;
}

// ---- Measuring ---------------------------------------------------------------

struct Metrics
{
    ImFont*      font = nullptr;
    ImFontBaked* baked = nullptr;
    float        size = 0;
    float        lineH = 0;
    float        tabW = 0;
};

// Steps through a line a code point at a time, knowing where each one
// starts on screen; tabs run to the next stop.
struct Walker
{
    const Metrics&   m;
    std::string_view line;
    size_t           at = 0;      // byte of the current code point
    size_t           len = 0;     // its length in bytes
    unsigned int     cp = 0;
    float            x = 0;       // where it starts
    float            w = 0;       // and how wide it is

    bool Next()
    {
        at += len;
        x += w;
        if (at >= line.size()) return false;
        len = (size_t)ImTextCharFromUtf8(&cp, line.data() + at, line.data() + line.size());
        if (len == 0) len = 1;
        w = cp == '\t' ? m.tabW - std::fmod(x, m.tabW) : m.baked->GetCharAdvance((ImWchar)cp);
        return true;
    }
};

float XOf(const Metrics& m, std::string_view line, size_t byte)
{
    Walker w{ m, line };
    while (w.Next())
        if (w.at >= byte) return w.x;
    return w.x;
}

// The byte in the line nearest to x: past the middle of a character is
// after it.
size_t ByteAt(const Metrics& m, std::string_view line, float x)
{
    Walker w{ m, line };
    while (w.Next())
        if (x < w.x + w.w * 0.5f) return w.at;
    return line.size();
}

bool IsWordByte(char c)
{
    const unsigned char u = (unsigned char)c;
    return std::isalnum(u) || c == '_' || u >= 0x80;
}

void SelectWordAt(TextViewUi& t, size_t offset)
{
    const std::string& s = t.doc.text;
    size_t b = offset, e = offset;
    if (offset < s.size() && IsWordByte(s[offset]))
        {
        while (b > 0 && IsWordByte(s[b - 1])) --b;
        while (e < s.size() && IsWordByte(s[e])) ++e;
        }
    else if (offset < s.size() && s[offset] != '\n')
        {
        // Punctuation or a space: that one character.
        unsigned int cp;
        e = offset + (size_t)std::max(1, ImTextCharFromUtf8(&cp, s.data() + offset, s.data() + s.size()));
        }
    t.selAnchor = b;
    t.selCursor = e;
}

void SelectLine(TextViewUi& t, size_t line)
{
    t.selAnchor = t.doc.lineStarts[line];
    t.selCursor = t.doc.LineEnd(line);
}

// ---- Find and Go to line -----------------------------------------------------

void FindAll(TextViewUi& t)
{
    t.findDoneFor = t.findText;
    t.hits = text::FindAll(t.doc.text, t.findText, kMaxHits);
    t.currentHit = -1;
    if (t.hits.empty()) return;
    // The first match at or after the selection, so finding from where
    // the reader is looking.
    const auto it = std::lower_bound(t.hits.begin(), t.hits.end(), t.SelStart());
    t.currentHit = it == t.hits.end() ? 0 : (int)(it - t.hits.begin());
    t.scrollToHit = true;
}

void StepHit(TextViewUi& t, int dir)
{
    if (t.hits.empty()) return;
    const int n = (int)t.hits.size();
    t.currentHit = ((t.currentHit + dir) % n + n) % n;
    t.scrollToHit = true;
}

void OpenFind(TextViewUi& t)
{
    t.gotoOpen = false;
    t.findOpen = true;
    t.findWantFocus = true;
    // A selection on one line is what to look for.
    if (t.HasSelection())
        {
        const std::string_view sel = std::string_view(t.doc.text).substr(t.SelStart(), t.SelEnd() - t.SelStart());
        if (sel.find('\n') == std::string_view::npos && sel.size() < 200) t.findText = std::string(sel);
        }
}

void CloseFind(TextViewUi& t)
{
    t.findOpen = false;
    t.findText.clear();
    t.hits.clear();
    t.currentHit = -1;
    t.findDoneFor.clear();
}

void OpenGoto(TextViewUi& t)
{
    CloseFind(t);
    t.gotoOpen = true;
    t.gotoWantFocus = true;
    t.gotoText.clear();
}

void GoToLine(TextViewUi& t, const std::string& entry, float lineH, float viewH)
{
    const long n = std::strtol(entry.c_str(), nullptr, 10);
    if (n <= 0) return;
    const size_t line = std::min((size_t)n, t.doc.LineCount()) - 1;
    SelectLine(t, line);
    t.scrollTo = std::max(0.0f, (float)line * lineH - viewH * 0.33f);
    t.scrollXTo = 0;
}

// ---- Drawing -----------------------------------------------------------------

void CopySelection(const TextViewUi& t)
{
    if (!t.HasSelection()) return;
    const std::string sel = t.doc.text.substr(t.SelStart(), t.SelEnd() - t.SelStart());
    ImGui::SetClipboardText(sel.c_str());
}

void DrawToolbar(app::AppState& state, UiState& ui, app::FileView& view, TextViewUi& t)
{
    if (IconButton("Find", ICON_MD_SEARCH, "Find (Ctrl+F)", t.error.empty()))
        {
        if (t.findOpen) CloseFind(t);
        else OpenFind(t);
        }
    ImGui::SameLine();
    if (IconButton("GoToLine", ICON_MD_FORMAT_LIST_NUMBERED, "Go to line (Ctrl+G)", t.error.empty()))
        {
        if (t.gotoOpen) t.gotoOpen = false;
        else OpenGoto(t);
        }
    ImGui::SameLine();
    if (IconButton("CopyAll", ICON_MD_CONTENT_COPY, t.HasSelection() ? "Copy selection (Ctrl+C)" : "Copy all", t.error.empty()))
        {
        if (t.HasSelection()) CopySelection(t);
        else ImGui::SetClipboardText(t.doc.text.c_str());
        }

    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonW = [&](const char* icon) { return ImGui::CalcTextSize(icon).x + style.FramePadding.x * 2.0f; };
    const float cluster = buttonW(ICON_MD_FOLDER_OPEN) + buttonW(ICON_MD_OPEN_IN_NEW) + style.ItemSpacing.x;
    ToolbarGroup(cluster);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - cluster));
    if (IconButton("ShowInFolder", ICON_MD_FOLDER_OPEN, "Show in folder view")) ShowInFolderView(state, ui, view.path);
    ImGui::SameLine();
    if (IconButton("OpenExternal", ICON_MD_OPEN_IN_NEW, "Open with default program"))
        {
        std::string error;
        if (!platform::OpenWithShell(view.path, error)) ShowError(ui, error);
        }
}

void DrawFindBar(TextViewUi& t)
{
    if (t.findWantFocus)
        {
        ImGui::SetKeyboardFocusHere();
        t.findWantFocus = false;
        }
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.5f, ImGui::GetFontSize() * 18));
    if (ImGui::InputTextWithHint("##find", "Find in file", &t.findText, ImGuiInputTextFlags_EnterReturnsTrue))
        {
        StepHit(t, ImGui::GetIO().KeyShift ? -1 : 1);
        t.findWantFocus = true;   // Enter leaves the field; keep typing in it
        }
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) CloseFind(t);
    if (t.findText != t.findDoneFor) FindAll(t);
    ImGui::SameLine();
    if (IconButton("FindPrev", ICON_MD_KEYBOARD_ARROW_UP, "Previous match (Shift+Enter)", !t.hits.empty())) StepHit(t, -1);
    ImGui::SameLine();
    if (IconButton("FindNext", ICON_MD_KEYBOARD_ARROW_DOWN, "Next match (Enter)", !t.hits.empty())) StepHit(t, 1);
    if (!t.findText.empty())
        {
        std::string count = t.hits.empty() ? std::string("No matches")
                                           : std::to_string(t.currentHit + 1) + " of " + std::to_string(t.hits.size());
        if (t.hits.size() >= kMaxHits) count += "+";
        ToolbarGroup(ImGui::CalcTextSize(count.c_str()).x);
        ImGui::AlignTextToFramePadding();
        MutedText(count.c_str());
        }
    ToolbarGroup(ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()));
    if (IconButton("FindClose", ICON_MD_CLOSE, "Close (Escape)")) CloseFind(t);
}

void DrawGotoBar(TextViewUi& t, float lineH, float viewH)
{
    if (t.gotoWantFocus)
        {
        ImGui::SetKeyboardFocusHere();
        t.gotoWantFocus = false;
        }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
    const std::string hint = "Line, 1 to " + std::to_string(t.doc.LineCount());
    if (ImGui::InputTextWithHint("##goto", hint.c_str(), &t.gotoText,
                                 ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue))
        {
        GoToLine(t, t.gotoText, lineH, viewH);
        t.gotoOpen = false;
        }
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) t.gotoOpen = false;
    ToolbarGroup(ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()));
    if (IconButton("GotoClose", ICON_MD_CLOSE, "Close (Escape)")) t.gotoOpen = false;
}

// One line's text, only the part between clipL and clipR, in runs between
// its tabs.
void DrawLineText(ImDrawList* dl, const Metrics& m, std::string_view line, ImVec2 at, float clipL, float clipR, ImU32 color)
{
    Walker w{ m, line };
    size_t runStart = std::string_view::npos;
    float  runX = 0;
    auto flush = [&](size_t end) {
        if (runStart != std::string_view::npos && end > runStart)
            dl->AddText(m.font, m.size, ImVec2(at.x + runX, at.y), color, line.data() + runStart, line.data() + end);
        runStart = std::string_view::npos;
    };
    while (w.Next())
        {
        if (at.x + w.x > clipR) break;
        if (w.cp == '\t') flush(w.at);
        else if (runStart == std::string_view::npos && at.x + w.x + w.w >= clipL)
            {
            runStart = w.at;
            runX = w.x;
            }
        }
    flush(w.at);
}

void DrawLines(TextViewUi& t, ImVec2 size, bool focused)
{
    const ThemeTokens& theme = ThemeAt(CurrentThemeIndex());
    const Palette& pal = CurrentPalette();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::BeginChild("##text", size, ImGuiChildFlags_None,
                                        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!open)
        {
        ImGui::EndChild();
        return;
        }
    ImGui::PushFont(fonts::Mono(), 0.0f);

    Metrics m;
    m.font = ImGui::GetFont();
    m.baked = ImGui::GetFontBaked();
    m.size = ImGui::GetFontSize();
    m.lineH = std::round(m.size * 1.35f);
    const float adv = m.baked->GetCharAdvance(' ');
    m.tabW = adv * (float)text::kTabWidth;

    const text::Document& doc = t.doc;
    const size_t lines = doc.LineCount();
    const int digits = (int)std::to_string(lines).size();
    const float pad = std::round(m.size * 0.6f);
    const float gutterW = (float)std::max(digits, 3) * adv + pad * 2.0f;
    t.widest = std::max(t.widest, (float)doc.longestLine * adv);
    const ImVec2 content(gutterW + pad + t.widest + pad * 2.0f, (float)lines * m.lineH + pad);

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImRect inner = window->InnerRect;
    const float viewH = inner.GetHeight();

    // Scrolls asked for by the last frame, or by the reload just now.
    if (t.scrollToEnd)
        {
        ImGui::SetScrollY(content.y);
        t.scrollToEnd = false;
        }
    if (t.scrollTo >= 0)
        {
        ImGui::SetScrollY(t.scrollTo);
        t.scrollTo = -1;
        }
    if (t.scrollXTo >= 0)
        {
        ImGui::SetScrollX(t.scrollXTo);
        t.scrollXTo = -1;
        }
    if (t.scrollToHit && t.currentHit >= 0 && t.currentHit < (int)t.hits.size())
        {
        const size_t hit = t.hits[(size_t)t.currentHit];
        const size_t line = doc.LineOf(hit);
        const float y = (float)line * m.lineH;
        if (y < ImGui::GetScrollY() || y + m.lineH > ImGui::GetScrollY() + viewH) ImGui::SetScrollY(std::max(0.0f, y - viewH * 0.33f));
        const float x = XOf(m, doc.Line(line), hit - doc.lineStarts[line]);
        const float textW = inner.GetWidth() - gutterW - pad;
        if (x < ImGui::GetScrollX() || x > ImGui::GetScrollX() + textW * 0.9f) ImGui::SetScrollX(std::max(0.0f, x - textW * 0.33f));
        t.scrollToHit = false;
        }

    const ImVec2 origin = ImGui::GetCursorScreenPos();   // already moved by the scroll
    const float textX = origin.x + gutterW + pad;
    ImGui::InvisibleButton("##lines", ImVec2(std::max(content.x, 1.0f), std::max(content.y, 1.0f)));
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    // Where the pointer is, as a byte offset into the text.
    auto offsetAt = [&](ImVec2 p) -> size_t {
        const float y = p.y - origin.y;
        if (y < 0) return 0;
        const size_t line = (size_t)(y / m.lineH);
        if (line >= lines) return doc.text.size();
        return doc.lineStarts[line] + ByteAt(m, doc.Line(line), p.x - textX);
    };

    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
        const size_t at = offsetAt(io.MousePos);
        const int clicks = io.MouseClickedCount[ImGuiMouseButton_Left];
        t.dragging = false;
        if (clicks == 2) SelectWordAt(t, at);
        else if (clicks >= 3) SelectLine(t, doc.LineOf(at));
        else
            {
            if (!io.KeyShift) t.selAnchor = at;
            t.selCursor = at;
            t.dragging = true;
            }
        }
    if (t.dragging && ImGui::IsItemActive())
        {
        t.selCursor = offsetAt(io.MousePos);
        // Past an edge, the view follows the pointer.
        const float below = io.MousePos.y - inner.Max.y, above = inner.Min.y - io.MousePos.y;
        if (below > 0) ImGui::SetScrollY(ImGui::GetScrollY() + std::min(below, m.lineH * 3));
        if (above > 0) ImGui::SetScrollY(ImGui::GetScrollY() - std::min(above, m.lineH * 3));
        const float right = io.MousePos.x - inner.Max.x, left = inner.Min.x + gutterW - io.MousePos.x;
        if (right > 0) ImGui::SetScrollX(ImGui::GetScrollX() + std::min(right, adv * 4));
        if (left > 0) ImGui::SetScrollX(ImGui::GetScrollX() - std::min(left, adv * 4));
        }
    if (!ImGui::IsItemActive()) t.dragging = false;

    if (ImGui::BeginPopupContextItem("##textmenu"))
        {
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, t.HasSelection())) CopySelection(t);
        if (ImGui::MenuItem("Select all", "Ctrl+A"))
            {
            t.selAnchor = 0;
            t.selCursor = doc.text.size();
            }
        ImGui::EndPopup();
        }

    // Keys, while this view has focus and no field is being typed in.
    if (focused && !io.WantTextInput)
        {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A))
            {
            t.selAnchor = 0;
            t.selCursor = doc.text.size();
            }
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) CopySelection(t);
        float dy = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) dy = m.lineH;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) dy = -m.lineH;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) dy = viewH - m.lineH;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) dy = -(viewH - m.lineH);
        if (dy != 0) ImGui::SetScrollY(ImGui::GetScrollY() + dy);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Home) || ImGui::IsKeyPressed(ImGuiKey_Home, false)) ImGui::SetScrollY(0);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_End) || ImGui::IsKeyPressed(ImGuiKey_End, false)) ImGui::SetScrollY(content.y);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) ImGui::SetScrollX(ImGui::GetScrollX() + adv * 4);
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) ImGui::SetScrollX(ImGui::GetScrollX() - adv * 4);
        }

    // The lines in view, and only those.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float scrollY = ImGui::GetScrollY();
    const size_t first = (size_t)std::max(0.0f, std::floor(scrollY / m.lineH));
    const size_t last = std::min(lines, (size_t)std::ceil((scrollY + viewH) / m.lineH) + 1);
    const float clipL = inner.Min.x + gutterW;
    const float clipR = inner.Max.x;
    const float textDy = std::round((m.lineH - m.size) * 0.5f);
    const size_t selStart = t.SelStart(), selEnd = t.SelEnd();
    const ImU32 textColor = theme.text;
    const ImU32 hitColor = WithAlpha(pal.warning, 90), currentColor = WithAlpha(pal.accent, 150);
    const size_t needle = t.findDoneFor.size();

    dl->PushClipRect(ImVec2(clipL, inner.Min.y), inner.Max, true);
    for (size_t i = first; i < last; ++i)
        {
        const std::string_view line = doc.Line(i);
        const size_t ls = doc.lineStarts[i], le = doc.LineEnd(i);
        const float y = origin.y + (float)i * m.lineH;

        // Selection under the text; a selected line break shows as a
        // sliver past the line's end, as editors draw it.
        if (selStart < selEnd && selStart <= le && selEnd > ls)
            {
            const size_t a = std::max(selStart, ls) - ls, b = std::min(selEnd, le) - ls;
            const float x0 = textX + XOf(m, line, a);
            float x1 = textX + XOf(m, line, b);
            if (selEnd > le) x1 += adv * 0.5f;
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + m.lineH), theme.selection);
            }
        // Find's matches on this line.
        if (needle > 0)
            {
            for (auto it = std::lower_bound(t.hits.begin(), t.hits.end(), ls); it != t.hits.end() && *it < le; ++it)
                {
                const size_t a = *it - ls, b = std::min(*it + needle, le) - ls;
                const bool current = (int)(it - t.hits.begin()) == t.currentHit;
                dl->AddRectFilled(ImVec2(textX + XOf(m, line, a) - 1, y), ImVec2(textX + XOf(m, line, b) + 1, y + m.lineH),
                                  current ? currentColor : hitColor, 2.0f);
                }
            }
        DrawLineText(dl, m, line, ImVec2(textX, y + textDy), clipL, clipR, textColor);
        }
    dl->PopClipRect();

    // The gutter stays put while the text scrolls sideways under it.
    const ImVec2 gutterMin(inner.Min.x, inner.Min.y), gutterMax(inner.Min.x + gutterW, inner.Max.y);
    dl->AddRectFilled(gutterMin, gutterMax, theme.codeBackground);
    dl->AddLine(ImVec2(gutterMax.x - 0.5f, gutterMin.y), ImVec2(gutterMax.x - 0.5f, gutterMax.y), pal.border);
    const size_t cursorLine = t.HasSelection() ? doc.LineOf(t.selCursor) : (size_t)-1;
    dl->PushClipRect(gutterMin, gutterMax, true);
    for (size_t i = first; i < last; ++i)
        {
        const std::string number = std::to_string(i + 1);
        const float w = (float)number.size() * adv;
        const float y = origin.y + (float)i * m.lineH + textDy;
        dl->AddText(m.font, m.size, ImVec2(gutterMax.x - pad - w, y), i == cursorLine ? theme.textMuted : theme.textFaint,
                    number.c_str());
        }
    dl->PopClipRect();

    t.scrollY = scrollY;
    t.atEnd = ImGui::GetScrollMaxY() <= 0 || scrollY >= ImGui::GetScrollMaxY() - 1.0f;
    ImGui::PopFont();
    ImGui::EndChild();
}

void DrawStatusBar(const TextViewUi& t)
{
    std::string text = std::string(text::EncodingName(t.doc.encoding));
    if (t.doc.lineEnding != text::LineEnding::None) text += "    " + std::string(text::LineEndingName(t.doc.lineEnding));
    const size_t lines = t.doc.LineCount();
    text += "    " + std::to_string(lines) + (lines == 1 ? " line" : " lines");
    if (t.fileBytes > 0) text += "    " + app::HumanSize(t.fileBytes);
    if (t.HasSelection())
        {
        const size_t n = t.SelEnd() - t.SelStart();
        text += "    " + std::to_string(n) + (n == 1 ? " byte" : " bytes") + " selected";
        }
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    MutedText(text.c_str());
    if (t.truncated)
        {
        const std::string note = "Showing the first " + app::HumanSize(kMaxBytes);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().warning);
        ImGui::TextUnformatted(("    " + note).c_str());
        ImGui::PopStyleColor();
        }
}

void DrawMessage(const std::string& text, ImVec2 size)
{
    ImGui::BeginChild("##message", size);
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str(), nullptr, false, size.x - 40.0f);
    ImGui::SetCursorPos(ImVec2((size.x - textSize.x) * 0.5f, (size.y - textSize.y) * 0.5f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textSize.x);
    ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().danger);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

} // namespace

TextViewInfo DescribeTextView(const FileViewUi& v)
{
    TextViewInfo info;
    if (!v.text) return info;
    const TextViewUi& t = *v.text;
    info.lines = t.doc.LineCount();
    info.hits = t.hits.size();
    info.selected = t.doc.text.substr(t.SelStart(), t.SelEnd() - t.SelStart());
    info.encoding = text::EncodingName(t.doc.encoding);
    info.error = t.error;
    return info;
}

void DrawTextFile(app::AppState& state, UiState& ui, app::FileView& view, FileViewUi& v)
{
    if (!v.text) v.text = std::make_shared<TextViewUi>();
    TextViewUi& t = *v.text;
    if (t.loadedPath != view.path) Load(t, view.path, false);
    else ReloadIfChanged(t);

    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && t.error.empty() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) OpenFind(t);
    if (focused && t.error.empty() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) OpenGoto(t);
    if (focused && t.findOpen && (ImGui::IsKeyPressed(ImGuiKey_F3) || ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F3)))
        StepHit(t, ImGui::GetIO().KeyShift ? -1 : 1);

    // Go to line needs the line height and the view's, which are known
    // only inside the text's child; last frame's are near enough.
    const float lineH = std::round(ImGui::GetFontSize() * 1.35f);
    DrawToolbar(state, ui, view, t);
    const float statusHeight = state.settings.showStatusBar ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2 : 0.0f;
    if (t.findOpen) DrawFindBar(t);
    else if (t.gotoOpen) DrawGotoBar(t, lineH, ImGui::GetContentRegionAvail().y - statusHeight);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(avail.x, std::max(avail.y - statusHeight, 1.0f));
    if (!t.error.empty()) DrawMessage(t.error, size);
    else DrawLines(t, size, focused);
    if (state.settings.showStatusBar) DrawStatusBar(t);
}

} // namespace ui
