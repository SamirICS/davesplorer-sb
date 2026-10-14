// Saved layouts: reading the dockspace's tree into an app::Layout, and
// building views and a tree back from one.
#include "ui/MainWindow.h"

#include "app/Layout.h"
#include "app/PathUtil.h"
#include "app/TreeScan.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ui
{

namespace
{

ImGuiID NavWindowId() { return ImHashStr(kNavWindow); }

// What a docked window's tab is known by: the ID "#TAB" makes in the
// window's own ID stack, which a dock node's SelectedTabId holds.
ImGuiID TabIdOf(ImGuiID windowId) { return ImHashStr("#TAB", 0, windowId); }

void CaptureNode(const ImGuiDockNode* node, const std::unordered_map<ImGuiID, int>& viewByWindow, app::LayoutNode& out)
{
    if (node->IsSplitNode())
        {
        const int axis = (int)node->SplitAxis;
        out.split = node->SplitAxis == ImGuiAxis_X ? app::LayoutSplit::LeftRight : app::LayoutSplit::TopBottom;
        const float a = node->ChildNodes[0]->Size[axis];
        const float b = node->ChildNodes[1]->Size[axis];
        out.ratio = a + b > 0.0f ? a / (a + b) : 0.5f;
        out.children.resize(2);
        CaptureNode(node->ChildNodes[0], viewByWindow, out.children[0]);
        CaptureNode(node->ChildNodes[1], viewByWindow, out.children[1]);
        return;
        }

    // Tab order is the tab bar's; Windows is unordered. A window not in the
    // tab bar yet (docked this frame) goes last.
    std::vector<const ImGuiWindow*> windows;
    if (node->TabBar)
        for (const ImGuiTabItem& t : node->TabBar->Tabs)
            if (t.Window) windows.push_back(t.Window);
    for (const ImGuiWindow* w : node->Windows)
        if (std::find(windows.begin(), windows.end(), w) == windows.end()) windows.push_back(w);

    // Image viewers and windows of views already closed are not part of a
    // layout; only the navigation pane and open folder views are.
    for (const ImGuiWindow* w : windows)
        {
        if (w->ID == NavWindowId())
            {
            out.nav = true;
            continue;
            }
        const auto it = viewByWindow.find(w->ID);
        if (it == viewByWindow.end()) continue;
        out.views.push_back(it->second);
        if (node->SelectedTabId == w->TabId) out.selected = it->second;
        }
}

bool HasNav(const app::LayoutNode& node)
{
    if (node.nav) return true;
    for (const app::LayoutNode& c : node.children)
        if (HasNav(c)) return true;
    return false;
}

struct TreeBuilder
{
    app::AppState& state;
    UiState&       ui;
    std::vector<uint8_t> placed;                           // per view index
    std::vector<std::pair<ImGuiID, ImGuiID>> selections;   // node, window in front
    ImGuiID firstViewLeaf = 0;

    void Build(const app::LayoutNode& node, ImGuiID id)
    {
        if (node.split != app::LayoutSplit::None && node.children.size() == 2)
            {
            ImGuiID first = 0, second = 0;
            const ImGuiDir dir = node.split == app::LayoutSplit::LeftRight ? ImGuiDir_Left : ImGuiDir_Up;
            ImGui::DockBuilderSplitNode(id, dir, node.ratio, &first, &second);
            Build(node.children[0], first);
            Build(node.children[1], second);
            return;
            }
        if (node.nav)
            {
            ImGui::DockBuilderDockWindow(kNavWindow, id);
            // Alone, the navigation pane hides its tab bar, as in the
            // default layout.
            if (node.views.empty())
                if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(id))
                    n->SetLocalFlags(n->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
            }
        for (int v : node.views)
            {
            if (v < 0 || v >= (int)state.tabs.size() || placed[(size_t)v]) continue;
            const app::Tab& tab = state.tabs[(size_t)v];
            ImGui::DockBuilderDockWindow(ViewWindowName(state, tab).c_str(), id);
            ui.view(tab.id).shown = true;
            placed[(size_t)v] = 1;
            if (v == node.selected) selections.emplace_back(id, ViewWindowId(tab.id));
            if (firstViewLeaf == 0) firstViewLeaf = id;
            }
    }
};

} // namespace

app::Layout CaptureLayout(const app::AppState& state, const UiState& ui)
{
    app::Layout layout;
    std::unordered_map<ImGuiID, int> viewByWindow;
    for (const app::Tab& tab : state.tabs)
        {
        viewByWindow[ViewWindowId(tab.id)] = (int)layout.views.size();
        layout.views.push_back(app::LayoutView{ tab.path, tab.viewMode, tab.pinned });
        }
    layout.active = std::clamp(state.activeTab, 0, std::max(0, (int)state.tabs.size() - 1));
    if (const ImGuiDockNode* root = ImGui::DockBuilderGetNode(ui.dockspaceId))
        CaptureNode(root, viewByWindow, layout.root);
    return layout;
}

void ApplyLayout(app::AppState& state, UiState& ui, const app::Layout& layout, ImVec2 size)
{
    // The layout is the whole workspace: the views open now give way to
    // its views, pinned or not, and image viewers close with them.
    while (!state.fileViews.empty()) CloseFileView(state, ui, state.fileViews.back().id);
    for (const app::Tab& tab : state.tabs) app::treescan::Cancel(tab.id);
    state.tabs.clear();
    ui.views.clear();

    int missing = 0;
    for (const app::LayoutView& v : layout.views)
        {
        // A folder that is gone opens on This PC rather than vanishing, so
        // the tree keeps its shape.
        app::Tab& tab = state.tabs[(size_t)app::OpenTab(state, v.path)];
        if (tab.path != app::NormalizePath(v.path)) ++missing;
        tab.viewMode = v.viewMode;
        tab.pinned = v.pinned;
        }
    if (state.tabs.empty()) app::OpenTab(state, "");

    const ImGuiID dockspaceId = ui.dockspaceId;
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, size);
    ImGuiID root = dockspaceId;
    if (!HasNav(layout.root))
        {
        // A layout with nowhere for the navigation pane gets the default
        // place for it, rather than leaving it floating.
        ImGuiID left = 0;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.22f, &left, &root);
        if (ImGuiDockNode* nav = ImGui::DockBuilderGetNode(left))
            nav->SetLocalFlags(nav->LocalFlags | ImGuiDockNodeFlags_HiddenTabBar);
        ImGui::DockBuilderDockWindow(kNavWindow, left);
        }
    TreeBuilder builder{ state, ui, std::vector<uint8_t>(state.tabs.size(), 0), {}, 0 };
    builder.Build(layout.root, root);
    // Views the tree does not place (floating when saved) join the first
    // stack of views.
    const ImGuiID fallback = builder.firstViewLeaf != 0 ? builder.firstViewLeaf : root;
    for (size_t i = 0; i < state.tabs.size(); ++i)
        if (i >= builder.placed.size() || !builder.placed[i])
            {
            ImGui::DockBuilderDockWindow(ViewWindowName(state, state.tabs[i]).c_str(), fallback);
            ui.view(state.tabs[i].id).shown = true;
            }
    ImGui::DockBuilderFinish(dockspaceId);
    // The tab each stack showed in front. The windows do not exist yet, so
    // this is set on the node the way loading imgui.ini sets it, and the
    // tab bar honours it when they arrive.
    for (const auto& [node, window] : builder.selections)
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(node)) n->SelectedTabId = TabIdOf(window);

    state.activeTab = std::clamp(layout.active, 0, (int)state.tabs.size() - 1);
    ui.view(state.active().id).wantFocus = true;
    ui.activeFileView = -1;
    if (missing > 0)
        state.statusMessage = std::to_string(missing) + (missing == 1 ? " folder" : " folders") + " in the layout no longer exist; shown as This PC";
}

void HandleLayoutRequests(app::AppState& state, UiState& ui, ImVec2 size)
{
    if (!ui.layoutToSave.empty())
        {
        const std::string path = std::move(ui.layoutToSave);
        ui.layoutToSave.clear();
        std::string error;
        if (app::SaveLayoutFile(path, CaptureLayout(state, ui), error))
            {
            state.currentLayout = path;
            app::AddRecentLayout(state.settings, path);
            state.statusMessage = "Layout saved to " + path;
            app::SaveAppState(state);
            }
        else
            {
            ShowError(ui, error);
            }
        }
    if (!ui.layoutToOpen.empty())
        {
        const std::string path = std::move(ui.layoutToOpen);
        ui.layoutToOpen.clear();
        app::Layout layout;
        std::string error;
        if (!app::LoadLayoutFile(path, layout, error))
            {
            // Gone or damaged: it is no use in the recent list either.
            app::RemoveRecentLayout(state.settings, path);
            app::SaveAppState(state);
            ShowError(ui, error);
            return;
            }
        state.statusMessage.clear();
        ApplyLayout(state, ui, layout, size);
        state.currentLayout = path;
        app::AddRecentLayout(state.settings, path);
        app::SaveAppState(state);
        }
}

} // namespace ui
