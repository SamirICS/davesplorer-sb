#include "platform/DragDrop.h"
namespace platform
{
void InitDragOut(SDL_Window*) {}
void ShutdownDragOut() {}
bool StartDragOut(const std::vector<std::string>&, std::string& error)
{
    error = "Dragging to other applications is not available on macOS yet. Use Copy and Paste instead.";
    return false;
}
bool DragOutInProgress(bool& accepted)
{
    accepted = false;
    return false;
}
bool PollDragOut(DragOutcome&) { return false; }
}
