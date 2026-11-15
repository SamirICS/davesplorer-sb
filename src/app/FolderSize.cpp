#include "app/FolderSize.h"
#include "app/PathUtil.h"
#include "platform/FileSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace app::foldersize
{

namespace
{

#ifdef _WIN32
constexpr char kSep = '\\';
#else
constexpr char kSep = '/';
#endif

// A folder's total is kept when someone is likely to look at it again:
// near the top of the walk, where the user goes next, or big enough that
// walking it again would cost something. The rest are a few files each and
// cheaper to re-read than to remember.
constexpr int      kKeepDepth = 2;
constexpr uint64_t kKeepEntries = 256;
constexpr size_t   kMaxCached = 200000;

// How often a walk shows its running total.
constexpr auto kPublishEvery = std::chrono::milliseconds(100);

struct Job
{
    std::string       path;
    int64_t           modified = 0;
    bool              asked = false;
    std::set<int>     tabs;
    std::atomic<bool> cancelled{ false };
};
using JobPtr = std::shared_ptr<Job>;

struct Cached
{
    Totals   totals;
    int64_t  modified = 0;
    bool     skipped = false;
    uint64_t used = 0;
};

// Interface thread only.
bool        g_started = false;
std::thread g_worker;

// Shared with the worker, under g_mutex.
std::mutex                              g_mutex;
std::condition_variable                 g_wake;
std::deque<JobPtr>                      g_queue;
std::unordered_map<std::string, JobPtr> g_pending;   // queued or running, by path
JobPtr                                  g_current;
Totals                                  g_currentTotals;
std::map<std::string, Cached>           g_cache;     // ordered, so a subtree is one range
uint64_t                                g_clock = 0;
bool                                    g_stop = false;

std::atomic<uint64_t> g_version{ 1 };
std::atomic<uint64_t> g_foldersRead{ 0 };

std::string Join(const std::string& dir, const std::string& name)
{
    if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) return dir + name;
    return dir + kSep + name;
}

bool IsUnder(const std::string& path, const std::string& root)
{
    return path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
           (path[root.size()] == kSep || root.back() == kSep);
}

void Add(Totals& into, const Totals& child)
{
    into.bytes += child.bytes;
    into.files += child.files;
    into.folders += 1 + child.folders;
    into.partial = into.partial || child.partial;
}

// Halves the cache, keeping what was looked at most recently. Under g_mutex.
void Evict()
{
    std::vector<uint64_t> used;
    used.reserve(g_cache.size());
    for (const auto& [path, c] : g_cache) used.push_back(c.used);
    std::nth_element(used.begin(), used.begin() + (long)(used.size() / 2), used.end());
    const uint64_t cutoff = used[used.size() / 2];
    for (auto it = g_cache.begin(); it != g_cache.end();)
        it = it->second.used < cutoff ? g_cache.erase(it) : std::next(it);
}

// A finished folder, unless the walk it came from was called off: a total
// recorded after Invalidate could describe the folder before the change.
void Store(const Job& job, const std::string& path, int64_t modified, const Totals& totals, bool skipped)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (job.cancelled) return;
    g_cache[path] = Cached{ totals, modified, skipped, ++g_clock };
    if (g_cache.size() > kMaxCached) Evict();
    ++g_version;
}

// A total the walk can use instead of going in.
bool Known(const std::string& path, int64_t modified, Totals& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(path);
    if (it == g_cache.end() || it->second.skipped || it->second.modified != modified) return false;
    out = it->second.totals;
    return true;
}

struct Frame
{
    std::string path;
    int64_t     modified = 0;
    uint64_t    device = 0;
    Totals      totals;                       // files here plus the subfolders finished so far
    std::vector<platform::SizeChild> folders; // still to go into
    size_t      next = 0;
};

