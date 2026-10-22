// The two Linux drag sources behind platform/DragDrop.h. DragDrop.cpp picks
// one from SDL's video driver; each takes the payload ready made as a
// text/uri-list, the format every Linux file manager, browser and editor
// reads for a file drop.
#pragma once

#include "platform/DragDrop.h"

#include <string>

namespace platform
{

constexpr const char* kUriListMime = "text/uri-list";

// How long a drop may wait for the receiving program to say it is done
// before the drag is given up on; the data stays on offer until then.
constexpr unsigned long long kDropFinishTimeoutMs = 15000;

namespace x11dnd
{
bool Init(SDL_Window* window);
void Shutdown();
bool Start(const std::string& uris, std::string& error);
bool InProgress(bool& accepted);
bool Poll(DragOutcome& outcome);
} // namespace x11dnd

namespace wldnd
{
bool Init(SDL_Window* window);
void Shutdown();
bool Start(const std::string& uris, std::string& error);
bool InProgress(bool& accepted);
bool Poll(DragOutcome& outcome);
} // namespace wldnd

} // namespace platform
