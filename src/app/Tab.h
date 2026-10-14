// One open location: its listing, what is selected, and where it has been.
#pragma once

#include "app/Settings.h"
#include "platform/FileSystem.h"

#include <cstdint>
#include <string>
#include <vector>

namespace app
{

enum class SortColumn
{
    Name = 0,
    Modified,
    Type,
    Size,
};

struct SortSpec
{
    SortColumn column = SortColumn::Name;
    bool ascending = true;
};

// Where one entry sits in the expanded tree. Parallel to Tab::entries; in
// an ordinary listing every node is depth 0 with no parent, which is what
// all the non-recursive code already assumes.
//
// Children are always appended after their parent, so a child's entry index
// is always greater than its parent's. Several passes rely on that.
struct TreeNode
{
    int  parent = -1;        // entry index of the folder it came from, -1 at the top
    int  depth = 0;
    bool loaded = false;     // its children are in `entries`, open or not
    bool expanded = false;   // and are being shown
    bool pending = false;    // children asked for, not arrived yet
};

struct Tab
{
    int id = 0;                               // stable identity for the interface; indices shift as tabs close
    std::string path;                         // "" is This PC
    bool pinned = false;                      // reopened at the next launch

    std::vector<platform::FileEntry> entries; // everything the OS reported
    std::vector<std::string> typeNames;       // parallel to entries, shell type names
    std::vector<TreeNode> nodes;              // parallel to entries, where each sits in the tree
    std::vector<int> visible;                 // indices into entries after filter and sort
    std::vector<uint8_t> selected;            // parallel to entries
    int focused = -1;                         // entry index with keyboard focus, -1 none
    int anchor = -1;                          // where a shift-range starts

    // Subfolders opened in place. The toolbar toggle walks the whole tree;
    // a row's own arrow opens one level. Both are per-location: navigating
    // away puts the view back to a plain listing.
    bool expandAll = false;
    bool scanning = false;                    // a walk is in flight
    bool scanTruncated = false;               // it stopped at the entry cap

    std::vector<std::string> history;
    int historyIndex = -1;

    std::string filter;                       // search box text
    SortSpec sort;
    ViewMode viewMode = ViewMode::Details;    // starts from the setting; each view can differ
    std::string error;                        // last listing failure, shown in place of the list
    bool needsReload = true;
    std::string selectOnLoad;                 // entry to select once the next listing arrives

    // Rises with every reload so the interface can notice a new listing
    // (scroll to top, forget stale row state) without comparing vectors.
    uint64_t listingGeneration = 0;

    // Rises whenever a walk is abandoned, which is more often than the
    // listing changes: a folder read on the worker thread carries the token
    // it was asked under, and one that no longer matches is thrown away
    // instead of merged into a tree that has moved on.
    uint64_t scanGeneration = 0;
};

} // namespace app
