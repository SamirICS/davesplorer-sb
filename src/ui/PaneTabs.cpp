// Vertical tabs: with Settings > Vertical tabs on, a pane (a dock node
// holding views) shows no tab bar; instead each window in it draws the
// pane's tabs as a column down its left side, with a "+" at the bottom
// for a new tab in the same pane, the way Directory Opus lists folder tabs.
//
// A dock node with its tab bar gone shows only its first window, so
// choosing a tab moves that window to the front of the node's list.
//
// With the option off, the ordinary tab bars are drawn as tall, and their
// names as large, as the column's rows.
#include "ui/MainWindow.h"
#include "ui/IconsMaterialDesign.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include "app/PathUtil.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>
#include <vector>

namespace ui
{

namespace
{

constexpr const char* kTabPayload = "DSP_PANE_TAB";

enum class PaneKind
{
    Folder,
    Programs,
    File,
    Other,
};

struct PaneEntry
{
    ImGuiWindow* window = nullptr;
    PaneKind     kind = PaneKind::Other;
    int          order = 0;     // folder views in tab order, then Programs, then files
    int          id = 0;        // the tab's, Programs view's or file view's own id
    std::string  path;          // what a new tab beside it opens
};

PaneEntry Describe(const app::AppState& state, ImGuiWindow* w)
{
    PaneEntry e;
    e.window = w;
    e.order = 1 << 20;
    for (size_t i = 0; i < state.tabs.size(); ++i)
        if (ViewWindowId(state.tabs[i].id) == w->ID)
            return { w, PaneKind::Folder, (int)i, state.tabs[i].id, state.tabs[i].path };
    for (size_t i = 0; i < state.programViews.size(); ++i)
        if (ProgramsWindowId(state.programViews[i].id) == w->ID)
            return { w, PaneKind::Programs, 100000 + (int)i, state.programViews[i].id, state.programViews[i].root };
    for (size_t i = 0; i < state.fileViews.size(); ++i)
        if (FileViewWindowId(state.fileViews[i].id) == w->ID)
            return { w, PaneKind::File, 200000 + (int)i, state.fileViews[i].id, app::ParentPath(state.fileViews[i].path) };
    return e;
}

const char* IconFor(PaneKind kind)
{
    switch (kind)
        {
        case PaneKind::Folder:   return ICON_MD_FOLDER;
        case PaneKind::Programs: return ICON_MD_APPS;
        case PaneKind::File:     return ICON_MD_DESCRIPTION;
        case PaneKind::Other:    break;
        }
    return ICON_MD_FOLDER;
}

// The window's title, without the "###id" that keeps it stable, nor the
// icon a Programs window's title starts with (the column draws its own).
std::string TitleOf(const ImGuiWindow* w, PaneKind kind)
{
    const char* end = ImGui::FindRenderedTextEnd(w->Name);
    std::string title((const char*)w->Name, end);
    if (kind == PaneKind::Programs && title.rfind(ICON_MD_SETTINGS_APPLICATIONS " ", 0) == 0)
        title.erase(0, sizeof(ICON_MD_SETTINGS_APPLICATIONS));
    return title;
}

// The icon's colour: folders in the app's red, like the shortcut bar;
// Programs in its tab colour.
ImU32 IconColorFor(PaneKind kind)
{
    const Palette& p = CurrentPalette();
    switch (kind)
        {
        case PaneKind::Folder:   return kFolderRed;
        case PaneKind::Programs: return p.programsTabOverline;
        case PaneKind::File:     return p.iconFile;
        case PaneKind::Other:    break;
        }
    return kFolderRed;
}

// The column's sizes, from the interface's font. Bigger than the rest of
// the interface: tabs are what the eye goes to when switching folders.
struct TabMetrics
{
    float scale = 1.0f;
    float text = 0.0f;
    float icon = 0.0f;
    float row = 0.0f;
};

TabMetrics Metrics()
{
    TabMetrics m;
    m.scale = ImGui::GetStyle().FontScaleMain * ImGui::GetStyle().FontScaleDpi;
    m.text = ImGui::GetFontSize() * 1.2f;
    m.icon = ImGui::GetFontSize() * 1.45f;
    m.row = m.icon + 12.0f * m.scale;
    return m;
}

void BringToFront(ImGuiDockNode* node, ImGuiWindow* w)
{
    for (int i = 1; i < node->Windows.Size; ++i)
        if (node->Windows[i] == w)
            {
            node->Windows.erase(node->Windows.begin() + i);
            node->Windows.push_front(w);
            break;
            }
}

void DrawColumn(app::AppState& state, UiState& ui, ImGuiWindow* self)
{
    ImGuiDockNode* node = self->DockIsActive ? self->DockNode : nullptr;
    std::vector<PaneEntry> entries;
    if (node)
        for (ImGuiWindow* w : node->Windows) entries.push_back(Describe(state, w));
    else
        entries.push_back(Describe(state, self));
    std::stable_sort(entries.begin(), entries.end(), [](const PaneEntry& a, const PaneEntry& b) { return a.order < b.order; });

    const TabMetrics metrics = Metrics();
    const float scale = metrics.scale;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 4));
    // Not ImGuiChildFlags_ResizeX: that keeps each child's width in
    // imgui.ini, over the setting. The width is the setting's, the same
    // in every pane, and the handle after the column changes it.
    ImGui::BeginChild("##panetabs", ImVec2(state.settings.verticalTabsWidth * scale, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const Palette& pal = CurrentPalette();
    const float textSize = metrics.text;
    const float iconSize = metrics.icon;
    const float rowHeight = metrics.row;
    std::string selfPath;
    for (const PaneEntry& e : entries)
        {
        if (e.window == self) selfPath = e.path;
        ImGui::PushID((int)e.window->ID);
        // A tall row, its icon and name drawn by hand at a larger size so
        // the icon can take its own colour.
        const std::string title = TitleOf(e.window, e.kind);
        const std::string label = std::string(IconFor(e.kind)) + "  " + title;
        const bool selected = e.window == self;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const std::string rowId = "##panetab" + std::to_string(e.window->ID);   // what the tests click
        const bool clicked = ImGui::Selectable(rowId.c_str(), selected, ImGuiSelectableFlags_SpanAvailWidth, ImVec2(0, rowHeight));
        {
        const ImVec2 rowMax(rowMin.x + ImGui::GetItemRectSize().x, rowMin.y + rowHeight);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (selected) dl->AddRectFilled(rowMin, ImVec2(rowMin.x + 3.0f * scale, rowMax.y), kFolderRed);
        const float y = rowMin.y + (rowHeight - textSize) * 0.5f;
        const float iconX = rowMin.x + 10.0f * scale;
        const ImVec4 clip(rowMin.x, rowMin.y, rowMax.x - 4.0f * scale, rowMax.y);
        dl->AddText(nullptr, iconSize, ImVec2(iconX, rowMin.y + (rowHeight - iconSize) * 0.5f), IconColorFor(e.kind),
                    IconFor(e.kind), nullptr, 0.0f, &clip);
        dl->AddText(nullptr, textSize, ImVec2(iconX + iconSize + 8.0f * scale, y),
                    selected ? pal.text : pal.textMuted, title.c_str(), nullptr, 0.0f, &clip);
        }
        if (clicked && !selected)
            {
            if (node) BringToFront(node, e.window);
            ImGui::FocusWindow(e.window);
            if (e.kind == PaneKind::Folder) state.activeTab = e.order;
            }
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Middle)) ui.paneClose.push_back(e.window->ID);
        if (ImGui::IsItemHovered() && e.kind != PaneKind::Other) ImGui::SetTooltip("%s", e.path.empty() ? "This PC" : e.path.c_str());
        if (ImGui::BeginPopupContextItem("##panetabmenu"))
            {
            if (ImGui::MenuItem("New tab here")) ui.paneNewTab = { e.path, node ? node->ID : 0, true };
            if (e.kind == PaneKind::Folder)
                {
                app::Tab& tab = state.tabs[(size_t)e.order];
                if (ImGui::MenuItem(tab.locked ? "Unlock tab" : "Lock tab")) tab.locked = !tab.locked;
                }
            if (ImGui::MenuItem("Close tab")) ui.paneClose.push_back(e.window->ID);
            ImGui::EndPopup();
            }
        // Folder tabs reorder by dragging one onto another.
        if (e.kind == PaneKind::Folder)
            {
            if (ImGui::BeginDragDropSource())
                {
                ImGui::SetDragDropPayload(kTabPayload, &e.id, sizeof e.id);
                ImGui::TextUnformatted(label.c_str());
                ImGui::EndDragDropSource();
                }
            if (ImGui::BeginDragDropTarget())
                {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kTabPayload))
                    ui.paneMove = { *(const int*)p->Data, e.id };
                ImGui::EndDragDropTarget();
                }
            }
        ImGui::PopID();
        }
    ImGui::Spacing();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.45f);   // unscaled, as PushFont wants
    ImGui::PushStyleColor(ImGuiCol_Text, kFolderRed);
    const bool add = ImGui::Button(ICON_MD_ADD "##newpanetab", ImVec2(-FLT_MIN, rowHeight));
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (add)
        ui.paneNewTab = { selfPath, node ? node->ID : 0, true };
    TipOnHover("New tab in this pane");
    ImGui::EndChild();
    ImGui::SameLine(0.0f, 0.0f);
    const float handle = 6.0f * scale;
    ImGui::InvisibleButton("##panetabswidth", ImVec2(handle, std::max(ImGui::GetContentRegionAvail().y, 1.0f)));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (ImGui::IsItemActive())
        state.settings.verticalTabsWidth = std::clamp(state.settings.verticalTabsWidth + ImGui::GetIO().MouseDelta.x / scale,
                                                      120.0f, 600.0f);
    if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        {
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x + handle * 0.35f, a.y), ImVec2(b.x - handle * 0.35f, b.y),
                                                  WithAlpha(kFolderRed, 160));
        }
    ImGui::SameLine(0.0f, 0.0f);
}

