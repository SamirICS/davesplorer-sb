#include "app/TreeScan.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace app::treescan
{

namespace
{

struct Job
{
    int         tabId = 0;
    uint64_t    generation = 0;
    int         parentEntry = -1;
    std::string path;
};

// Interface thread only.
bool        g_started = false;
std::thread g_worker;

// Shared with the worker, under g_mutex.
std::mutex              g_mutex;
std::condition_variable g_wake;
std::deque<Job>         g_queue;
std::vector<Batch>      g_done;
bool                    g_stop = false;

// How much work each tab has outstanding, so Busy() never walks the queue.
// A job is counted from the moment it is asked for until its batch is taken
// (or dropped), which is exactly the span over which the listing is still
// growing.
std::unordered_map<int, int> g_queued;
std::unordered_map<int, int> g_running;
std::unordered_map<int, int> g_ready;

void Drop(std::unordered_map<int, int>& counter, int tabId)
{
    auto it = counter.find(tabId);
    if (it == counter.end()) return;
    if (--it->second <= 0) counter.erase(it);
}

void WorkerMain()
{
    for (;;)
        {
        Job job;
        {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_wake.wait(lock, [] { return g_stop || !g_queue.empty(); });
        if (g_stop) return;
        // Oldest first: a tree fills in from the top, which is where the
        // user is looking while it arrives.
        job = std::move(g_queue.front());
        g_queue.pop_front();
        Drop(g_queued, job.tabId);
        ++g_running[job.tabId];
        }

        Batch batch;
        batch.tabId = job.tabId;
        batch.generation = job.generation;
        batch.parentEntry = job.parentEntry;
        platform::ListDirectory(job.path, batch.entries, batch.error);

        std::lock_guard<std::mutex> lock(g_mutex);
        Drop(g_running, job.tabId);
        ++g_ready[job.tabId];
        g_done.push_back(std::move(batch));
        }
}

} // namespace

void Request(int tabId, uint64_t generation, int parentEntry, const std::string& path)
{
    if (!g_started)
        {
        g_started = true;
        g_stop = false;
        g_worker = std::thread(WorkerMain);
        }
    {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.push_back(Job{ tabId, generation, parentEntry, path });
    ++g_queued[tabId];
    }
    g_wake.notify_one();
}

void Take(int tabId, uint64_t generation, std::vector<Batch>& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_done.size();)
        {
        if (g_done[i].tabId != tabId)
            {
            ++i;
            continue;
            }
        // A batch from a listing that has since been replaced describes a
        // tree that no longer exists.
        if (g_done[i].generation == generation) out.push_back(std::move(g_done[i]));
        Drop(g_ready, tabId);
        g_done.erase(g_done.begin() + (long)i);
        }
}

bool Busy(int tabId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_queued.count(tabId) || g_running.count(tabId) || g_ready.count(tabId);
}

void Cancel(int tabId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < g_queue.size();)
        {
        if (g_queue[i].tabId == tabId) g_queue.erase(g_queue.begin() + (long)i);
        else ++i;
        }
    for (size_t i = 0; i < g_done.size();)
        {
        if (g_done[i].tabId == tabId) g_done.erase(g_done.begin() + (long)i);
        else ++i;
        }
    g_queued.erase(tabId);
    g_ready.erase(tabId);
    // A job already running is left alone; its batch lands in g_done and is
    // dropped by the next Take, whose generation will not match.
}

void Shutdown()
{
    if (g_started)
        {
        {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
        }
        g_wake.notify_all();
        g_worker.join();
        g_started = false;
        }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_queue.clear();
    g_done.clear();
    g_queued.clear();
    g_running.clear();
    g_ready.clear();
}

} // namespace app::treescan
