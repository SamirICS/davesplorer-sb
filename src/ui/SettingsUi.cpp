// The Settings menu on the menu bar, and the Settings window it opens: the
// options that are not about how one view looks.
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include "app/Extract.h"
#include "app/PathUtil.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <string>

namespace ui
{

namespace
{

constexpr const char* kSettingsWindow = "Settings###settings";

const char* const kDoubleClickNames[] = { "Ask", "Extract here", "Extract to its own folder", "Open with default program" };

float Scaled(float px)
{
    return px * ImGui::GetStyle().FontScaleMain * ImGui::GetStyle().FontScaleDpi;
}

void SectionHeading(const char* text)
{
    ImGui::Spacing();
    ImGui::SeparatorText(text);
}

} // namespace

std::string MoveOriginalsLabel(const app::Settings& s)
{
    return "Move archives to '" + s.originalsFolder + "' after extracting";
}

void DrawSettingsMenu(app::AppState& state, UiState& ui)
{
    if (!ImGui::BeginMenu("Settings")) return;
    app::Settings& s = state.settings;
#ifdef __linux__
    ImGui::MenuItem((MoveOriginalsLabel(s) + "###moveoriginals").c_str(), nullptr, &s.moveOriginals);
    ImGui::MenuItem("Also extract archives found inside", nullptr, &s.extractNested);
    ImGui::Separator();
#endif
    ImGui::MenuItem("Vertical tabs", nullptr, &s.verticalTabs);
    ImGui::Separator();
    if (ImGui::MenuItem("All settings...")) ui.showSettings = true;
    ImGui::EndMenu();
}

void DrawSettingsWindow(app::AppState& state, UiState& ui)
{
    if (!ui.showSettings) return;
    app::Settings& s = state.settings;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(Scaled(520.0f), 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin(kSettingsWindow, &ui.showSettings,
                      ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize))
        {
        ImGui::End();
        return;
        }
    // The name field edits a copy: a half-typed or invalid name never
    // reaches the setting.
    if (ImGui::IsWindowAppearing())
        {
        ui.settingsFolderText = s.originalsFolder;
        ui.settingsFolderError.clear();
        }

#ifdef __linux__
    SectionHeading("Extracting archives");
    ImGui::Checkbox("Move the archive into a folder beside it once extracted", &s.moveOriginals);
    ImGui::BeginDisabled(!s.moveOriginals);
    ImGui::Indent();
    ImGui::TextUnformatted("Folder name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Scaled(220.0f));
    if (ImGui::InputText("##originalsfolder", &ui.settingsFolderText))
        {
        std::string reason;
        if (app::IsValidFileName(ui.settingsFolderText, reason))
            {
            s.originalsFolder = ui.settingsFolderText;
            ui.settingsFolderError.clear();
            }
        else
            {
            ui.settingsFolderError = reason;
            }
        }
    if (!ui.settingsFolderError.empty())
        {
        ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().danger);
        ImGui::TextWrapped("%s", ui.settingsFolderError.c_str());
        ImGui::PopStyleColor();
        }
    MutedText(("photos.zip goes to " + s.originalsFolder + "/photos.zip, beside where it was.").c_str());
    ImGui::Unindent();
    ImGui::EndDisabled();

    ImGui::Checkbox("Also extract archives found inside, each into its own folder", &s.extractNested);
    MutedText(("Followed " + std::to_string(app::kMaxNestedDepth) + " levels down.").c_str());

    ImGui::TextUnformatted("Double-click on an archive");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(Scaled(220.0f));
    int dc = (int)s.archiveDoubleClick;
    if (ImGui::Combo("##archivedoubleclick", &dc, kDoubleClickNames, IM_ARRAYSIZE(kDoubleClickNames)))
        s.archiveDoubleClick = (app::ArchiveDoubleClick)dc;
#endif

    SectionHeading("Tabs");
    ImGui::Checkbox("Vertical tabs: each pane lists its tabs down its left side", &s.verticalTabs);

    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(100, 0)) ||
        (!ImGui::IsWindowAppearing() && !ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused() &&
         ImGui::IsKeyPressed(ImGuiKey_Escape)))
        ui.showSettings = false;
    ImGui::End();
}

} // namespace ui
