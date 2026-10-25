#include "app/ProgramScan.h"
#include "app/AppState.h"
#include "app/PathUtil.h"
#include "platform/FileSystem.h"
#include "platform/Strings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace app
{

namespace programscan
{

namespace
{

// One walk. The interface thread drops its reference when the view closes
// or rescans; the worker notices `cancelled` and stops at the next folder.
struct Walk
{
    std::string       root;
    std::atomic<bool> cancelled{ false };
    // Under g_mutex.
    std::vector<Program> found;     // not yet taken
    size_t               folders = 0;
    bool                 done = false;
    bool                 truncated = false;
};

// Interface thread only.
bool        g_started = false;
std::thread g_worker;

// Shared with the worker, under g_mutex. One walk runs at a time; a second
// view's walk waits its turn.
std::mutex                              g_mutex;
std::condition_variable                 g_wake;
std::deque<std::shared_ptr<Walk>>       g_queue;
std::map<int, std::shared_ptr<Walk>>    g_walks;   // by view id
std::shared_ptr<Walk>                   g_current;
bool                                    g_stop = false;

// Cargo keeps a hashed copy of each example beside it: ctx and
// ctx-afd58939f3d052b4. Only the plain name is the one to run.
bool IsCargoHashedCopy(const std::string& name)
{
    if (name.size() < 18 || name[name.size() - 17] != '-') return false;
    for (size_t i = name.size() - 16; i < name.size(); ++i)
        if (!std::isxdigit((unsigned char)name[i])) return false;
    return true;
}

void RunWalk(Walk& walk)
{
    using Clock = std::chrono::steady_clock;
    std::vector<Program> batch;
    size_t folders = 0;
    bool truncated = false;
    auto lastFlush = Clock::now();
    auto flush = [&](bool done) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Program& p : batch) walk.found.push_back(std::move(p));
        batch.clear();
        walk.folders = folders;
        walk.truncated = truncated;
        walk.done = done;
        lastFlush = Clock::now();
    };

    // Depth first, so one project's programs arrive together. Linked
    // folders wait at the bottom of the stack: by the time one is walked,
    // every folder reachable without links has been, so a link into the
    // tree is seen as a folder already walked and the real path wins.
    struct Folder { std::string path; int depth; bool inCargoProfile; };
    std::deque<Folder> stack{ { walk.root, 0, false } };
    std::set<std::string> walked;   // folder identities, so a link loop ends
    std::vector<platform::FileEntry> entries;
    while (!stack.empty() && !walk.cancelled)
        {
        const Folder folder = std::move(stack.back());
        const std::string& dir = folder.path;
        const int depth = folder.depth;
        stack.pop_back();
        const std::string identity = platform::FolderIdentity(dir);
        if (!identity.empty() && !walked.insert(identity).second) continue;
        if (++folders > kMaxProgramFolders)
            {
            truncated = true;
            break;
            }
        std::string error;
        if (!platform::ListDirectory(dir, entries, error)) continue;   // unreadable: nothing to find there
        bool cargoProfile = false;
        for (const platform::FileEntry& e : entries)
            if (!e.isDirectory && e.name == ".cargo-lock") cargoProfile = true;
        for (const platform::FileEntry& e : entries)
            {
            if (e.isHidden) continue;
            if (e.isDirectory)
                {
                // A linked folder is followed (a build folder is often a
                // link to another disk) only where its identity can be
                // told, since that is what stops a loop.
                if (SkipFolderForPrograms(e.name, cargoProfile)) continue;
                if (e.isReparsePoint && platform::FolderIdentity(e.path).empty()) continue;
                if (depth + 1 > kMaxProgramDepth) truncated = true;
                else if (e.isReparsePoint) stack.push_front({ e.path, depth + 1, cargoProfile });
                else stack.push_back({ e.path, depth + 1, cargoProfile });
                continue;
                }
            // A linked program is the same program listed twice.
            if (e.isReparsePoint) continue;
            if (!e.isExecutable || (folder.inCargoProfile && IsCargoHashedCopy(e.name))) continue;
            if (!platform::IsElfProgram(e.path)) continue;
            Program p;
            p.path = e.path;
            p.name = e.name;
            p.size = e.size;
            p.modified = e.modified;
            PlaceProgram(walk.root, p);
            batch.push_back(std::move(p));
            }
        if (Clock::now() - lastFlush > std::chrono::milliseconds(50)) flush(false);
        }
    flush(true);
}

void WorkerMain()
{
    for (;;)
        {
        std::shared_ptr<Walk> walk;
        {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_wake.wait(lock, [] { return g_stop || !g_queue.empty(); });
        if (g_stop) return;
        walk = g_queue.front();
        g_queue.pop_front();
        g_current = walk;
        }
        if (!walk->cancelled) RunWalk(*walk);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_current.reset();
        }
}

void Cancel(int viewId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_walks.find(viewId);
    if (it == g_walks.end()) return;
    it->second->cancelled = true;
    std::erase(g_queue, it->second);
    g_walks.erase(it);
}

void Start(int viewId, const std::string& root)
{
    Cancel(viewId);
    if (!g_started)
        {
        g_started = true;
        g_stop = false;
        g_worker = std::thread(WorkerMain);
        }
    auto walk = std::make_shared<Walk>();
    walk->root = root;
    {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_walks[viewId] = walk;
    g_queue.push_back(walk);
    }
    g_wake.notify_one();
}

// Appends what the view's walk found since last time. False when there is
// no walk for it.
bool Take(int viewId, ProgramsView& view)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_walks.find(viewId);
    if (it == g_walks.end()) return false;
    Walk& walk = *it->second;
    for (Program& p : walk.found) view.programs.push_back(std::move(p));
    walk.found.clear();
    view.folders = walk.folders;
    view.truncated = walk.truncated;
    view.scanning = !walk.done;
    if (walk.done) g_walks.erase(it);
    return true;
}

} // namespace

