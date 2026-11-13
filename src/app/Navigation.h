// Moving a tab between locations, with back/forward history.
#pragma once

#include "app/Tab.h"

#include <string>

namespace app
{

// Records the new location in history and marks the tab for reload. The
// path is normalized first. Returns false (with the reason) when the path
// is not a folder that exists; the tab is left where it was.
//
// A locked tab only refreshes: anywhere else is refused, and Back, Forward
// and Up do nothing. The interface sends those to a new view instead.
bool NavigateTo(Tab& tab, const std::string& path, std::string& error);

// Whether the tab is locked and the path is somewhere other than its own.
bool LockedAgainst(const Tab& tab, const std::string& path);

bool CanGoBack(const Tab& tab);
bool CanGoForward(const Tab& tab);
bool CanGoUp(const Tab& tab);

void GoBack(Tab& tab);
void GoForward(Tab& tab);
void GoUp(Tab& tab);
void Refresh(Tab& tab);

} // namespace app
