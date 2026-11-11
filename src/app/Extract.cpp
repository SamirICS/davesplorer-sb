#include "app/Extract.h"
#include "app/AppState.h"
#include "app/Settings.h"
#include "app/PathUtil.h"
#include "app/Undo.h"
#include "platform/FileSystem.h"
#include "platform/Strings.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

namespace app
{

namespace
{

namespace fs = std::filesystem;

// Longest first, so "tar.gz" wins over "gz".
constexpr const char* kArchiveExtensions[] = {
    "tar.gz", "tar.bz2", "tar.xz", "tar.zst", "tar.lz4", "tar.lzma", "tar.lz", "tar.z",
    "tgz", "tbz", "tbz2", "txz", "tzst", "tlz", "taz",
    "zip", "zipx", "7z", "rar", "tar", "gz", "bz2", "xz", "zst", "lz4", "lzma", "lz", "z",
    "iso", "cab", "cpio", "deb", "rpm", "lha", "lzh", "xar", "arj",
    "dmg", "wim", "swm", "esd", "squashfs", "sfs", "msi", "chm", "vhd", "vhdx", "vmdk",
};

std::string Utf8(const fs::path& p)
{
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

fs::path Fs(const std::string& s)
{
    return fs::path(std::u8string(s.begin(), s.end()));
}

bool Exists(const std::string& path)
{
    std::error_code ec;
    return fs::exists(fs::symlink_status(Fs(path), ec));
}

// "name (2)", "name (3)"... whichever is free in dir. A file keeps its
// extension on the outside, as New file does; a folder's dots are its name.
std::string FreeName(const std::string& dir, const std::string& name, bool isFolder)
{
    if (!Exists(JoinPath(dir, name))) return name;
    const size_t dot = isFolder ? std::string::npos : name.find_last_of('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string ext  = dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
    for (int n = 2; n < 10000; ++n)
        {
        const std::string candidate = stem + " (" + std::to_string(n) + ")" + ext;
        if (!Exists(JoinPath(dir, candidate))) return candidate;
        }
    return name;
}

// The hidden folder an archive is unpacked into before anything is put in
// place. Inside the destination, so moving out of it is a rename.
bool MakeStaging(const std::string& destDir, std::string& staging, std::string& error)
{
    std::random_device rd;
    for (int attempt = 0; attempt < 100; ++attempt)
        {
        char suffix[9];
        snprintf(suffix, sizeof suffix, "%08x", (unsigned)rd());
        staging = JoinPath(destDir, std::string(".davesplorer-extract-") + suffix);
        std::error_code ec;
        if (fs::create_directory(Fs(staging), ec)) return true;
        if (ec)
            {
            error = "Can't unpack into " + destDir + ": " + ec.message();
            return false;
            }
        }
    error = "Can't make a folder to unpack into in " + destDir + ".";
    return false;
}

void RemoveTree(const std::string& path)
{
    std::error_code ec;
    fs::remove_all(Fs(path), ec);
}

void EmptyFolder(const std::string& path)
{
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(Fs(path), ec)) fs::remove_all(e.path(), ec);
}

struct StagedItem
{
    std::string name;
    bool        isFolder = false;
};

std::vector<StagedItem> ListStaged(const std::string& staging)
{
    std::vector<StagedItem> items;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(Fs(staging), ec))
        items.push_back({ Utf8(e.path().filename()), e.is_directory(ec) && !e.is_symlink(ec) });
    return items;
}

ExtractOutcome Failed(std::string error, bool cancelled = false)
{
    ExtractOutcome out;
    out.error = std::move(error);
    out.cancelled = cancelled;
    return out;
}

} // namespace

bool IsArchiveName(const std::string& name)
{
    return !ArchiveStem(name).empty() && ArchiveStem(name) != name;
}

std::string ArchiveStem(const std::string& name)
{
    const std::string lower = platform::ToLowerAscii(name);
    for (const char* ext : kArchiveExtensions)
        {
        const std::string dotted = std::string(".") + ext;
        if (lower.size() > dotted.size() && lower.compare(lower.size() - dotted.size(), dotted.size(), dotted) == 0)
            return name.substr(0, name.size() - dotted.size());
        }
    return name;
}

namespace
{

// One archive, nothing found inside it.
ExtractOutcome ExtractOne(const ExtractRequest& request, const ExtractQuestions& questions,
                          const std::atomic<bool>& cancel)
{
    if (!platform::PathExists(request.archive)) return Failed("'" + PathName(request.archive) + "' is no longer there.");
    if (!platform::IsDirectory(request.destDir)) return Failed("The folder " + request.destDir + " is no longer there.");

    // The table of contents first: it says whether a passphrase is wanted
    // and what would collide, before anything is unpacked.
    std::string passphrase;
    platform::ArchiveListing listing;
    for (bool wrong = false;;)
        {
        platform::ArchiveFailure why{};
        std::string error;
        if (platform::ListArchive(request.archive, passphrase, listing, why, error)) break;
        if (why != platform::ArchiveFailure::NeedPassphrase && why != platform::ArchiveFailure::WrongPassphrase)
            return Failed("Can't read '" + PathName(request.archive) + "'.\n" + error);
        if (!questions.passphrase || !questions.passphrase(wrong, passphrase)) return Failed("", true);
        wrong = true;
        }
    if (cancel) return Failed("", true);

    ConflictChoice choice = ConflictChoice::KeepBoth;
    if (!request.ownFolder)
        {
        std::vector<std::string> clashes;
        for (const platform::ArchiveTopItem& item : listing.topLevel)
            if (Exists(JoinPath(request.destDir, item.name))) clashes.push_back(item.name);
        if (!clashes.empty() && questions.conflict) choice = questions.conflict(clashes);
        if (choice == ConflictChoice::Cancel) return Failed("", true);
        }

    std::string staging, error;
    if (!MakeStaging(request.destDir, staging, error)) return Failed(error);
    ExtractOutcome out;
    for (;;)
        {
        platform::ArchiveFailure why{};
        error.clear();
        if (platform::ExtractArchive(request.archive, staging, passphrase, questions.progress, cancel, why, error, out.note)) break;
        // A zip's passphrase is only proved wrong once its data is read.
        const bool askAgain = why == platform::ArchiveFailure::WrongPassphrase || why == platform::ArchiveFailure::NeedPassphrase;
        if (askAgain && questions.passphrase && !cancel &&
            questions.passphrase(why == platform::ArchiveFailure::WrongPassphrase, passphrase))
            {
            EmptyFolder(staging);
            continue;
            }
        RemoveTree(staging);
        if (why == platform::ArchiveFailure::Cancelled || cancel || askAgain) return Failed("", true);
        return Failed("Can't extract '" + PathName(request.archive) + "'.\n" + error);
        }

    const std::vector<StagedItem> staged = ListStaged(staging);
    if (staged.empty())
        {
        RemoveTree(staging);
        out.ok = true;
        out.note = (out.note.empty() ? "" : out.note + "\n") + "'" + PathName(request.archive) + "' is empty.";
        return out;
        }

    if (request.ownFolder)
        {
        // One folder and nothing else is already the folder it should
        // unpack into; any other shape gets one named after the archive.
        if (staged.size() == 1 && staged[0].isFolder)
            {
            const std::string target = JoinPath(request.destDir, FreeName(request.destDir, staged[0].name, true));
            if (!platform::RenamePath(JoinPath(staging, staged[0].name), target, error))
                {
                RemoveTree(staging);
                return Failed(error);
                }
            RemoveTree(staging);
            out.placed.push_back(target);
            }
        else
            {
            std::string stem = ArchiveStem(PathName(request.archive));
            if (stem.empty()) stem = "Extracted";
            const std::string target = JoinPath(request.destDir, FreeName(request.destDir, stem, true));
            if (!platform::RenamePath(staging, target, error))
                {
                RemoveTree(staging);
                return Failed(error);
                }
            out.placed.push_back(target);
            }
        out.ok = true;
        return out;
        }

    // Beside what is already there. The listing may not have named
    // everything (7-Zip's and libarchive's ideas of a name can differ), so
    // a clash it did not see keeps both rather than replacing unasked.
    std::vector<std::string> failures;
    for (const StagedItem& item : staged)
        {
        std::string target = JoinPath(request.destDir, item.name);
        bool asked = false;
        for (const platform::ArchiveTopItem& t : listing.topLevel) asked = asked || t.name == item.name;
        if (Exists(target))
            {
            const ConflictChoice c = asked ? choice : ConflictChoice::KeepBoth;
            if (c == ConflictChoice::Skip) continue;
            if (c == ConflictChoice::Replace)
                {
                if (!platform::MoveToRecycleBin({ target }, error))
                    {
                    failures.push_back("'" + item.name + "' was kept: " + error);
                    continue;
                    }
                out.replaced.push_back(target);
                }
            else
                {
                target = JoinPath(request.destDir, FreeName(request.destDir, item.name, item.isFolder));
                }
            }
        if (platform::RenamePath(JoinPath(staging, item.name), target, error)) out.placed.push_back(target);
        else failures.push_back("'" + item.name + "': " + error);
        }
    RemoveTree(staging);
    for (const std::string& f : failures) out.note += (out.note.empty() ? "" : "\n") + f;
    out.ok = true;
    return out;
}

void AddNote(ExtractOutcome& out, const std::string& line)
{
    out.note += (out.note.empty() ? "" : "\n") + line;
}

bool IsUnder(const std::string& path, const std::string& dir)
{
    return path.size() > dir.size() && path.compare(0, dir.size(), dir) == 0 && path[dir.size()] == '/';
}

// Puts an unpacked archive away in the originals folder beside it. One
// already in such a folder stays put, so extracting it again does not
// nest a second one.
bool MoveOriginal(const std::string& archive, const std::string& folderName, std::string& movedTo, std::string& error)
{
    const std::string dir = ParentPath(archive);
    if (PathName(dir) == folderName) return false;
    const std::string folder = JoinPath(dir, folderName);
    if (!platform::IsDirectory(folder))
        {
        if (Exists(folder))
            {
            error = "'" + folderName + "' in " + PathName(dir) + " is a file, so '" + PathName(archive) + "' stayed where it was.";
            return false;
            }
        if (!platform::CreateFolder(folder, error)) return false;
        }
    movedTo = JoinPath(folder, FreeName(folder, PathName(archive), false));
    return platform::RenamePath(archive, movedTo, error);
}

// Every archive in what was just unpacked, not looking inside links or an
// originals folder.
void FindArchives(const std::string& path, const std::string& originalsFolder, std::vector<std::string>& found)
{
    std::error_code ec;
    const fs::file_status st = fs::symlink_status(Fs(path), ec);
    if (ec || fs::is_symlink(st)) return;
    if (fs::is_regular_file(st))
        {
        if (IsArchiveName(PathName(path))) found.push_back(path);
        return;
        }
    if (!fs::is_directory(st) || (!originalsFolder.empty() && PathName(path) == originalsFolder)) return;
    std::vector<std::string> children;
    for (const fs::directory_entry& e : fs::directory_iterator(Fs(path), ec)) children.push_back(Utf8(e.path()));
    std::sort(children.begin(), children.end());
    for (const std::string& c : children) FindArchives(c, originalsFolder, found);
}

// Archives inside what `out` placed, each into its own folder beside it,
// then whatever those held, down to kMaxNestedDepth.
void ExtractNested(const ExtractRequest& request, const ExtractQuestions& questions, const std::atomic<bool>& cancel,
                   ExtractOutcome& out)
{
    std::vector<std::string> roots = out.placed;
    for (int depth = 1; depth <= kMaxNestedDepth && !roots.empty(); ++depth)
        {
        std::vector<std::string> found;
        for (const std::string& r : roots) FindArchives(r, request.originalsFolder, found);
        roots.clear();
        for (const std::string& inner : found)
            {
            if (cancel)
                {
                AddNote(out, "Stopped before every archive inside was extracted.");
                return;
                }
            if (out.nestedExtracted >= kMaxNestedArchives)
                {
                AddNote(out, "Stopped after " + std::to_string(kMaxNestedArchives) + " archives found inside.");
                return;
                }
            if (questions.started) questions.started(inner);
            ExtractOutcome sub = ExtractOne({ inner, ParentPath(inner), true }, questions, cancel);
            if (!sub.ok)
                {
                if (!sub.cancelled) AddNote(out, sub.error);
                continue;
                }
            ++out.nestedExtracted;
            if (!sub.note.empty()) AddNote(out, sub.note);
            for (const std::string& p : sub.placed)
                {
                // Beside an archive at the top of an "Extract here": in the
                // destination, so Undo must know about it too.
                bool inside = false;
                for (const std::string& q : out.placed) inside = inside || IsUnder(p, q);
                if (!inside) out.placed.push_back(p);
                roots.push_back(p);
                }
            if (request.originalsFolder.empty()) continue;
            std::string movedTo, error;
            if (MoveOriginal(inner, request.originalsFolder, movedTo, error))
                {
                for (std::string& p : out.placed)
                    if (p == inner) p = movedTo;
                }
            else if (!error.empty())
                {
                AddNote(out, error);
                }
            }
        if (depth == kMaxNestedDepth && !roots.empty())
            AddNote(out, "Archives more than " + std::to_string(kMaxNestedDepth) + " deep were left packed.");
        }
}

} // namespace

ExtractRequest MakeExtractRequest(const Settings& settings, const std::string& archive, const std::string& destDir,
                                  bool ownFolder)
{
    ExtractRequest r{ archive, destDir, ownFolder };
    r.nested = settings.extractNested;
    std::string reason;
    if (settings.moveOriginals && IsValidFileName(settings.originalsFolder, reason)) r.originalsFolder = settings.originalsFolder;
    return r;
}

ExtractOutcome ExtractNow(const ExtractRequest& request, const ExtractQuestions& questions,
                          const std::atomic<bool>& cancel)
{
    ExtractOutcome out = ExtractOne(request, questions, cancel);
    if (!out.ok) return out;
    if (request.nested) ExtractNested(request, questions, cancel, out);
    if (!request.originalsFolder.empty())
        {
        std::string error;
        if (MoveOriginal(request.archive, request.originalsFolder, out.movedTo, error)) out.movedFrom = request.archive;
        else if (!error.empty()) AddNote(out, error);
        }
    return out;
}

void FinishExtract(AppState& state, const ExtractRequest& request, const ExtractOutcome& outcome)
{
    const std::string name = "'" + PathName(request.archive) + "'";
    if (!outcome.replaced.empty())
        {
        UndoEntry trash{ UndoKind::Trash, {}, "Replace " + std::to_string(outcome.replaced.size()) + " items from " + name };
        if (outcome.replaced.size() == 1) trash.label = "Replace '" + PathName(outcome.replaced[0]) + "' from " + name;
        for (const std::string& p : outcome.replaced) trash.items.push_back({ p, "" });
        RecordUndo(state, std::move(trash));
        }
    // Recorded before the extraction, so Undo takes the unpacked items away
    // first and puts the archive back second.
    if (!outcome.movedTo.empty())
        RecordUndo(state, { UndoKind::Move, { { outcome.movedFrom, outcome.movedTo } },
                            "Move " + name + " to '" + PathName(ParentPath(outcome.movedTo)) + "'" });
    UndoEntry entry{ UndoKind::Extract, {}, "Extract " + name };
    for (const std::string& p : outcome.placed) entry.items.push_back({ request.archive, p });
    RecordUndo(state, std::move(entry));
    RefreshAll(state);
    if (outcome.ok)
        {
        state.statusMessage = outcome.placed.size() == 1 && request.ownFolder
            ? "Extracted " + name + " to '" + PathName(outcome.placed[0]) + "'"
            : "Extracted " + name;
        if (outcome.nestedExtracted == 1) state.statusMessage += ", and 1 archive inside";
        else if (outcome.nestedExtracted > 1) state.statusMessage += ", and " + std::to_string(outcome.nestedExtracted) + " archives inside";
        }
}

namespace extractjob
{

namespace
{

struct Job
{
    ExtractRequest    request;
    std::atomic<bool> cancelled{ false };
};

// Interface thread only.
bool        g_started = false;
std::thread g_worker;

// Shared with the worker, under g_mutex.
std::mutex                        g_mutex;
std::condition_variable           g_wake;      // the worker: a job, or an answer
std::deque<std::shared_ptr<Job>>  g_queue;
std::shared_ptr<Job>              g_current;
Status                            g_status;
size_t                            g_done = 0;  // finished in this run
size_t                            g_total = 0;
bool                              g_stop = false;
bool                              g_answered = false;
bool                              g_answerOk = false;
std::string                       g_answerText;
ConflictChoice                    g_answerChoice = ConflictChoice::Cancel;
std::vector<std::pair<ExtractRequest, ExtractOutcome>> g_finished;

// Puts a question up and waits for the interface to answer it, or for
// Cancel, which answers no.
template <typename Fn>
void Ask(Job& job, Stage stage, Fn setUp)
{
    std::unique_lock<std::mutex> lock(g_mutex);
    g_answered = false;
    g_status.stage = stage;
    setUp();
    g_wake.wait(lock, [&] { return g_answered || job.cancelled || g_stop; });
    if (!g_answered)
        {
        g_answerOk = false;
        g_answerChoice = ConflictChoice::Cancel;
        }
    g_status.stage = Stage::Working;
    g_status.conflicts.clear();
}

void Run(Job& job)
{
    ExtractQuestions q;
    q.passphrase = [&](bool wrong, std::string& passphrase) {
        Ask(job, Stage::NeedPassphrase, [&] { g_status.wrongPassphrase = wrong; });
        std::lock_guard<std::mutex> lock(g_mutex);
        passphrase = g_answerText;
        g_answerText.clear();
        return g_answerOk;
    };
    q.conflict = [&](const std::vector<std::string>& names) {
        Ask(job, Stage::NeedConflictChoice, [&] { g_status.conflicts = names; });
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_answerChoice;
    };
    q.progress = [](float f) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.progress = f;
    };
    q.started = [](const std::string& archive) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_status.archive = archive;
        g_status.progress = 0.0f;
    };
    ExtractOutcome outcome = ExtractNow(job.request, q, job.cancelled);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_finished.emplace_back(job.request, std::move(outcome));
}

void WorkerMain()
{
    for (;;)
        {
        std::shared_ptr<Job> job;
        {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_wake.wait(lock, [] { return g_stop || !g_queue.empty(); });
        if (g_stop) return;
        job = g_queue.front();
        g_queue.pop_front();
        g_current = job;
        g_status = {};
        g_status.stage = Stage::Working;
        g_status.archive = job->request.archive;
        g_status.destDir = job->request.destDir;
        g_status.index = g_done + 1;
        g_status.count = g_total;
        }
        if (!job->cancelled) Run(*job);
        std::lock_guard<std::mutex> lock(g_mutex);
        g_current.reset();
        ++g_done;
        if (g_queue.empty())
            {
            g_status = {};
            g_done = g_total = 0;
            }
        }
}

} // namespace

