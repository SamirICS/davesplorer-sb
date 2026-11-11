// Vertical tabs: with Settings > Vertical tabs on, a pane (a dock node
// holding views) shows no tab bar; instead each window in it draws the
// pane's tabs as a column down its left side, with a "+" at the bottom
// for a new tab in the same pane, the way Directory Opus lists folder tabs.
//
// A dock node with its tab bar gone shows only its first window, so
// choosing a tab moves that window to the front of the node's list.
#include "ui/MainWindow.h"
#include "ui/IconsMaterialDesign.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include "app/PathUtil.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cfloat>
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

// The window's title, without the "###id" that keeps it stable.
std::string TitleOf(const ImGuiWindow* w)
{
    const char* end = ImGui::FindRenderedTextEnd(w->Name);
    return std::string((const char*)w->Name, end);
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

    const float scale = ImGui::GetStyle().FontScaleMain * ImGui::GetStyle().FontScaleDpi;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 4));
    ImGui::BeginChild("##panetabs", ImVec2(state.settings.verticalTabsWidth * scale, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    std::string selfPath;
    for (const PaneEntry& e : entries)
        {
        if (e.window == self) selfPath = e.path;
        ImGui::PushID((int)e.window->ID);
        const std::string label = std::string(IconFor(e.kind)) + "  " + TitleOf(e.window);
        if (ImGui::Selectable(label.c_str(), e.window == self, ImGuiSelectableFlags_SpanAvailWidth) && e.window != self)
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
    if (ImGui::Button(ICON_MD_ADD "##newpanetab", ImVec2(-FLT_MIN, 0)))
        ui.paneNewTab = { selfPath, node ? node->ID : 0, true };
    TipOnHover("New tab in this pane");
    ImGui::EndChild();
    // The column's width is the user's, kept for every pane.
    state.settings.verticalTabsWidth = std::clamp(ImGui::GetItemRectSize().x / scale, 80.0f, 600.0f);
    ImGui::SameLine();
}

} // namespace

void PaneBeforeBegin(const app::AppState& state)
{
    // Set every frame, either way: a window keeps the class it was last
    // given, so turning the option off must hand it a plain one.
    ImGuiWindowClass wc;
    if (state.settings.verticalTabs) wc.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
    ImGui::SetNextWindowClass(&wc);
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
