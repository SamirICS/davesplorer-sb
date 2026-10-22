// The Programs window: every compiled program under a folder, one group
// per project, newest build first, with where each one lives. Double-click
// runs a program in its own folder; the context menu also runs it in a
// terminal, opens the folder holding it, or copies its path.
#include "ui/MainWindow.h"
#include "ui/IconsMaterialDesign.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include "app/Format.h"
#include "app/PathUtil.h"
#include "app/ProgramScan.h"
#include "platform/FileSystem.h"
#include "platform/Shell.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace ui
{

std::string ProgramsWindowName(const app::AppState& state, const app::ProgramsView& view)
{
    return ICON_MD_SETTINGS_APPLICATIONS " Programs: " + app::LocationTitle(state, view.root) + "###programs" +
           std::to_string(view.id);
}

int OpenProgramsView(app::AppState& state, UiState& ui, const std::string& root)
{
    ImGuiID target = 0;
    if (!state.tabs.empty()) target = ui.view(state.active().id).dockId;
    if (target == 0) target = ui.defaultViewDock;

    const size_t before = state.programViews.size();
    const int index = app::OpenProgramsView(state, root);
    ProgramsViewUi& v = ui.programsView(state.programViews[(size_t)index].id);
    v.wantFocus = true;
    if (state.programViews.size() != before) v.dockHint = target;
    return index;
}

void CloseProgramsView(app::AppState& state, UiState& ui, int id)
{
    ui.programViews.erase(id);
    for (int i = 0; i < (int)state.programViews.size(); ++i)
        if (state.programViews[(size_t)i].id == id)
            {
            app::CloseProgramsView(state, i);
            break;
            }
}

namespace
{

// A path below the root as an id: '/' splits test-engine id paths.
std::string IdOf(const std::string& key)
{
    std::string id = key;
    for (char& c : id)
        if (c == '/' || c == '\\') c = '|';
    return id;
}

// The program's path below the scanned folder: what a row is known by, so
// two programs of the same name in different builds stay apart.
std::string RowKey(const app::ProgramsView& view, const app::Program& p)
{
    if (p.path.size() > view.root.size() + 1 && p.path.compare(0, view.root.size(), view.root) == 0)
        return IdOf(p.path.substr(view.root.size() + 1));
    return IdOf(p.path);
}

void RunProgram(UiState& ui, const app::Program& p, bool inTerminal)
{
    std::string error;
    const bool ok = inTerminal ? platform::RunInTerminal(p.path, error) : platform::RunExecutable(p.path, "", error);
    if (!ok) ShowError(ui, error);
}

void DrawRowMenu(app::AppState& state, UiState& ui, const app::Program& p)
{
    // The dash is asked as the menu opens, not every frame: a GSettings read.
    if (ImGui::IsWindowAppearing())
        {
        ui.pinQueryPath = p.path;
        ui.pinQueryCanPin = platform::CanPinApplications();
        ui.pinQueryPinned = ui.pinQueryCanPin && platform::IsPinned(p.path);
        }
    const bool canPin = ui.pinQueryPath == p.path && ui.pinQueryCanPin;
    const bool pinned = canPin && ui.pinQueryPinned;
    if (ImGui::MenuItem("Run")) RunProgram(ui, p, false);
    if (ImGui::MenuItem("Run in terminal")) RunProgram(ui, p, true);
    ImGui::Separator();
    if (ImGui::MenuItem("Open containing folder"))
        {
        const int index = OpenView(state, ui, app::ParentPath(p.path));
        state.tabs[(size_t)index].selectOnLoad = p.name;
        }
    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(p.path.c_str());
    if (ImGui::MenuItem(pinned ? "Unpin from dash" : "Pin to dash", nullptr, false, canPin))
        {
        std::string error;
        const bool ok = pinned ? platform::UnpinApplication(p.path, error) : platform::PinApplication(p.path, error);
        if (ok) ui.pinQueryPinned = !pinned;
        else ShowError(ui, error);
        }
}

// Every folder open or closed, including those a scan has yet to find.
void ExpandAll(ProgramsViewUi& v)
{
    v.collapsed.clear();
    v.collapsedByDefault = false;
}

void CollapseAll(ProgramsViewUi& v)
{
    v.collapsed.clear();
    v.collapsedByDefault = true;
}

bool IsCollapsed(const ProgramsViewUi& v, const std::string& key)
{
    const auto it = v.collapsed.find(key);
    return it != v.collapsed.end() ? it->second : v.collapsedByDefault;
}

// A folder and every folder below it, opened or closed together.
void SetSubtree(const std::vector<app::ProgramNode>& nodes, int n, ProgramsViewUi& v, bool collapsed)
{
    v.collapsed[nodes[(size_t)n].key] = collapsed;
    for (int c : nodes[(size_t)n].folders) SetSubtree(nodes, c, v, collapsed);
}

// One drawn row: a folder (program -1) or a program, at its depth.
struct Line
{
    int node;
    int program;
    int depth;
};

// The tree as the rows on show, folders before the programs beside them.
// A search opens every folder: a match hidden in a closed one is no match.
void Flatten(const std::vector<app::ProgramNode>& nodes, int n, int depth, const ProgramsViewUi& v, bool filtering,
             std::vector<Line>& out)
{
    const app::ProgramNode& node = nodes[(size_t)n];
    if (n != 0)
        {
        out.push_back({ n, -1, depth });
        if (!filtering && IsCollapsed(v, node.key)) return;
        }
    const int inner = n == 0 ? 0 : depth + 1;
    for (int c : node.folders) Flatten(nodes, c, inner, v, filtering, out);
    for (int p : node.programs) out.push_back({ n, p, inner });
}

void DrawFolderMenu(app::AppState& state, UiState& ui, const app::ProgramsView& view,
                    const std::vector<app::ProgramNode>& nodes, int n, ProgramsViewUi& v)
{
    const std::string path = app::JoinPath(view.root, nodes[(size_t)n].key);
    if (ImGui::MenuItem("Expand all under here")) SetSubtree(nodes, n, v, false);
    if (ImGui::MenuItem("Collapse all under here")) SetSubtree(nodes, n, v, true);
    ImGui::Separator();
    if (ImGui::MenuItem("Open folder")) OpenView(state, ui, path);
    if (ImGui::MenuItem("Copy path")) ImGui::SetClipboardText(path.c_str());
}

void DrawToolbar(app::ProgramsView& view, ProgramsViewUi& v, size_t shown, size_t groups)
{
    if (v.filterWantFocus || ImGui::IsWindowAppearing())
        {
        ImGui::SetKeyboardFocusHere();
        v.filterWantFocus = false;
        }
    ImGui::SetNextItemWidth(260.0f * ImGui::GetStyle().FontScaleMain * ImGui::GetStyle().FontScaleDpi);
    ImGui::InputTextWithHint("##filter", ICON_MD_SEARCH " Name or path", &view.filter);
    ImGui::SameLine();
    if (IconButton("Rescan", ICON_MD_REFRESH, "Look again")) app::RescanPrograms(view);
    ImGui::SameLine();
    if (IconButton("ExpandAll", ICON_MD_UNFOLD_MORE, "Expand all folders")) ExpandAll(v);
    ImGui::SameLine();
    if (IconButton("CollapseAll", ICON_MD_UNFOLD_LESS, "Collapse all folders")) CollapseAll(v);
    ImGui::SameLine();
    std::string status;
    const std::string programs = app::WithThousands(shown) + (shown == 1 ? " program" : " programs");
    if (view.scanning)
        status = "Scanning... " + app::WithThousands(view.folders) + " folders, " + programs;
    else
        status = programs + " in " + app::WithThousands(groups) + (groups == 1 ? " project" : " projects");
    if (!view.filter.empty() && !view.scanning)
        status += " matching";
    ImGui::AlignTextToFramePadding();
    MutedText(status.c_str());
    if (view.truncated)
        {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().danger);
        ImGui::TextUnformatted("(stopped early: too many folders or too deep)");
        ImGui::PopStyleColor();
        }
    FaintText(view.root.c_str());
}

} // namespace

