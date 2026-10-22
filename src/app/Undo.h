// What was done to files, kept so it can be taken back: the last ten
// operations, oldest first. Each entry says where its items came from and
// where they are now; undoing checks the disk still agrees before touching
// anything, and never overwrites, so an item changed or replaced since is
// left where it is with a reason.
//
// A permanent delete records nothing: there is nothing to bring back.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace app
{

struct AppState;

enum class UndoKind
{
    Create,   // a new folder or file; undo sends it to the Recycle Bin while still empty
    Rename,   // undo renames it back
    Move,     // undo moves it back to the folder it came from
    Copy,     // undo sends the copy to the Recycle Bin
    Trash,    // undo restores it from the Recycle Bin
};

struct UndoItem
{
    std::string from;   // where it was; empty for Create and Copy
    std::string to;     // where it is now; empty for Trash
};

struct UndoEntry
{
    UndoKind              kind = UndoKind::Create;
    std::vector<UndoItem> items;
    std::string           label;   // "Move 3 items to Documents", for the Edit menu
};

inline constexpr size_t kUndoDepth = 10;

// Adds an entry, dropping the oldest past kUndoDepth. An entry with no
// items (the operation failed outright) is not kept.
void RecordUndo(AppState& state, UndoEntry entry);

// What undoing the entry would do, phrased as the question its
// confirmation asks: "Move 'a.txt' back to Documents?"
std::string UndoQuestion(const AppState& state, const UndoEntry& entry);

// Undoes history[index] and takes it out of the history, whether every
// item came back or not. Items that cannot come back are reported in
// `error`; the rest are still undone.
bool Undo(AppState& state, size_t index, std::string& error);

} // namespace app
