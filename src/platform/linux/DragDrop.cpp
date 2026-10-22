#include "platform/DragDrop.h"
#include "platform/linux/DragOut.h"

#include <SDL3/SDL.h>
#include <glib.h>

#include <cstring>

namespace platform
{
namespace
{
enum class Backend { None, X11, Wayland };
Backend backend = Backend::None;

// Finished without ever reaching a backend: nothing to drag, or the button
// was already up.
bool        haveOutcome = false;
DragOutcome pendingOutcome = DragOutcome::Cancelled;
}

void InitDragOut(SDL_Window* window)
{
    backend = Backend::None;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (!window || !driver) return;
    if (std::strcmp(driver, "x11") == 0 && x11dnd::Init(window)) backend = Backend::X11;
    else if (std::strcmp(driver, "wayland") == 0 && wldnd::Init(window)) backend = Backend::Wayland;
}

void ShutdownDragOut()
{
    if (backend == Backend::X11) x11dnd::Shutdown();
    else if (backend == Backend::Wayland) wldnd::Shutdown();
    backend = Backend::None;
}

bool StartDragOut(const std::vector<std::string>& paths, std::string& error)
{
    std::string uris;
    for (const std::string& path : paths)
        {
        char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
        if (!uri) continue;
        uris += uri;
        uris += "\r\n";
        g_free(uri);
        }
    if (uris.empty())
        {
        haveOutcome = true;
        pendingOutcome = DragOutcome::Cancelled;
        return true;
        }
    switch (backend)
        {
        case Backend::X11:     return x11dnd::Start(uris, error);
        case Backend::Wayland: return wldnd::Start(uris, error);
        case Backend::None:    break;
        }
    error = "Dragging to other applications needs an X11 or Wayland desktop. Use Copy and Paste instead.";
    return false;
}

bool DragOutInProgress(bool& accepted)
{
    accepted = false;
    if (backend == Backend::X11) return x11dnd::InProgress(accepted);
    if (backend == Backend::Wayland) return wldnd::InProgress(accepted);
    return false;
}

bool PollDragOut(DragOutcome& outcome)
{
    if (haveOutcome)
        {
        haveOutcome = false;
        outcome = pendingOutcome;
        return true;
        }
    if (backend == Backend::X11) return x11dnd::Poll(outcome);
    if (backend == Backend::Wayland) return wldnd::Poll(outcome);
    return false;
}
}
