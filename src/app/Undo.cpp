#include "app/Undo.h"
#include "app/AppState.h"
#include "app/PathUtil.h"
#include "platform/FileSystem.h"

namespace app
{

namespace
{

std::string Quoted(const std::string& path)
{
    return "'" + PathName(path) + "'";
}

// An empty folder, or a file with nothing in it: what Create made, before
// anyone put something there worth keeping.
bool StillEmpty(const std::string& path)
{
    std::vector<platform::FileEntry> entries;
    std::string error;
    if (platform::IsDirectory(path)) return platform::ListDirectory(path, entries, error) && entries.empty();
    if (!platform::ListDirectory(ParentPath(path), entries, error)) return false;
    for (const platform::FileEntry& e : entries)
        if (e.path == path) return e.size == 0;
    return false;
}

bool UndoItemBack(UndoKind kind, const UndoItem& item, std::string& error)
{
    switch (kind)
        {
        case UndoKind::Create:
        case UndoKind::Copy:
            if (!platform::PathExists(item.to)) return true;   // already gone: nothing to take back
            if (kind == UndoKind::Create && !StillEmpty(item.to))
                {
                error = Quoted(item.to) + " has something in it now, so it was kept.";
                return false;
                }
            return platform::MoveToRecycleBin({ item.to }, error);
        case UndoKind::Rename:
        case UndoKind::Move:
            if (!platform::PathExists(item.to))
                {
                error = Quoted(item.to) + " is no longer there.";
                return false;
                }
            if (platform::PathExists(item.from))
                {
                error = "An item named " + Quoted(item.from) + " is already back in its place.";
                return false;
                }
            if (kind == UndoKind::Rename) return platform::RenamePath(item.to, item.from, error);
            if (!platform::IsDirectory(ParentPath(item.from)))
                {
                error = "The folder " + Quoted(ParentPath(item.from)) + " it came from no longer exists.";
                return false;
                }
            return platform::MovePaths({ item.to }, ParentPath(item.from), error);
        case UndoKind::Trash:
            return platform::RestoreFromRecycleBin(item.from, error);
        }
    return false;
}

} // namespace

void RecordUndo(AppState& state, UndoEntry entry)
{
    if (entry.items.empty()) return;
    state.undoHistory.push_back(std::move(entry));
    if (state.undoHistory.size() > kUndoDepth)
        state.undoHistory.erase(state.undoHistory.begin(), state.undoHistory.end() - (ptrdiff_t)kUndoDepth);
}

std::string UndoQuestion(const AppState& state, const UndoEntry& entry)
{
    const size_t n = entry.items.size();
    const UndoItem& first = entry.items.front();
    const std::string these = "these " + std::to_string(n) + " items";
    switch (entry.kind)
        {
        case UndoKind::Create:
            return "Move the new " + std::string(platform::IsDirectory(first.to) ? "folder " : "file ") +
                   Quoted(first.to) + " to the Recycle Bin?";
        case UndoKind::Rename:
            return "Rename " + Quoted(first.to) + " back to " + Quoted(first.from) + "?";
        case UndoKind::Move:
            return "Move " + (n == 1 ? Quoted(first.to) : these) + " back to " +
                   LocationTitle(state, ParentPath(first.from)) + "?";
        case UndoKind::Copy:
            return "Move " + (n == 1 ? "the copy " + Quoted(first.to) : "the " + std::to_string(n) + " copies") +
                   " in " + LocationTitle(state, ParentPath(first.to)) + " to the Recycle Bin?";
        case UndoKind::Trash:
            return "Restore " + (n == 1 ? Quoted(first.from) : these) + " from the Recycle Bin?";
        }
    return entry.label + "?";
}

bool Undo(AppState& state, size_t index, std::string& error)
{
    if (index >= state.undoHistory.size()) return false;
    const UndoEntry entry = state.undoHistory[index];
    state.undoHistory.erase(state.undoHistory.begin() + (ptrdiff_t)index);

    std::vector<std::string> failures;
    for (const UndoItem& item : entry.items)
        {
        std::string why;
        if (!UndoItemBack(entry.kind, item, why)) failures.push_back(why);
        }
    RefreshAll(state);
    if (failures.empty())
        {
        state.statusMessage = "Undone: " + entry.label;
        return true;
        }
    // A long list helps nobody; the first few say what went wrong.
    error = failures.size() == entry.items.size() ? "Nothing could be undone." : "Some items could not be undone.";
    for (size_t i = 0; i < failures.size() && i < 3; ++i) error += "\n" + failures[i];
    if (failures.size() > 3) error += "\n...and " + std::to_string(failures.size() - 3) + " more.";
    return false;
}

} // namespace app