// Depth-first without recursion, finishing each folder after everything in
// it, so every folder's total is known the moment the walk leaves it.
void Walk(Job& job)
{
    if (!job.asked && platform::ClassifyMount(job.path) != platform::MountKind::Local)
        {
        Store(job, job.path, job.modified, Totals{}, true);
        return;
        }

    std::set<std::pair<uint64_t, uint64_t>> linkedFiles;   // hard links counted once
    std::set<std::pair<uint64_t, uint64_t>> walked;        // a bind mount cannot loop the walk
    std::unordered_map<uint64_t, bool> localDevice;
    std::vector<platform::SizeChild> children;

    auto open = [&](Frame& f) {
        ++g_foldersRead;
        if (!platform::ListSizes(f.path, children))
            {
            f.totals.partial = true;
            return;
            }
        for (platform::SizeChild& c : children)
            {
            if (c.isDirectory)
                {
                f.folders.push_back(std::move(c));
                continue;
                }
            if (c.isLink) continue;
            if (c.links > 1 && c.device && !linkedFiles.insert({ c.device, c.inode }).second) continue;
            f.totals.bytes += c.size;
            ++f.totals.files;
            }
    };

    auto lastPublish = std::chrono::steady_clock::now();
    std::vector<Frame> stack;
    stack.push_back(Frame{ job.path, job.modified, platform::FolderDevice(job.path), {}, {}, 0 });
    open(stack.back());

    while (!stack.empty())
        {
        if (job.cancelled) return;

        Frame& top = stack.back();
        if (top.next < top.folders.size())
            {
            const platform::SizeChild& c = top.folders[top.next++];
            const std::string path = Join(top.path, c.name);
            if (c.device && top.device && c.device != top.device)
                {
                // Another drive mounted here: a USB disk is walked, a
                // network share or /proc is not.
                auto it = localDevice.find(c.device);
                if (it == localDevice.end())
                    it = localDevice.emplace(c.device, platform::ClassifyMount(path) == platform::MountKind::Local).first;
                if (!it->second)
                    {
                    top.totals.partial = true;
                    continue;
                    }
                }
            if (c.device && !walked.insert({ c.device, c.inode }).second) continue;
            Totals known;
            if (Known(path, c.modified, known))
                {
                Add(top.totals, known);
                continue;
                }
            if ((int)stack.size() >= kMaxDepth)
                {
                ++top.totals.folders;
                top.totals.partial = true;
                continue;
                }
            Frame child{ path, c.modified, c.device ? c.device : top.device, {}, {}, 0 };
            open(child);
            stack.push_back(std::move(child));   // `top` is not used past here
            }
        else
            {
            Frame done = std::move(stack.back());
            stack.pop_back();
            const int depth = (int)stack.size();   // 0 for the folder asked about
            if (depth <= kKeepDepth || done.totals.files + done.totals.folders >= kKeepEntries)
                Store(job, done.path, done.modified, done.totals, false);
            if (!stack.empty()) Add(stack.back().totals, done.totals);
            }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastPublish >= kPublishEvery && !stack.empty())
            {
            lastPublish = now;
            // Everything finished so far has been added into its parent, so
            // the frames still open hold the whole running total.
            Totals sofar;
            for (const Frame& f : stack)
                {
                sofar.bytes += f.totals.bytes;
                sofar.files += f.totals.files;
                sofar.folders += f.totals.folders;
                }
            std::lock_guard<std::mutex> lock(g_mutex);
            g_currentTotals = sofar;
            ++g_version;
            }
        }
}

void WorkerMain()
{
    platform::LowerCurrentThreadPriority();
    for (;;)
        {
        JobPtr job;
        {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_wake.wait(lock, [] { return g_stop || !g_queue.empty(); });
        if (g_stop) return;
        job = std::move(g_queue.front());
        g_queue.pop_front();
        // Measured meanwhile by a walk of a folder above it.
        auto it = g_cache.find(job->path);
        if (it != g_cache.end() && it->second.modified == job->modified)
            {
            g_pending.erase(job->path);
            ++g_version;
            continue;
            }
        g_current = job;
        g_currentTotals = Totals{};
        ++g_version;
        }

        Walk(*job);

        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_pending.find(job->path);
        if (it != g_pending.end() && it->second == job) g_pending.erase(it);
        g_current.reset();
        ++g_version;
        }
}