// The first pane (leaf node) under `node`, other than `skip`, holding a
// folder, Programs or file view: not the navigation pane.
ImGuiDockNode* FirstPaneWithViews(const app::AppState& state, ImGuiDockNode* node, const ImGuiDockNode* skip)
{
    if (node == nullptr || node == skip) return nullptr;
    if (node->IsLeafNode())
        {
        for (ImGuiWindow* w : node->Windows)
            if (Describe(state, w).kind != PaneKind::Other) return node;
        return nullptr;
        }
    if (ImGuiDockNode* found = FirstPaneWithViews(state, node->ChildNodes[0], skip)) return found;
    return FirstPaneWithViews(state, node->ChildNodes[1], skip);
}

// A view on its way into this node (a split just made, a new tab).
bool SomethingDocksInto(const UiState& ui, ImGuiID node)
{
    for (const auto& [id, v] : ui.views)
        if (v.dockHint == node) return true;
    for (const auto& [id, v] : ui.programViews)
        if (v.dockHint == node) return true;
    for (const auto& [id, v] : ui.fileViews)
        if (v.dockHint == node) return true;
    return ui.paneNewTab.requested && ui.paneNewTab.dockId == node;
}

} // namespace

void CollapseEmptyMainPane(const app::AppState& state, UiState& ui, ImGuiID dockspaceId)
{
    ImGuiDockNode* central = ImGui::DockBuilderGetCentralNode(dockspaceId);
    if (central == nullptr || !central->IsLeafNode() || central->Windows.Size > 0 || central->ParentNode == nullptr ||
        ui.viewerSplitFor >= 0 || SomethingDocksInto(ui, central->ID))
        {
        ui.emptyMainPaneFrames = 0;
        return;
        }
    // A window closing or about to dock passes through an empty node for
    // a frame or two; only one that stays empty is let go.
    if (++ui.emptyMainPaneFrames < 3) return;
    ImGuiDockNode* heir = FirstPaneWithViews(state, ImGui::DockBuilderGetNode(dockspaceId), central);
    if (heir == nullptr) return;   // no views anywhere: the empty main pane is all there is
    // The dockspace must keep one central node, and a removed central node
    // would hand its flag to its parent; so the flag moves to the heir first.
    central->SetLocalFlags(central->LocalFlags & ~ImGuiDockNodeFlags_CentralNode);
    heir->SetLocalFlags(heir->LocalFlags | ImGuiDockNodeFlags_CentralNode);
    ImGui::DockBuilderRemoveNode(central->ID);
    ui.emptyMainPaneFrames = 0;
}

