// Filling a tab from the OS and deriving the visible, sorted view.
#pragma once

#include "app/Settings.h"
#include "app/Tab.h"

#include <cstddef>

namespace app
{

struct AppState;

// How far down and how wide an expanded view will go before it stops. A
// home directory is unbounded in principle; these keep it to something the
// interface can hold and the user can scroll.
inline constexpr int    kMaxTreeDepth = 16;
inline constexpr size_t kMaxTreeEntries = 50000;

// Re-reads the folder (or the drive list for This PC) if the tab asked for
// it. Selection survives a reload when the same paths are still present.
void ReloadIfNeeded(Tab& tab, const Settings& settings);

// Rebuilds tab.visible from the search filter, hidden-file setting and sort
// order. Cheap enough to call whenever any of those change.
void ApplyView(Tab& tab, const Settings& settings);

// Folders first, then Explorer's natural order on the chosen column.
void SortEntries(Tab& tab);

// Once per frame: folds finished folder reads into their tabs and asks for
// the next level of any tree still filling in.
void PumpTreeScan(AppState& state);

// One folder's children, read in the background and inserted under it.
// Expanding something already expanded is free: the children are kept.
void ExpandEntry(Tab& tab, const Settings& settings, int entryIndex);
void CollapseEntry(Tab& tab, const Settings& settings, int entryIndex);

// The toolbar toggle: walk everything below this folder, or put the view
// back to a plain listing.
void SetExpandAll(Tab& tab, const Settings& settings, bool on);

// Back to the entries of tab.path alone.
void ClearTree(Tab& tab);

} // namespace app
