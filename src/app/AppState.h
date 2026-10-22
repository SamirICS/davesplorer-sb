// Everything the application knows, in one plain struct the interface reads
// and the tests can build without a window.
#pragma once

#include "app/Settings.h"
#include "app/Tab.h"
#include "app/ProgramScan.h"
#include "app/Undo.h"
#include "platform/FileSystem.h"

#include <chrono>
#include <string>
#include <vector>

namespace app
{

// Cut/copy go to the Windows clipboard so they interoperate with Explorer.
// This is a mirror of what THIS app last placed there, kept so cut items
// can be drawn ghosted; it is dropped as soon as anything else writes the
// clipboard.
struct Clipboard
{
    std::vector<std::string> paths;
    bool cut = false;
};

// A file shown inside the app rather than handed to another program: an
// image in the built-in viewer. Its own dockable window, like a folder view,
// but never the "active tab" that menus and the navigation pane act on.
struct FileView
{
    int         id = 0;          // from the same counter as tabs, so window ids never collide
    std::string path;
};

struct AppState
{
    Settings    settings;
    std::string exeDir;          // resolved once at startup
    std::string settingsFile;
    std::string layoutFile;      // imgui.ini beside the exe
    std::string layoutsDir;      // where File > Save layout suggests saving
    std::string currentLayout;   // the layout file last opened or saved; File > Save layout writes it again

    std::vector<Tab> tabs;
    int activeTab = 0;
    int nextTabId = 1;

    std::vector<FileView> fileViews;
    std::vector<ProgramsView> programViews;   // the Programs windows, each walking its own folder

    // Pinned folders that were not there at startup (a drive not mounted):
    // kept in the settings rather than forgotten, and tried again next time.
    std::vector<PinnedView> unavailablePins;

    std::string buildInfo;       // "SDL 3.4.8, MSVC 1950", filled in by the host for the About box

    std::vector<platform::KnownFolder> quickAccess;
    std::vector<platform::DriveInfo>   drives;
    // A mount is noticed the moment the kernel has it, which can be before
    // the desktop's volume monitor has caught up; the list is read again a
    // little later to be sure.
    std::chrono::steady_clock::time_point drivesRecheckAt{};

    Clipboard     clipboard;
    unsigned long clipboardOwnedSeq = 0;   // clipboard sequence when `clipboard` was written
    bool          clipboardHasFiles = false;

    std::vector<UndoEntry> undoHistory;   // newest last, at most kUndoDepth

    std::string statusMessage;   // transient note for the status bar
    bool quitRequested = false;

    Tab&       active();
    const Tab& active() const;
};

// Loads settings, discovers drives and known folders, reopens the pinned
// views, then opens the start folder unless a pinned view already shows it.
void InitAppState(AppState& state, const std::string& exeDir, const std::string& configDir = {});

// A path handed over on the command line (by the shell, a shortcut, or a
// terminal) replaces the unpinned tabs with one tab: a folder opens directly, a file opens
// its folder with the file selected, and anything else (a shell CLSID
// string, garbage) lands on This PC. Returns false for that last case.
bool ApplyStartArgument(AppState& state, const std::string& argument);

// Writes settings, the pinned views among them; the last path of the active
// tab is remembered when the user asked for that.
void SaveAppState(AppState& state);

// Pins a view so it reopens at the next launch, or takes the pin off.
// Written to the settings at once, as the shortcut bar is.
void SetPinned(AppState& state, int index, bool pinned);

// Returns the index of the new tab, made active.
int  OpenTab(AppState& state, const std::string& path);
void CloseTab(AppState& state, int index);

// Once per frame: reloads any tab that asked for it, syncs clipboard facts,
// and picks up drives mounted or unmounted since the last frame.
void TickAppState(AppState& state);

// Reads the drive list again; views on This PC re-read with it.
void RefreshDrives(AppState& state);

// After a file operation whose reach is unknown: every tab re-reads.
void RefreshAll(AppState& state);

// Display name for a location: drive label for roots, folder name otherwise.
std::string LocationTitle(const AppState& state, const std::string& path);

} // namespace app
