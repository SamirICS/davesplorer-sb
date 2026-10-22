// Dragging files out to other programs.
//
// Dropping INTO this app is SDL's job: it registers the window's drop target
// and reports drops as events. Dragging OUT needs a drop source, which SDL
// does not provide, so it lives here: OLE on Windows, XDND on X11 and the
// core data-device protocol on Wayland.
#pragma once

#include <string>
#include <vector>

struct SDL_Window;

namespace platform
{

enum class DragOutcome
{
    Cancelled,
    Copied,
    Moved,
    Linked,
};

// Once the window exists, and again before it is destroyed. Wayland has to
// watch the pointer from the start: a drag must name the button press that
// began it, and only a listener that was there for the press knows it.
void InitDragOut(SDL_Window* window);
void ShutdownDragOut();

// Hands the paths to the desktop as a drag, with the left button still held
// from the in-app drag that crossed the window edge. The receiving program
// performs the file operation itself.
//
// On Windows DoDragDrop blocks until the drop. On Linux the drag runs
// alongside the main loop, which must keep pumping events: the other
// program's requests for the data arrive through them.
bool StartDragOut(const std::vector<std::string>& paths, std::string& error);

// True while a drag is in flight. `accepted` says whether the program under
// the pointer would take a drop there.
bool DragOutInProgress(bool& accepted);

// Once per frame. True exactly once for each drag that has ended, with what
// the receiving program did.
bool PollDragOut(DragOutcome& outcome);

} // namespace platform