void Shutdown()
{
    if (g_started)
        {
        {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
        if (g_current) g_current->cancelled = true;
        }
        g_wake.notify_all();
        g_worker.join();
        g_started = false;
        }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.clear();
    g_walks.clear();
    g_current.reset();
}

} // namespace programscan

bool SkipFolderForPrograms(const std::string& name, bool inCargoProfile)
{
    if (inCargoProfile && (name == "deps" || name == "build" || name == "incremental")) return true;
    // Dependencies and build-system scratch: thousands of folders, and the
    // only programs in them are someone else's tests or CMake's compiler
    // checks. Hidden folders (.git, .venv, .cache) are skipped before this.
    static const char* const kSkip[] = {
        "node_modules", "CMakeFiles", "_deps", "__pycache__", "site-packages", "venv",
    };
    for (const char* s : kSkip)
        if (name == s) return true;
    return false;
}

void PlaceProgram(const std::string& root, Program& program)
{
    std::string rel = program.path;
    if (rel.size() > root.size() && rel.compare(0, root.size(), root) == 0)
        {
        rel.erase(0, root.size());
        while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
        }
    const size_t slash = rel.find_first_of("/\\");
    const size_t last = rel.find_last_of("/\\");
    if (slash == std::string::npos)
        {
        program.project.clear();
        program.relDir.clear();
        return;
        }
    program.project = rel.substr(0, slash);
    program.relDir = last > slash ? rel.substr(slash + 1, last - slash - 1) : std::string();
}

