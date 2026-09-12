#include "app/Listing.h"
#include "app/AppState.h"
#include "app/TreeScan.h"
#include "platform/Strings.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace app
{

namespace
{

// Shell type-name lookups go through the registry; one per extension per
// process is plenty.
std::unordered_map<std::string, std::string>& TypeNameCache()
{
    static std::unordered_map<std::string, std::string> cache;
    return cache;
}

std::string TypeNameFor(const platform::FileEntry& e)
{
    if (e.isDirectory) return "File folder";
    auto& cache = TypeNameCache();
    auto it = cache.find(e.extension);
    if (it != cache.end()) return it->second;
    std::string name = platform::FileTypeName(e.extension, false);
    cache.emplace(e.extension, name);
    return name;
}

void LoadThisPC(Tab& tab)
{
    for (const platform::DriveInfo& d : platform::GetDrives())
        {
        platform::FileEntry e;
        e.name        = d.displayName;
        e.nameWide    = platform::Utf8ToWide(d.root);   // sort drives by letter, not label
        e.path        = d.root;
        e.isDirectory = true;
        e.size        = d.totalBytes - d.freeBytes;
        tab.entries.push_back(std::move(e));
        tab.typeNames.push_back(d.typeName);
        }
}

bool PassesHidden(const Settings& settings, const platform::FileEntry& e)
{
    return settings.showHidden || !(e.isHidden || e.isSystem);
}

bool PassesFilter(const Tab& tab, const platform::FileEntry& e)
{
    return tab.filter.empty() || platform::ContainsNoCase(e.name, tab.filter);
}

// Folders stay above files whichever way the column is sorted; that is how
// Explorer behaves and what users expect.
bool EntryLess(const Tab& tab, int ia, int ib)
{
    const platform::FileEntry& a = tab.entries[(size_t)ia];
    const platform::FileEntry& b = tab.entries[(size_t)ib];
    if (a.isDirectory != b.isDirectory) return a.isDirectory;
    int cmp = 0;
    switch (tab.sort.column)
        {
        case SortColumn::Modified:
            cmp = (a.modified < b.modified) ? -1 : (a.modified > b.modified) ? 1 : 0;
            break;
        case SortColumn::Type:
            cmp = tab.typeNames[(size_t)ia].compare(tab.typeNames[(size_t)ib]);
            break;
        case SortColumn::Size:
            cmp = (a.size < b.size) ? -1 : (a.size > b.size) ? 1 : 0;
            break;
        case SortColumn::Name:
            break;
        }
    if (cmp == 0) cmp = platform::NaturalCompare(a.nameWide, b.nameWide);
    return tab.sort.ascending ? cmp < 0 : cmp > 0;
}

// Any entry that came out of a subfolder: the view is a tree rather than
// one directory's listing.
bool IsTree(const Tab& tab)
{
    for (const TreeNode& n : tab.nodes)
        if (n.parent >= 0) return true;
    return false;
}

// Stops the walk and forgets what was asked for. The token moves on, so a
// folder the worker is still reading is dropped on arrival rather than
// merged, and nothing is left waiting for children that will never come.
void CancelWalk(Tab& tab)
{
    treescan::Cancel(tab.id);
    ++tab.scanGeneration;
    for (TreeNode& n : tab.nodes) n.pending = false;
    tab.scanning = false;
}

// Queues the folders in [from, to) for reading. A link is left alone: it
// can point back up the tree, and a walk that followed one would never
// finish.
void RequestSubfolders(Tab& tab, const Settings& settings, size_t from, size_t to)
{
    for (size_t i = from; i < to && i < tab.entries.size(); ++i)
        {
        const platform::FileEntry& e = tab.entries[i];
        if (!e.isDirectory || e.isReparsePoint) continue;
        if (!PassesHidden(settings, e)) continue;
        if (tab.nodes[i].pending) continue;
        // Read once already: opening it again costs nothing.
        if (tab.nodes[i].loaded)
            {
            tab.nodes[i].expanded = true;
            continue;
            }
        if (tab.nodes[i].depth + 1 >= kMaxTreeDepth)
            {
            tab.scanTruncated = true;
            continue;
            }
        if (tab.entries.size() >= kMaxTreeEntries)
            {
            // Far past what anyone will scroll through. Stop the whole walk
            // rather than let a queue of folders keep it growing.
            tab.scanTruncated = true;
            CancelWalk(tab);
            return;
            }
        tab.nodes[i].pending = true;
        treescan::Request(tab.id, tab.scanGeneration, (int)i, e.path);
        }
}

// One folder's children, arrived from the worker.
bool MergeBatch(Tab& tab, const Settings& settings, treescan::Batch& batch)
{
    const int parent = batch.parentEntry;
    if (parent < 0 || parent >= (int)tab.nodes.size()) return false;
    tab.nodes[(size_t)parent].pending = false;
    // An unreadable or empty folder still counts as read, so its arrow
    // shows the folder for what it is instead of retrying every frame.
    tab.nodes[(size_t)parent].loaded = true;
    tab.nodes[(size_t)parent].expanded = true;
    const int depth = tab.nodes[(size_t)parent].depth + 1;
    if (batch.entries.empty()) return true;

    const size_t first = tab.entries.size();
    for (platform::FileEntry& e : batch.entries)
        {
        tab.entries.push_back(std::move(e));
        tab.typeNames.push_back(TypeNameFor(tab.entries.back()));
        TreeNode child;
        child.parent = parent;
        child.depth = depth;
        tab.nodes.push_back(child);
        }
    tab.selected.resize(tab.entries.size(), 0);
    if (tab.expandAll) RequestSubfolders(tab, settings, first, tab.entries.size());
    return true;
}

// Depth-first from one parent, in the order its children sorted into.
void AppendSubtree(Tab& tab, int parent, const std::vector<int>& start, const std::vector<int>& kids,
                   const std::vector<uint8_t>& wanted, bool filtering)
{
    const size_t bucket = (size_t)(parent + 1);
    for (int k = start[bucket]; k < start[bucket + 1]; ++k)
        {
        const int i = kids[(size_t)k];
        if (!wanted[(size_t)i]) continue;
        tab.visible.push_back(i);
        if (!tab.entries[(size_t)i].isDirectory) continue;
        // A search reaches into closed folders, since a match the user
        // cannot see is no answer at all.
        if (tab.nodes[(size_t)i].expanded || filtering)
            AppendSubtree(tab, i, start, kids, wanted, filtering);
        }
}

void ApplyTreeView(Tab& tab, const Settings& settings)
{
    const size_t n = tab.entries.size();
    const bool filtering = !tab.filter.empty();

    // A row the hidden-file rule excludes takes its subtree with it. One
    // that only fails the search stays when something inside it matches --
    // children always sit at a higher index than their parent, so a single
    // reverse pass carries that up the tree.
    std::vector<uint8_t> wanted(n, 0);
    for (size_t i = n; i-- > 0;)
        {
        const platform::FileEntry& e = tab.entries[i];
        if (!PassesHidden(settings, e)) continue;
        if (!wanted[i] && !PassesFilter(tab, e)) continue;
        wanted[i] = 1;
        const int parent = tab.nodes[i].parent;
        if (parent >= 0) wanted[(size_t)parent] = 1;
        }
    // The pass above marks a parent wanted from below, which can reach a
    // folder the hidden rule excluded; take those back out, along with
    // everything under them.
    for (size_t i = 0; i < n; ++i)
        {
        if (!wanted[i]) continue;
        const int parent = tab.nodes[i].parent;
        if (!PassesHidden(settings, tab.entries[i]) || (parent >= 0 && !wanted[(size_t)parent]))
            wanted[i] = 0;
        }

    // The children of each parent as one flat array: slot 0 holds the top
    // level, slot i+1 the children of entry i.
    std::vector<int> start(n + 2, 0);
    for (size_t i = 0; i < n; ++i) ++start[(size_t)(tab.nodes[i].parent + 1) + 1];
    for (size_t k = 1; k < start.size(); ++k) start[k] += start[k - 1];
    std::vector<int> cursor(start.begin(), start.end() - 1);
    std::vector<int> kids(n);
    for (size_t i = 0; i < n; ++i)
        kids[(size_t)cursor[(size_t)(tab.nodes[i].parent + 1)]++] = (int)i;

    // Siblings sort among themselves, so a subfolder's files never
    // interleave with its parent's.
    for (size_t b = 0; b + 1 < start.size(); ++b)
        {
        if (start[b + 1] - start[b] < 2) continue;
        std::stable_sort(kids.begin() + start[b], kids.begin() + start[b + 1],
                         [&](int ia, int ib) { return EntryLess(tab, ia, ib); });
        }

    AppendSubtree(tab, -1, start, kids, wanted, filtering);
}

} // namespace

void ReloadIfNeeded(Tab& tab, const Settings& settings)
{
    if (!tab.needsReload) return;
    tab.needsReload = false;

    // Paths, not names: the same name turns up in every other subfolder
    // once a tree is expanded.
    std::unordered_set<std::string> keep;
    for (size_t i = 0; i < tab.entries.size(); ++i)
        if (i < tab.selected.size() && tab.selected[i]) keep.insert(tab.entries[i].path);
    const std::string focusedPath = (tab.focused >= 0 && tab.focused < (int)tab.entries.size())
                                        ? tab.entries[(size_t)tab.focused].path : std::string();

    // Whatever the walker is still reading describes a listing that is
    // about to be replaced.
    CancelWalk(tab);
    tab.entries.clear();
    tab.typeNames.clear();
    tab.nodes.clear();
    tab.error.clear();
    tab.scanTruncated = false;
    if (tab.path.empty())
        {
        LoadThisPC(tab);
        }
    else if (platform::ListDirectory(tab.path, tab.entries, tab.error))
        {
        tab.typeNames.reserve(tab.entries.size());
        for (const platform::FileEntry& e : tab.entries)
            tab.typeNames.push_back(TypeNameFor(e));
        }

    tab.nodes.assign(tab.entries.size(), TreeNode{});
    tab.selected.assign(tab.entries.size(), 0);
    tab.focused = -1;
    tab.anchor = -1;
    for (size_t i = 0; i < tab.entries.size(); ++i)
        {
        if (keep.count(tab.entries[i].path)) tab.selected[i] = 1;
        if (!focusedPath.empty() && tab.entries[i].path == focusedPath) tab.focused = (int)i;
        // A file named on the command line: land on it, as "Show in
        // Explorer" would.
        if (!tab.selectOnLoad.empty() && tab.entries[i].name == tab.selectOnLoad)
            {
            tab.selected[i] = 1;
            tab.focused = (int)i;
            tab.anchor = (int)i;
            }
        }
    tab.selectOnLoad.clear();
    ++tab.listingGeneration;
    // The new listing is walked again from the top; folders opened one at a
    // time are not restored.
    if (tab.expandAll && !tab.path.empty()) RequestSubfolders(tab, settings, 0, tab.entries.size());
    tab.scanning = treescan::Busy(tab.id);
    ApplyView(tab, settings);
}

void SortEntries(Tab& tab)
{
    std::stable_sort(tab.visible.begin(), tab.visible.end(),
                     [&](int ia, int ib) { return EntryLess(tab, ia, ib); });
}

void ApplyView(Tab& tab, const Settings& settings)
{
    if (tab.nodes.size() != tab.entries.size()) tab.nodes.resize(tab.entries.size());
    tab.visible.clear();
    tab.visible.reserve(tab.entries.size());
    if (IsTree(tab))
        {
        ApplyTreeView(tab, settings);
        return;
        }
    for (size_t i = 0; i < tab.entries.size(); ++i)
        {
        const platform::FileEntry& e = tab.entries[i];
        if (!PassesHidden(settings, e)) continue;
        if (!PassesFilter(tab, e)) continue;
        tab.visible.push_back((int)i);
        }
    SortEntries(tab);
}

void PumpTreeScan(AppState& state)
{
    std::vector<treescan::Batch> batches;
    for (Tab& tab : state.tabs)
        {
        batches.clear();
        treescan::Take(tab.id, tab.scanGeneration, batches);
        bool changed = false;
        for (treescan::Batch& b : batches)
            changed = MergeBatch(tab, state.settings, b) || changed;
        if (changed) ApplyView(tab, state.settings);
        tab.scanning = treescan::Busy(tab.id);
        }
}

void ExpandEntry(Tab& tab, const Settings& settings, int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= (int)tab.nodes.size()) return;
    const platform::FileEntry& e = tab.entries[(size_t)entryIndex];
    if (!e.isDirectory) return;
    TreeNode& node = tab.nodes[(size_t)entryIndex];
    if (node.expanded || node.pending) return;
    // Read once, kept: re-opening a folder is instant.
    if (node.loaded)
        {
        node.expanded = true;
        ApplyView(tab, settings);
        return;
        }
    if (node.depth + 1 >= kMaxTreeDepth || e.isReparsePoint) return;
    node.pending = true;
    treescan::Request(tab.id, tab.scanGeneration, entryIndex, e.path);
    tab.scanning = true;
}