void Start(const std::vector<ExtractRequest>& requests)
{
    if (requests.empty()) return;
    if (!g_started)
        {
        g_started = true;
        g_stop = false;
        g_worker = std::thread(WorkerMain);
        }
    {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const ExtractRequest& r : requests)
        {
        auto job = std::make_shared<Job>();
        job->request = r;
        g_queue.push_back(job);
        }
    g_total += requests.size();
    // Shown at once, so the interface never sees a gap between asking and
    // the worker picking the first one up.
    if (g_status.stage == Stage::Idle)
        {
        g_status.stage = Stage::Working;
        g_status.archive = requests[0].archive;
        g_status.destDir = requests[0].destDir;
        g_status.index = g_done + 1;
        }
    g_status.count = g_total;
    }
    g_wake.notify_all();
}

Status Poll()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_status;
}

void AnswerPassphrase(bool ok, const std::string& passphrase)
{
    {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_answered = true;
    g_answerOk = ok;
    g_answerText = passphrase;
    }
    g_wake.notify_all();
}

void AnswerConflict(ConflictChoice choice)
{
    {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_answered = true;
    g_answerChoice = choice;
    }
    g_wake.notify_all();
}

void Cancel()
{
    {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_current) g_current->cancelled = true;
    for (const std::shared_ptr<Job>& job : g_queue) job->cancelled = true;
    }
    g_wake.notify_all();
}

void Tick(AppState& state, std::vector<std::string>& messages)
{
    std::vector<std::pair<ExtractRequest, ExtractOutcome>> finished;
    {
    std::lock_guard<std::mutex> lock(g_mutex);
    finished.swap(g_finished);
    }
    for (const auto& [request, outcome] : finished)
        {
        if (outcome.cancelled) continue;
        FinishExtract(state, request, outcome);
        if (!outcome.error.empty()) messages.push_back(outcome.error);
        if (!outcome.note.empty()) messages.push_back("Extracted '" + PathName(request.archive) + "', but:\n" + outcome.note);
        }
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
    g_current.reset();
    g_finished.clear();
    g_status = {};
    g_done = g_total = 0;
}

} // namespace extractjob

} // namespace app
