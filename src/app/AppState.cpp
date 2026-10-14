#include "app/AppState.h"
#include "app/Listing.h"
#include "app/Navigation.h"
#include "app/PathUtil.h"
#include "app/TreeScan.h"
#include "platform/Clipboard.h"
#include "platform/Strings.h"

#include <algorithm>
#include <cstring>

namespace app
{

Tab& AppState::active()
{
    if (tabs.empty()) tabs.emplace_back();
    activeTab = std::clamp(activeTab, 0, (int)tabs.size() - 1);
    return tabs[(size_t)activeTab];
}

const Tab& AppState::active() const
{
    const int i = std::clamp(activeTab, 0, (int)tabs.size() - 1);
    return tabs[(size_t)i];
}

void InitAppState(AppState& state, const std::string& exeDir, const std::string& configDir)
{
    state.exeDir = exeDir;
    state.settingsFile = JoinPath(configDir.empty() ? exeDir : configDir, "settings.ini");
    state.layoutFile   = JoinPath(configDir.empty() ? exeDir : configDir, "imgui.ini");
    state.layoutsDir   = JoinPath(configDir.empty() ? exeDir : configDir, "layouts");
    LoadSettings(state.settingsFile, state.settings);

    state.quickAccess = platform::GetKnownFolders();
    state.drives      = platform::GetDrives();

    std::string start = state.settings.startPath;
    if (!start.empty() && !platform::IsDirectory(start)) start.clear();
    if (start.empty())
        {
        // First run, or the remembered folder is gone: the user's home is a
        // more useful place than This PC.
        for (const platform::KnownFolder& kf : state.quickAccess)
            if (kf.kind == platform::KnownFolderKind::Home) start = kf.path;
        }
    state.tabs.clear();
    state.unavailablePins.clear();
    // Each pinned view keeps the id it had, which is what imgui.ini knows
    // its window by, so it reopens docked where it was.
    int maxId = 0;
    for (const PinnedView& pin : state.settings.pinnedViews)
        {
        const bool taken = std::any_of(state.tabs.begin(), state.tabs.end(), [&](const Tab& t) { return t.id == pin.id; }) ||
                           std::any_of(state.unavailablePins.begin(), state.unavailablePins.end(), [&](const PinnedView& p) { return p.id == pin.id; });
        if (taken) continue;
        // Its id stays reserved either way, so no new view takes it.
        maxId = std::max(maxId, pin.id);
        if (!pin.path.empty() && !platform::IsDirectory(pin.path))
            {
            state.unavailablePins.push_back(pin);
            continue;
            }
        state.nextTabId = pin.id;
        Tab& tab = state.tabs[(size_t)OpenTab(state, pin.path)];
        tab.pinned = true;
        tab.viewMode = pin.viewMode;
        }
    state.nextTabId = maxId + 1;
    for (int i = 0; i < (int)state.tabs.size(); ++i)
        if (state.tabs[(size_t)i].path == NormalizePath(start))
            {
            state.activeTab = i;
            return;
            }
    OpenTab(state, start);
}

bool ApplyStartArgument(AppState& state, const std::string& argument)
{
    std::string raw = argument;
    // Explorer's own switches, so a program that runs
    // `explorer.exe /select,"file"` or `/e,folder` can run davesplorer.exe
    // with the same arguments unchanged. /select on a file selects it;
    // on a folder it shows the parent with the folder selected.
    bool selectMode = false;
    for (const char* prefix : { "/select,", "/e,", "/root," })
        {
        const size_t n = strlen(prefix);
        if (raw.size() >= n && platform::ToLowerAscii(raw.substr(0, n)) == prefix)
            {
            selectMode = prefix[1] == 's';
            raw = raw.substr(n);
            break;
            }
        }
#ifdef _WIN32
    std::string path = NormalizePath(raw);
#else
    std::string path = raw;
#endif
    // Explorer quotes paths it passes along; a shell verb may leave the
    // quotes in.
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = NormalizePath(path.substr(1, path.size() - 2));

    path = NormalizePath(path);

    std::string folder, select;
    if (!path.empty() && platform::IsDirectory(path) && !(selectMode && !IsDriveRoot(path)))
        {
        folder = path;
        }
    else if (!path.empty() && platform::PathExists(path))
        {
        folder = ParentPath(path);
        select = PathName(path);
        }

    // The pinned views stay; the argument takes the place of the rest.
    std::erase_if(state.tabs, [](const Tab& t) { return !t.pinned; });
    OpenTab(state, folder);
    state.active().selectOnLoad = select;
    return !folder.empty();
}

void SaveAppState(AppState& state)
{
    if (state.settings.rememberLastPath && !state.tabs.empty())
        state.settings.startPath = state.active().path;
    state.settings.pinnedViews.clear();
    for (const Tab& tab : state.tabs)
        if (tab.pinned) state.settings.pinnedViews.push_back(PinnedView{ tab.id, tab.path, tab.viewMode });
    for (const PinnedView& pin : state.unavailablePins)
        state.settings.pinnedViews.push_back(pin);
    std::string error;
    SaveSettings(state.settingsFile, state.settings, error);
}

void SetPinned(AppState& state, int index, bool pinned)
{
    if (index < 0 || index >= (int)state.tabs.size()) return;
    state.tabs[(size_t)index].pinned = pinned;
    SaveAppState(state);
}

int OpenTab(AppState& state, const std::string& path)
{
    Tab tab;
    tab.id = state.nextTabId++;
    tab.viewMode = state.settings.viewMode;
    std::string error;
    if (!NavigateTo(tab, path, error)) NavigateTo(tab, "", error);
    state.tabs.push_back(std::move(tab));
    state.activeTab = (int)state.tabs.size() - 1;
    return state.activeTab;
}

void CloseTab(AppState& state, int index)
{
    if (index < 0 || index >= (int)state.tabs.size()) return;
    if (state.tabs.size() == 1)
        {
        // Explorer closes the window when its last tab goes; so do we.
        state.quitRequested = true;
        return;
        }
    // Nothing is left to receive what the walker is still reading, and the
    // id will be handed to a tab opened later.
    treescan::Cancel(state.tabs[(size_t)index].id);
    state.tabs.erase(state.tabs.begin() + index);
    if (state.activeTab >= (int)state.tabs.size()) state.activeTab = (int)state.tabs.size() - 1;
    else if (state.activeTab > index) --state.activeTab;
}

void TickAppState(AppState& state)
{
    // Folders read in the background land first, so a view that is filling
    // in never skips a frame behind a reload.
    PumpTreeScan(state);
    for (Tab& tab : state.tabs)
        ReloadIfNeeded(tab, state.settings);

    state.clipboardHasFiles = platform::ClipboardHasFiles();
    // Someone else wrote the clipboard: the ghosted cut list no longer
    // describes what a paste would do.
    if (!state.clipboard.paths.empty() && platform::ClipboardSequence() != state.clipboardOwnedSeq)
        state.clipboard = Clipboard{};
}

void RefreshAll(AppState& state)
{
    for (Tab& tab : state.tabs)
        tab.needsReload = true;
}

std::string LocationTitle(const AppState& state, const std::string& path)
{
    if (IsDriveRoot(path))
        for (const platform::DriveInfo& d : state.drives)
            if (d.root == path) return d.displayName;
    return PathName(path);
}

} // namespace app