void CollapseEntry(Tab& tab, const Settings& settings, int entryIndex)
{
    if (entryIndex < 0 || entryIndex >= (int)tab.nodes.size()) return;
    if (!tab.nodes[(size_t)entryIndex].expanded) return;
    tab.nodes[(size_t)entryIndex].expanded = false;

    // What a closed folder holds is out of reach, so it must not stay
    // selected: Delete and Copy act on the selection, not on what is on
    // screen. Children sit above their parent's index, so one forward pass
    // reaches the whole subtree.
    std::vector<uint8_t> inside(tab.nodes.size(), 0);
    for (size_t i = (size_t)entryIndex + 1; i < tab.nodes.size(); ++i)
        {
        const int parent = tab.nodes[i].parent;
        if (parent != entryIndex && !(parent > entryIndex && inside[(size_t)parent])) continue;
        inside[i] = 1;
        if (i < tab.selected.size()) tab.selected[i] = 0;
        if (tab.focused == (int)i) tab.focused = entryIndex;
        if (tab.anchor == (int)i) tab.anchor = entryIndex;
        }
    ApplyView(tab, settings);
}

void SetExpandAll(Tab& tab, const Settings& settings, bool on)
{
    if (tab.path.empty()) return;   // This PC lists drives, not a folder to walk
    tab.expandAll = on;
    if (!on)
        {
        ClearTree(tab);
        ApplyView(tab, settings);
        return;
        }
    tab.scanTruncated = false;
    RequestSubfolders(tab, settings, 0, tab.entries.size());
    tab.scanning = treescan::Busy(tab.id);
}

void ClearTree(Tab& tab)
{
    CancelWalk(tab);
    tab.scanTruncated = false;

    // The tab's own entries were read first, so they are the front of the
    // vector and everything from the first child on can simply go.
    size_t top = tab.nodes.size();
    for (size_t i = 0; i < tab.nodes.size(); ++i)
        if (tab.nodes[i].parent >= 0) { top = i; break; }

    tab.entries.resize(top);
    tab.typeNames.resize(top);
    tab.nodes.resize(top);
    tab.selected.resize(top, 0);
    for (TreeNode& n : tab.nodes) { n.loaded = false; n.expanded = false; n.pending = false; }
    if (tab.focused >= (int)top) tab.focused = -1;
    if (tab.anchor >= (int)top) tab.anchor = -1;
    tab.visible.erase(std::remove_if(tab.visible.begin(), tab.visible.end(),
                                     [top](int i) { return i >= (int)top; }),
                      tab.visible.end());
}

} // namespace app