bool PushTabBarSize(const app::AppState& state, const char* window)
{
    if (state.settings.verticalTabs) return false;
    // A floating node's tab bar is drawn in NewFrame, at the ordinary
    // size, so its windows must leave the ordinary room for it.
    if (window != nullptr)
        if (ImGuiWindow* w = ImGui::FindWindowByName(window))
            if (w->DockNode != nullptr && ImGui::DockNodeGetRootNode(w->DockNode)->IsFloatingNode()) return false;
    // The bar is the font's height plus the frame padding above and below,
    // so the padding makes up the rest of a column row. In whole pixels:
    // half a pixel more title than the window was laid out for leaves
    // every view scrollable by that half.
    const float row = std::round(Metrics().row);
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.2f);   // unscaled, as PushFont wants
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, (row - ImGui::GetFontSize()) * 0.5f));
    return true;
}

void PopTabBarSize(bool pushed)
{
    if (!pushed) return;
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

bool PaneBegin(const app::AppState& state, const char* name, bool* open, ImGuiWindowFlags flags)
{
    // Set every frame, either way: a window keeps the class it was last
    // given, so turning the option off must hand it a plain one.
    ImGuiWindowClass wc;
    if (state.settings.verticalTabs) wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&wc);
    const bool pushed = PushTabBarSize(state, name);
    const bool visible = ImGui::Begin(name, open, flags);
    PopTabBarSize(pushed);
    return visible;
}