bool DrawProgramsView(app::AppState& state, UiState& ui, int index)
{
    app::ProgramsView& view = state.programViews[(size_t)index];
    ProgramsViewUi& v = ui.programsView(view.id);

    if (v.wantFocus)
        {
        ImGui::SetNextWindowFocus();
        v.wantFocus = false;
        v.filterWantFocus = true;
        }
    if (v.dockHint != 0)
        {
        ImGui::SetNextWindowDockID(v.dockHint, ImGuiCond_Always);
        v.dockHint = 0;
        }
    else if (!v.shown)
        {
        ImGui::SetNextWindowDockID(ui.defaultViewDock, ImGuiCond_FirstUseEver);
        }
    v.shown = true;

    // Its tab in the theme's accent, so it is told apart from the folder
    // views at a glance. Docking reads tab colours as Begin runs, so they
    // are pushed for Begin alone; the title bar gets them too, for when it
    // floats.
    const Palette& pal = CurrentPalette();
    const std::pair<ImGuiCol, ImU32> tabColors[] = {
        { ImGuiCol_Tab, pal.programsTab },
        { ImGuiCol_TabDimmed, pal.programsTab },
        { ImGuiCol_TabHovered, pal.programsTabHovered },
        { ImGuiCol_TabSelected, pal.programsTabSelected },
        { ImGuiCol_TabDimmedSelected, pal.programsTabSelected },
        { ImGuiCol_TabSelectedOverline, pal.programsTabOverline },
        { ImGuiCol_TabDimmedSelectedOverline, pal.programsTabOverline },
        { ImGuiCol_TitleBg, pal.programsTab },
        { ImGuiCol_TitleBgActive, pal.programsTabSelected },
        { ImGuiCol_TitleBgCollapsed, pal.programsTab },
    };
    for (const auto& [col, value] : tabColors) ImGui::PushStyleColor(col, value);
    const std::string name = ProgramsWindowName(state, view);
    bool open = true;
    const bool visible = ImGui::Begin(name.c_str(), &open, ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleColor((int)std::size(tabColors));
    // Right-click on the tab (its title bar when floating): straight after
    // Begin the last item is that tab, even for a tab not in front.
    if (ImGui::BeginPopupContextItem("###programstab"))
        {
        if (ImGui::MenuItem("Expand all")) ExpandAll(v);
        if (ImGui::MenuItem("Collapse all")) CollapseAll(v);
        ImGui::Separator();
        if (ImGui::MenuItem("Rescan")) app::RescanPrograms(view);
        ImGui::EndPopup();
        }
    if (!visible)
        {
        ImGui::End();
        return !open;
        }
    v.dockId = ImGui::GetWindowDockID();
    if (view.scanning) ui.animating = true;   // results keep arriving without input

    const std::vector<app::ProgramNode> nodes = app::BuildProgramTree(view.programs, view.filter);
    DrawToolbar(view, v, nodes[0].count, nodes[0].folders.size());

    if (nodes[0].count == 0)
        {
        ImGui::Spacing();
        if (view.scanning) MutedText("Looking for programs...");
        else if (!view.filter.empty()) MutedText(("No programs match '" + view.filter + "'.").c_str());
        else MutedText("No programs found under this folder.");
        ImGui::End();
        return !open;
        }

    // One flat list of rows, folders and programs together, so a clipper
    // keeps thousands of test binaries cheap to draw.
    const bool filtering = !view.filter.empty();
    std::vector<Line> lines;
    Flatten(nodes, 0, 0, v, filtering, lines);

    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("Programs", 4, flags))
        {
        const float em = ImGui::GetFontSize();
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide, 1.4f);
        ImGui::TableSetupColumn("Location", 0, 1.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
        ImGui::TableSetupColumn("Built", ImGuiTableColumnFlags_WidthFixed, em * 8.5f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)lines.size());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                const Line& line = lines[(size_t)i];
                const float indent = (float)line.depth * em;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (indent > 0.0f) ImGui::Indent(indent);
                if (line.program < 0)
                    {
                    // A folder: click to open or close it, right-click for
                    // the whole subtree at once. Projects in the accent.
                    const app::ProgramNode& node = nodes[(size_t)line.node];
                    const bool closed = !filtering && IsCollapsed(v, node.key);
                    const std::string label = std::string(closed ? ICON_MD_CHEVRON_RIGHT : ICON_MD_EXPAND_MORE) + " " +
                                              ICON_MD_FOLDER " " + node.name + "  (" + std::to_string(node.count) + ")" +
                                              "###folder|" + IdOf(node.key);
                    ImGui::PushStyleColor(ImGuiCol_Text, line.depth == 0 ? CurrentPalette().accent : CurrentPalette().text);
                    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && !filtering)
                        v.collapsed[node.key] = !closed;
                    ImGui::PopStyleColor();
                    if (ImGui::BeginPopupContextItem())
                        {
                        DrawFolderMenu(state, ui, view, nodes, line.node, v);
                        ImGui::EndPopup();
                        }
                    if (indent > 0.0f) ImGui::Unindent(indent);
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    ImGui::TableNextColumn();
                    FaintText(platform::FormatDateTime(node.newest).c_str());
                    continue;
                    }
                const app::Program& p = view.programs[(size_t)line.program];
                const std::string key = RowKey(view, p);
                ImGui::PushID(key.c_str());
                const bool selected = v.selectedPath == p.path;
                if (ImGui::Selectable(("###" + key).c_str(), selected,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick |
                                      ImGuiSelectableFlags_AllowOverlap))
                    {
                    v.selectedPath = p.path;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) RunProgram(ui, p, false);
                    }
                if (ImGui::BeginPopupContextItem("row"))
                    {
                    v.selectedPath = p.path;
                    DrawRowMenu(state, ui, p);
                    ImGui::EndPopup();
                    }
                ImGui::SameLine();
                ImGui::TextUnformatted(ICON_MD_SETTINGS_APPLICATIONS);
                ImGui::SameLine();
                ImGui::TextUnformatted(p.name.c_str());
                if (indent > 0.0f) ImGui::Unindent(indent);
                ImGui::TableNextColumn();
                MutedText(p.relDir.empty() ? "." : p.relDir.c_str());
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", p.path.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(app::HumanSize(p.size).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(platform::FormatDateTime(p.modified).c_str());
                ImGui::PopID();
                }
        ImGui::EndTable();
        }

    // Enter runs the row last clicked, while this window has the keyboard
    // and nobody is typing in the search box.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        for (const app::Program& p : view.programs)
            if (p.path == v.selectedPath)
                {
                RunProgram(ui, p, false);
                break;
                }
    ImGui::End();
    return !open;
}

} // namespace ui
