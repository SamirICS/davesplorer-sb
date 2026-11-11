// Extracting archives: the menu's three ways in, the double-click question,
// and the window that shows an extraction going and asks what it needs to
// know (a passphrase, what to do about names already taken).
#include "ui/MainWindow.h"
#include "ui/IconsMaterialDesign.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include "app/Extract.h"
#include "app/PathUtil.h"
#include "app/Settings.h"
#include "platform/Shell.h"

#include "imgui.h"
#include "misc/cpp/imgui_stdlib.h"

#include <string>
#include <vector>

namespace ui
{

namespace
{

constexpr const char* kExtractingTitle = "Extracting###extractjob";

float Scaled(float px)
{
    return px * ImGui::GetStyle().FontScaleMain * ImGui::GetStyle().FontScaleDpi;
}

bool EscapePressed()
{
    return !ImGui::IsWindowAppearing() && ImGui::IsKeyPressed(ImGuiKey_Escape);
}

bool EnterPressed()
{
    return !ImGui::IsWindowAppearing() &&
           (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
}

std::string Quoted(const std::string& path)
{
    return "'" + app::PathName(path) + "'";
}

void DrawPassphrase(const app::extractjob::Status& status, UiState& ui)
{
    ImGui::TextUnformatted(ICON_MD_LOCK);
    ImGui::SameLine();
    ImGui::TextWrapped("%s is protected by a passphrase.", Quoted(status.archive).c_str());
    if (status.wrongPassphrase)
        {
        ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().danger);
        ImGui::TextUnformatted("That passphrase does not open it.");
        ImGui::PopStyleColor();
        }
    ImGui::SetNextItemWidth(Scaled(360.0f));
    if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) ImGui::SetKeyboardFocusHere();
    bool submit = ImGui::InputText("##passphrase", &ui.extractPassphrase,
                                   ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    if (ImGui::Button("OK", ImVec2(100, 0))) submit = true;
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(100, 0)) || EscapePressed();
    if (submit && !ui.extractPassphrase.empty())
        {
        app::extractjob::AnswerPassphrase(true, ui.extractPassphrase);
        ui.extractPassphrase.clear();
        }
    else if (cancel)
        {
        app::extractjob::AnswerPassphrase(false, "");
        ui.extractPassphrase.clear();
        }
}

void DrawConflict(const app::extractjob::Status& status)
{
    const size_t n = status.conflicts.size();
    ImGui::TextUnformatted(ICON_MD_CONTENT_COPY);
    ImGui::SameLine();
    const std::string where = app::PathName(status.destDir);
    if (n == 1) ImGui::TextWrapped("'%s' already exists in %s.", status.conflicts[0].c_str(), where.c_str());
    else ImGui::TextWrapped("%zu items from %s already exist in %s:", n, Quoted(status.archive).c_str(), where.c_str());
    for (size_t i = 0; n > 1 && i < n && i < 5; ++i) MutedText(("    " + status.conflicts[i]).c_str());
    if (n > 5) MutedText(("    ...and " + std::to_string(n - 5) + " more").c_str());
    MutedText("Replaced items go to the Recycle Bin.");
    ImGui::Spacing();
    if (ImGui::Button("Replace", ImVec2(100, 0))) app::extractjob::AnswerConflict(app::ConflictChoice::Replace);
    ImGui::SameLine();
    if (ImGui::Button("Skip", ImVec2(100, 0))) app::extractjob::AnswerConflict(app::ConflictChoice::Skip);
    ImGui::SameLine();
    if (ImGui::Button("Keep both", ImVec2(100, 0)) || EnterPressed()) app::extractjob::AnswerConflict(app::ConflictChoice::KeepBoth);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || EscapePressed()) app::extractjob::AnswerConflict(app::ConflictChoice::Cancel);
}

void DrawProgress(const app::extractjob::Status& status)
{
    ImGui::TextUnformatted(ICON_MD_UNARCHIVE);
    ImGui::SameLine();
    std::string line = "Extracting " + Quoted(status.archive);
    if (status.count > 1) line += "  (" + std::to_string(status.index) + " of " + std::to_string(status.count) + ")";
    ImGui::TextUnformatted(line.c_str());
    MutedText(("to " + app::PathName(status.destDir)).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status.destDir.c_str());
    ImGui::ProgressBar(status.progress, ImVec2(Scaled(360.0f), 0));
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || EscapePressed()) app::extractjob::Cancel();
}

} // namespace

std::string ExtractToFolderLabel(const std::vector<std::string>& archives)
{
    if (archives.size() != 1) return "Extract each to its own folder";
    return "Extract to \"" + app::ArchiveStem(app::PathName(archives[0])) + "/\"";
}

