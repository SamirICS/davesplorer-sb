// What a folder holds, all the way down, measured off the interface thread.
//
// Walking a tree costs a stat per file, so nothing here runs unless asked:
// the View menu's "Folder sizes" asks for every folder a view is showing,
// "Calculate size" for the selected ones. One worker serves every view, at
// the lowest priority the OS offers, and keeps what it learns: a walk of a
// folder records the totals of the folders under it too, so going down into
// one shows its sizes without touching the disk again.
//
// There is no watcher. A total stays until something says it is stale: a
// file operation through Invalidate, F5 through Invalidate on the view's
// folder, or the folder's own modified time no longer matching.
#pragma once

#include <cstdint>
#include <string>

namespace app::foldersize
{

struct Totals
{
    uint64_t bytes = 0;      // the files' own sizes, as the Size column shows a file
    uint64_t files = 0;
    uint64_t folders = 0;    // below this one, not counting itself
    bool     partial = false;   // something inside could not be read or is on another kind of drive
};

enum class State : uint8_t
{
    None,      // never measured, or no longer known to be right
    Queued,
    Running,   // Query gives the total so far
    Done,
    Skipped,   // on a network drive; measured only when asked for by name
};

// Set on a row's state in Tab::sizeState when its Done total left
// something out.
inline constexpr uint8_t kPartialBit = 0x80;

// How deep a walk goes before it counts the rest as unread.
inline constexpr int kMaxDepth = 128;

// Walks one folder unless it is already known, queued or being walked.
// `modified` is the folder's own modified time from the listing; a total
// recorded under a different one is not trusted. `asked` is the user
// choosing this folder: it goes ahead of the queue, and it is measured even
// on a network drive.
void Request(int tabId, const std::string& path, int64_t modified, bool asked);

// What is known about one folder. `out` is filled for Running and Done.
State Query(const std::string& path, int64_t modified, Totals& out);

// Rises whenever anything Query could answer changes, so a view can skip
// asking again on frames where nothing moved.
uint64_t Version();

// Something at `path` changed: forget it, everything under it and every
// folder above it, whose totals included it. A walk that covers it starts
// over the next time it is asked for.
void Invalidate(const std::string& path);

// Work is queued or in flight for this view.
bool Busy(int tabId);

// This view no longer wants its folders measured. A walk another view
// still wants carries on.
void Cancel(int tabId);

// The View menu toggle went off: drop every walk nobody asked for by name.
void CancelUnasked();

// Stops the worker and forgets everything. Before the program exits, and
// between tests.
void Shutdown();

// Folders read from disk since start, for tests that check a total came
// from memory.
uint64_t FoldersRead();

} // namespace app::foldersize
