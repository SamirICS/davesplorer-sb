// Reading folders off the interface thread, for the views that expand
// subfolders in place.
//
// One job lists one directory. Recursion is driven by whoever asked: only
// the interface thread knows what entry index a folder ended up at, so it
// merges a batch and then asks for the subfolders it just learned about.
#pragma once

#include "platform/FileSystem.h"

#include <cstdint>
#include <string>
#include <vector>

namespace app::treescan
{

struct Batch
{
    int      tabId = 0;
    uint64_t generation = 0;      // tab.listingGeneration when it was asked for
    int      parentEntry = -1;    // entry index of the folder listed, -1 for the tab's own
    std::vector<platform::FileEntry> entries;
    std::string error;            // unreadable folder; the batch is empty
};

// Queues one folder. The worker only calls platform::ListDirectory: the
// shell type-name cache belongs to the interface thread.
void Request(int tabId, uint64_t generation, int parentEntry, const std::string& path);

// Moves the finished batches for this tab into `out`, dropping anything
// from an older listing.
void Take(int tabId, uint64_t generation, std::vector<Batch>& out);

// Work is queued or in flight for this tab.
bool Busy(int tabId);

// Forgets this tab's queued and finished work. A job already running still
// finishes; its batch is dropped on arrival.
void Cancel(int tabId);

// Stops the worker and drops everything. Before the program exits.
void Shutdown();

} // namespace app::treescan
