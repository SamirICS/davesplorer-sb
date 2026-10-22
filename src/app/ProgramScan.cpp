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

    // Depth first, so one project's programs arrive together.
    struct Folder { std::string path; int depth; bool inCargoProfile; };
    std::vector<Folder> stack{ { walk.root, 0, false } };
    std::vector<platform::FileEntry> entries;
    while (!stack.empty() && !walk.cancelled)
        {
        const Folder folder = std::move(stack.back());
        const std::string& dir = folder.path;
        const int depth = folder.depth;
        stack.pop_back();
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
            // Links are skipped both ways: a linked folder can loop, and a
            // linked program is the same program listed twice.
            if (e.isReparsePoint || e.isHidden) continue;
            if (e.isDirectory)
                {
                if (SkipFolderForPrograms(e.name, cargoProfile)) continue;
                if (depth + 1 > kMaxProgramDepth) truncated = true;
                else stack.push_back({ e.path, depth + 1, cargoProfile });
                continue;
                }
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

std::vector<ProgramGroup> GroupPrograms(const std::vector<Program>& programs, const std::string& filter)
{
    const std::string needle = platform::ToLowerAscii(filter);
    std::map<std::string, ProgramGroup> byProject;
    for (size_t i = 0; i < programs.size(); ++i)
        {
        const Program& p = programs[i];
        if (!needle.empty() && platform::ToLowerAscii(p.path).find(needle) == std::string::npos) continue;
        ProgramGroup& g = byProject[p.project];
        g.project = p.project;
        g.rows.push_back((int)i);
        g.newest = std::max(g.newest, p.modified);
        }
    std::vector<ProgramGroup> groups;
    for (auto& [name, g] : byProject)
        {
        std::sort(g.rows.begin(), g.rows.end(), [&](int a, int b) {
            const Program& x = programs[(size_t)a];
            const Program& y = programs[(size_t)b];
            if (x.modified != y.modified) return x.modified > y.modified;
            return x.path < y.path;
        });
        groups.push_back(std::move(g));
        }
    std::stable_sort(groups.begin(), groups.end(),
                     [](const ProgramGroup& a, const ProgramGroup& b) { return a.newest > b.newest; });
    return groups;
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