void StartExtract(const app::Settings& settings, UiState& ui, const std::vector<std::string>& archives, ExtractTarget target)
{
    if (archives.empty()) return;
    if (target == ExtractTarget::Choose)
        {
        if (ui.fileDialogOpen) return;
        ui.extractArchives = archives;
        ui.fileDialogRequest = FileDialog::ExtractDest;
        return;
        }
    std::vector<app::ExtractRequest> requests;
    for (const std::string& a : archives)
        requests.push_back(app::MakeExtractRequest(settings, a, app::ParentPath(a), target == ExtractTarget::OwnFolder));
    app::extractjob::Start(requests);
}

void TickExtract(app::AppState& state, UiState& ui)
{
    if (!ui.extractDestChosen.empty())
        {
        std::vector<app::ExtractRequest> requests;
        for (const std::string& a : ui.extractArchives)
            requests.push_back(app::MakeExtractRequest(state.settings, a, ui.extractDestChosen, false));
        app::extractjob::Start(requests);
        ui.extractDestChosen.clear();
        ui.extractArchives.clear();
        }
    app::extractjob::Tick(state, ui.extractMessages);
}

void DrawExtractDialogs(app::AppState& state, UiState& ui)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 centre(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);

    // The double-click question.
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Extract", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
        const std::vector<std::string>& archives = ui.extractArchives;
        ImGui::TextUnformatted(ICON_MD_FOLDER_ZIP);
        ImGui::SameLine();
        if (archives.size() == 1) ImGui::TextWrapped("%s is an archive. Extract it?", Quoted(archives[0]).c_str());
        else ImGui::TextWrapped("Extract these %zu archives?", archives.size());
        ImGui::Spacing();
        int choice = -1;
        if (ImGui::Button(ExtractToFolderLabel(archives).c_str()) || EnterPressed()) choice = (int)app::ArchiveDoubleClick::ExtractToFolder;
        ImGui::SameLine();
        if (ImGui::Button("Extract here")) choice = (int)app::ArchiveDoubleClick::ExtractHere;
        ImGui::SameLine();
        if (ImGui::Button("Open with default program")) choice = (int)app::ArchiveDoubleClick::Open;
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel") || EscapePressed();
        ImGui::Checkbox("Always do this on double-click", &ui.extractRemember);
        MutedText("View > Double-click on an archive changes it back.");
        if (choice >= 0)
            {
            if (ui.extractRemember) state.settings.archiveDoubleClick = (app::ArchiveDoubleClick)choice;
            const std::vector<std::string> chosen = archives;
            ui.extractArchives.clear();
            if (choice == (int)app::ArchiveDoubleClick::Open)
                {
                std::string error;
                for (const std::string& a : chosen)
                    if (!platform::OpenWithShell(a, error)) ShowError(ui, error);
                }
            else
                {
                StartExtract(state.settings, ui, chosen, choice == (int)app::ArchiveDoubleClick::ExtractHere ? ExtractTarget::Here
                                                                                             : ExtractTarget::OwnFolder);
                }
            }
        if (choice >= 0 || cancel)
            {
            ui.extractRemember = false;
            if (cancel) ui.extractArchives.clear();
            ImGui::CloseCurrentPopup();
            }
        ImGui::EndPopup();
        }

    // An extraction going: shown while the worker has one, over anything
    // but another dialog (opening it then would close that one).
    const app::extractjob::Status status = app::extractjob::Poll();
    const bool busy = status.stage != app::extractjob::Stage::Idle;
    if (busy)
        {
        ui.animating = true;
        if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && !ui.dialogRequested)
            ImGui::OpenPopup(kExtractingTitle);
        }
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kExtractingTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
        {
        switch (status.stage)
            {
            case app::extractjob::Stage::NeedPassphrase:     DrawPassphrase(status, ui); break;
            case app::extractjob::Stage::NeedConflictChoice: DrawConflict(status); break;
            case app::extractjob::Stage::Working:            DrawProgress(status); break;
            case app::extractjob::Stage::Idle:               ImGui::CloseCurrentPopup(); break;
            }
        ImGui::EndPopup();
        }

    // What went wrong, once it is all over.
    if (!busy && !ui.extractMessages.empty() &&
        !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) && !ui.dialogRequested)
        {
        std::string text;
        for (const std::string& m : ui.extractMessages) text += (text.empty() ? "" : "\n\n") + m;
        ui.extractMessages.clear();
        ShowError(ui, text);
        }
}

} // namespace ui