std::vector<ProgramNode> BuildProgramTree(const std::vector<Program>& programs, const std::string& filter)
{
    const std::string needle = platform::ToLowerAscii(filter);
    std::vector<ProgramNode> nodes(1);
    std::map<std::string, int> byKey{ { "", 0 } };
    for (size_t i = 0; i < programs.size(); ++i)
        {
        const Program& p = programs[i];
        if (!needle.empty() && platform::ToLowerAscii(p.path).find(needle) == std::string::npos) continue;
        std::vector<std::string> parts;
        if (!p.project.empty()) parts.push_back(p.project);
        for (size_t a = 0; !p.relDir.empty() && a <= p.relDir.size();)
            {
            const size_t b = std::min(p.relDir.find_first_of("/\\", a), p.relDir.size());
            if (b > a) parts.push_back(p.relDir.substr(a, b - a));
            a = b + 1;
            }
        int at = 0;
        std::string key;
        for (const std::string& part : parts)
            {
            key = key.empty() ? part : key + "/" + part;
            auto [it, added] = byKey.emplace(key, (int)nodes.size());
            if (added)
                {
                ProgramNode node;
                node.name = part;
                node.key = key;
                nodes.push_back(std::move(node));
                nodes[(size_t)at].folders.push_back(it->second);
                }
            at = it->second;
            }
        nodes[(size_t)at].programs.push_back((int)i);
        }

    // A child is always made after its parent, so one pass from the end
    // sees every subtree finished before the folder holding it.
    for (size_t n = nodes.size(); n-- > 0;)
        {
        ProgramNode& node = nodes[n];
        node.count = node.programs.size();
        for (int r : node.programs) node.newest = std::max(node.newest, programs[(size_t)r].modified);
        for (int c : node.folders)
            {
            node.count += nodes[(size_t)c].count;
            node.newest = std::max(node.newest, nodes[(size_t)c].newest);
            }
        std::sort(node.programs.begin(), node.programs.end(), [&](int a, int b) {
            const Program& x = programs[(size_t)a];
            const Program& y = programs[(size_t)b];
            if (x.modified != y.modified) return x.modified > y.modified;
            return x.path < y.path;
        });
        std::sort(node.folders.begin(), node.folders.end(), [&](int a, int b) {
            const ProgramNode& x = nodes[(size_t)a];
            const ProgramNode& y = nodes[(size_t)b];
            if (x.newest != y.newest) return x.newest > y.newest;
            return x.name < y.name;
        });
        }

    // Compact everything below the projects: a folder holding no program
    // and one folder takes that folder's place, its name joined on.
    std::vector<int> pending;
    for (int project : nodes[0].folders) pending.insert(pending.end(), nodes[(size_t)project].folders.begin(),
                                                        nodes[(size_t)project].folders.end());
    while (!pending.empty())
        {
        const int n = pending.back();
        pending.pop_back();
        while (nodes[(size_t)n].programs.empty() && nodes[(size_t)n].folders.size() == 1)
            {
            const ProgramNode child = nodes[(size_t)nodes[(size_t)n].folders[0]];
            ProgramNode& node = nodes[(size_t)n];
            node.name += "/" + child.name;
            node.key = child.key;
            node.folders = child.folders;
            node.programs = child.programs;
            }
        pending.insert(pending.end(), nodes[(size_t)n].folders.begin(), nodes[(size_t)n].folders.end());
        }
    return nodes;
}

int OpenProgramsView(AppState& state, const std::string& root)
{
    for (size_t i = 0; i < state.programViews.size(); ++i)
        if (state.programViews[i].root == root) return (int)i;
    ProgramsView view;
    view.id = state.nextTabId++;
    view.root = root;
    state.programViews.push_back(std::move(view));
    RescanPrograms(state.programViews.back());
    return (int)state.programViews.size() - 1;
}

void CloseProgramsView(AppState& state, int index)
{
    if (index < 0 || index >= (int)state.programViews.size()) return;
    programscan::Cancel(state.programViews[(size_t)index].id);
    state.programViews.erase(state.programViews.begin() + index);
}

void RescanPrograms(ProgramsView& view)
{
    view.programs.clear();
    view.folders = 0;
    view.truncated = false;
    view.scanning = true;
    programscan::Start(view.id, view.root);
}

void PumpProgramScans(AppState& state)
{
    for (ProgramsView& view : state.programViews)
        if (view.scanning && !programscan::Take(view.id, view))
            view.scanning = false;   // its walk is gone (Shutdown): nothing more is coming
}

} // namespace app
