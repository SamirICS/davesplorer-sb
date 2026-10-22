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

#include <string>
#include <vector>

namespace ui
{

std::string ProgramsWindowName(const app::AppState& state, const app::ProgramsView& view)
{
    return "Programs: " + app::LocationTitle(state, view.root) + "###programs" + std::to_string(view.id);
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

// The program's path below the scanned folder: what a row is known by, so
// two programs of the same name in different builds stay apart. Slashes
// become '|' as in the file list's nested rows, since an id path splits on
// '/'.
std::string RowKey(const app::ProgramsView& view, const app::Program& p)
{
    std::string key = p.path;
    if (p.path.size() > view.root.size() + 1 && p.path.compare(0, view.root.size(), view.root) == 0)
        key = p.path.substr(view.root.size() + 1);
    for (char& c : key)
        if (c == '/' || c == '\\') c = '|';
    return key;
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

    const std::string name = ProgramsWindowName(state, view);
    bool open = true;
    const bool visible = ImGui::Begin(name.c_str(), &open, ImGuiWindowFlags_NoCollapse);
    if (!visible)
        {
        ImGui::End();
        return !open;
        }
    v.dockId = ImGui::GetWindowDockID();
    if (view.scanning) ui.animating = true;   // results keep arriving without input

    const std::vector<app::ProgramGroup> groups = app::GroupPrograms(view.programs, view.filter);
    size_t shown = 0;
    for (const app::ProgramGroup& g : groups) shown += g.rows.size();
    DrawToolbar(view, v, shown, groups.size());

    if (groups.empty())
        {
        ImGui::Spacing();
        if (view.scanning) MutedText("Looking for programs...");
        else if (!view.filter.empty()) MutedText(("No programs match '" + view.filter + "'.").c_str());
        else MutedText("No programs found under this folder.");
        ImGui::End();
        return !open;
        }

    // One flat list of lines, group headers and rows together, so a clipper
    // keeps thousands of test binaries cheap to draw.
    struct Line { int group; int row; };   // row -1: the group's header
    std::vector<Line> lines;
    const bool filtering = !view.filter.empty();
    for (int g = 0; g < (int)groups.size(); ++g)
        {
        lines.push_back({ g, -1 });
        // A search opens every group: a hidden match is no match at all.
        if (!filtering && v.collapsed[groups[(size_t)g].project]) continue;
        for (int r : groups[(size_t)g].rows) lines.push_back({ g, r });
        }

    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("Programs", 4, flags))
        {
        const float em = ImGui::GetFontSize();
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_NoHide, 1.0f);
        ImGui::TableSetupColumn("Location", 0, 1.4f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, em * 5.5f);
        ImGui::TableSetupColumn("Built", ImGuiTableColumnFlags_WidthFixed, em * 8.5f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin((int)lines.size());
        while (clipper.Step())
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                {
                const Line& line = lines[(size_t)i];
                const app::ProgramGroup& g = groups[(size_t)line.group];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (line.row < 0)
                    {
                    const std::string project = g.project.empty() ? "(this folder)" : g.project;
                    const bool closed = !filtering && v.collapsed[g.project];
                    const std::string label = std::string(closed ? ICON_MD_CHEVRON_RIGHT : ICON_MD_EXPAND_MORE) + " " +
                                              ICON_MD_FOLDER " " + project + "  (" + std::to_string(g.rows.size()) + ")" +
                                              "###group|" + g.project;
                    ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().accent);
                    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns) && !filtering)
                        v.collapsed[g.project] = !closed;
                    ImGui::PopStyleColor();
                    continue;
                    }
                const app::Program& p = view.programs[(size_t)line.row];
                const std::string key = RowKey(view, p);
                ImGui::PushID(key.c_str());
                ImGui::Indent(em);
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
                ImGui::Unindent(em);
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