bool PaneAfterBegin(app::AppState& state, UiState& ui, bool visible, bool wantFront)
{
    if (!state.settings.verticalTabs) return false;
    ImGuiWindow* self = ImGui::GetCurrentWindow();
    // A window just docked may not have its node until a frame later, so
    // the wish to be in front waits for it.
    if (wantFront) ui.paneFrontPending.insert(self->ID);
    if (ui.paneFrontPending.count(self->ID) && self->DockIsActive && self->DockNode)
        {
        BringToFront(self->DockNode, self);
        ui.paneFrontPending.erase(self->ID);
        }
    if (!visible) return false;
    DrawColumn(state, ui, self);
    ImGui::BeginChild("##panebody", ImVec2(0, 0), ImGuiChildFlags_None, self->Flags & (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse));
    return true;
}

void PaneEnd(bool body)
{
    if (body) ImGui::EndChild();
    ImGui::End();
}

void HandlePaneRequests(app::AppState& state, UiState& ui)
{
    if (ui.paneNewTab.requested)
        {
        OpenView(state, ui, ui.paneNewTab.path, ui.paneNewTab.dockId);
        ui.paneNewTab = {};
        }
    // A locked view's new neighbour goes straight after it in tab order,
    // which is the order the vertical tab column lists them in.
    for (const UiState::LockedOpen& r : ui.lockedOpens)
        {
        const int index = OpenView(state, ui, r.path, r.dockId);
        state.tabs[(size_t)index].selectOnLoad = r.select;
        for (size_t i = 0; i < state.tabs.size(); ++i)
            if (state.tabs[i].id == r.fromTab && (int)i + 1 < index)
                {
                std::rotate(state.tabs.begin() + (long)i + 1, state.tabs.begin() + index, state.tabs.begin() + index + 1);
                state.activeTab = (int)i + 1;
                break;
                }
        }
    ui.lockedOpens.clear();
    for (ImGuiID id : ui.paneClose)
        {
        for (size_t i = 0; i < state.tabs.size(); ++i)
            if (ViewWindowId(state.tabs[i].id) == id)
                {
                ui.views.erase(state.tabs[i].id);
                app::CloseTab(state, (int)i);
                break;
                }
        for (size_t i = 0; i < state.programViews.size(); ++i)
            if (ProgramsWindowId(state.programViews[i].id) == id)
                {
                CloseProgramsView(state, ui, state.programViews[i].id);
                break;
                }
        for (size_t i = 0; i < state.fileViews.size(); ++i)
            if (FileViewWindowId(state.fileViews[i].id) == id)
                {
                CloseFileView(state, ui, state.fileViews[i].id);
                break;
                }
        }
    ui.paneClose.clear();
    if (ui.paneMove.from != 0 && ui.paneMove.from != ui.paneMove.before)
        {
        auto indexOf = [&](int id) {
            for (size_t i = 0; i < state.tabs.size(); ++i)
                if (state.tabs[i].id == id) return (int)i;
            return -1;
        };
        const int from = indexOf(ui.paneMove.from), to = indexOf(ui.paneMove.before);
        if (from >= 0 && to >= 0)
            {
            const int activeId = state.active().id;
            app::Tab moving = std::move(state.tabs[(size_t)from]);
            state.tabs.erase(state.tabs.begin() + from);
            state.tabs.insert(state.tabs.begin() + to, std::move(moving));
            state.activeTab = indexOf(activeId);
            }
        }
    ui.paneMove = {};
}

} // namespace ui
