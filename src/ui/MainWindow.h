// One frame of interface: host window with menu bar and dockspace, the
// Navigation pane, one dockable window per open location, the dialogs, and
// the global shortcuts.
#pragma once

#include "app/AppState.h"
#include "app/Layout.h"
#include "ui/UiState.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <string>
#include <vector>

namespace ui
{

// Names the tests and the docking layout refer to.
inline constexpr const char* kHostWindow = "Davesplorer";
inline constexpr const char* kNavWindow  = "Navigation";

// A view window is titled after its folder; the "###view<id>" suffix keeps
// its identity (dock position, tests) stable as the title changes.
std::string ViewWindowName(const app::AppState& state, const app::Tab& tab);
inline ImGuiID ViewWindowId(int tabId) { return ImHashStr(("###view" + std::to_string(tabId)).c_str()); }

void DrawFrame(app::AppState& state, UiState& ui);

// Opens a location in a new view, focused, docked beside the active view
// (or into dockNode when given). Returns the new tab's index.
int OpenView(app::AppState& state, UiState& ui, const std::string& path, ImGuiID dockNode = 0);

// Asks for a new view split off the given tab's dock node next frame.
void RequestSplitView(UiState& ui, int tabId, ImGuiDir dir);

// A file view window is titled after its file, with the same stable-id
// scheme as folder views.
std::string FileViewWindowName(const app::AppState& state, const app::FileView& view);
inline ImGuiID FileViewWindowId(int id) { return ImHashStr(("###file" + std::to_string(id)).c_str()); }

// Opens (or focuses) the built-in viewer on a file, docked beside the
// active folder view. Returns the index into state.fileViews.
int  OpenFileView(app::AppState& state, UiState& ui, const std::string& path);
void CloseFileView(app::AppState& state, UiState& ui, int id);

// The Programs window: every program under a folder, by project. Titled
// after the folder, with the same stable-id scheme as the other windows.
std::string ProgramsWindowName(const app::AppState& state, const app::ProgramsView& view);
inline ImGuiID ProgramsWindowId(int id) { return ImHashStr(("###programs" + std::to_string(id)).c_str()); }
// Opens (or focuses) one on root, docked beside the active folder view.
int  OpenProgramsView(app::AppState& state, UiState& ui, const std::string& root);
void CloseProgramsView(app::AppState& state, UiState& ui, int id);
// Returns true when the user closed it.
bool DrawProgramsView(app::AppState& state, UiState& ui, int index);

// Saved layouts. Capture reads the dockspace's current tree; Apply replaces
// every view with the layout's and rebuilds the tree, so it must run before
// DockSpace() in the frame. Both are what File > Save layout / Open layout
// do once the file is chosen.
app::Layout CaptureLayout(const app::AppState& state, const UiState& ui);
void ApplyLayout(app::AppState& state, UiState& ui, const app::Layout& layout, ImVec2 size);
// Opens or saves whatever the file dialog (or Recent layouts) asked for.
void HandleLayoutRequests(app::AppState& state, UiState& ui, ImVec2 size);

// Requests used by more than one pane.
void RequestDialog(UiState& ui, Dialog which, const std::string& title = {});
void ShowError(UiState& ui, const std::string& message);
// A move asks first; fromCut empties the clipboard once it is done.
void StartMove(app::AppState& state, UiState& ui, const std::vector<std::string>& sources,
               const std::string& dest, bool fromCut);
// Ctrl+V: a copy pastes at once, a cut asks before moving.
void StartPaste(app::AppState& state, UiState& ui, app::Tab& tab);
// Asks before undoing state.undoHistory[index].
void StartUndo(app::AppState& state, UiState& ui, size_t index);
// Edit > Undo and the history beneath it, for the menu bar and context menu.
void DrawUndoMenuItems(app::AppState& state, UiState& ui);
void ZoomBy(app::AppState& state, float delta);
void ZoomReset(app::AppState& state);

// Pane bodies, each drawn inside its own Begin/End by DrawFrame.
void DrawShortcutBar(app::AppState& state, UiState& ui);   // inside the host window, under the menu
void DrawNavPane(app::AppState& state, UiState& ui);
// One location's window. Returns true when the user closed it.
bool DrawView(app::AppState& state, UiState& ui, int tabIndex);
// One file's window. Returns true when the user closed it.
bool DrawFileView(app::AppState& state, UiState& ui, int index);
void DrawDialogs(app::AppState& state, UiState& ui);

} // namespace ui