// Takes a job out of the queue, or calls it off if it is running. Under
// g_mutex.
void Drop(const JobPtr& job)
{
    if (job == g_current) job->cancelled = true;
    else g_queue.erase(std::remove(g_queue.begin(), g_queue.end(), job), g_queue.end());
    g_pending.erase(job->path);
}

} // namespace

void Request(int tabId, const std::string& path, int64_t modified, bool asked)
{
    if (path.empty()) return;
    {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto known = g_cache.find(path);
    // A network folder skipped by the toggle is measured when asked for.
    if (known != g_cache.end() && known->second.modified == modified && !(asked && known->second.skipped)) return;
    auto it = g_pending.find(path);
    if (it != g_pending.end())
        {
        JobPtr job = it->second;
        job->tabs.insert(tabId);
        if (asked && !job->asked)
            {
            job->asked = true;
            if (job != g_current)
                {
                g_queue.erase(std::remove(g_queue.begin(), g_queue.end(), job), g_queue.end());
                g_queue.push_front(job);
                }
            }
        return;
        }
    auto job = std::make_shared<Job>();
    job->path = path;
    job->modified = modified;
    job->asked = asked;
    job->tabs.insert(tabId);
    // What the user picked goes first; what the toggle asks for is served
    // in the order the rows are shown, top first.
    if (asked) g_queue.push_front(job);
    else g_queue.push_back(job);
    g_pending[path] = job;
    ++g_version;
    }
    if (!g_started)
        {
        g_started = true;
        g_stop = false;
        g_worker = std::thread(WorkerMain);
        }
    g_wake.notify_one();
}

State Query(const std::string& path, int64_t modified, Totals& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_cache.find(path);
    if (it != g_cache.end() && it->second.modified == modified)
        {
        it->second.used = ++g_clock;
        if (it->second.skipped) return State::Skipped;
        out = it->second.totals;
        return State::Done;
        }
    auto job = g_pending.find(path);
    if (job == g_pending.end()) return State::None;
    if (job->second != g_current) return State::Queued;
    out = g_currentTotals;
    return State::Running;
}

uint64_t Version()
{
    return g_version.load();
}

void Invalidate(const std::string& path)
{
    if (path.empty()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_cache.erase(path);
    for (auto it = g_cache.lower_bound(path); it != g_cache.end();)
        {
        if (it->first.compare(0, path.size(), path) != 0) break;
        it = IsUnder(it->first, path) ? g_cache.erase(it) : std::next(it);
        }
    for (std::string up = ParentPath(path), last; !up.empty() && up != last; last = up, up = ParentPath(up))
        g_cache.erase(up);
    // A walk over the change would record totals from before it.
    if (g_current && (g_current->path == path || IsUnder(path, g_current->path) || IsUnder(g_current->path, path)))
        Drop(g_current);
    ++g_version;
}

bool Busy(int tabId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& [path, job] : g_pending)
        if (job->tabs.count(tabId)) return true;
    return false;
}

void Cancel(int tabId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<JobPtr> unwanted;
    for (auto& [path, job] : g_pending)
        if (job->tabs.erase(tabId) && job->tabs.empty()) unwanted.push_back(job);
    for (const JobPtr& job : unwanted) Drop(job);
    ++g_version;
}

void CancelUnasked()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<JobPtr> unwanted;
    for (auto& [path, job] : g_pending)
        if (!job->asked) unwanted.push_back(job);
    for (const JobPtr& job : unwanted) Drop(job);
    ++g_version;
}

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
    g_pending.clear();
    g_current.reset();
    g_cache.clear();
    ++g_version;
}

uint64_t FoldersRead()
{
    return g_foldersRead.load();
}

} // namespace app::foldersize
