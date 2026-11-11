#include "platform/Archive.h"
#include "platform/Strings.h"
#include <archive.h>
#include <archive_entry.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
namespace platform
{
namespace
{
namespace fs = std::filesystem;

bool HasExtension(const std::string& path, std::initializer_list<const char*> extensions)
{
    const std::string name = ToLowerAscii(fs::path(path).filename().string());
    for (const char* ext : extensions)
        {
        const std::string dotted = std::string(".") + ext;
        if (name.size() > dotted.size() && name.compare(name.size() - dotted.size(), dotted.size(), dotted) == 0) return true;
        }
    return false;
}

// Disk images and installer formats libarchive has no reader for. Its raw
// reader would happily "unpack" any of them as one opaque file, so these
// never reach it.
bool SevenZipOnly(const std::string& path)
{
    return HasExtension(path, {"dmg", "wim", "swm", "esd", "squashfs", "sfs", "msi", "chm", "vhd", "vhdx",
                               "vmdk", "arj", "img", "hfs", "ntfs", "fat", "mbr", "udf"});
}

// "./a//b\\c/" -> "a/b/c". Empty for anything that would land outside the
// destination: an absolute path or a ".." anywhere. Backslashes count as
// separators: zips made on Windows sometimes use them.
std::string SafeRelative(const char* raw)
{
    if (!raw || !*raw || raw[0] == '/') return {};
    std::string p(raw);
    std::replace(p.begin(), p.end(), '\\', '/');
    std::string out;
    size_t a = 0;
    while (a <= p.size())
        {
        const size_t b = std::min(p.find('/', a), p.size());
        const std::string part = p.substr(a, b - a);
        a = b + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return {};
        out += (out.empty() ? "" : "/") + part;
        }
    return out;
}

const char* EntryPath(archive_entry* entry)
{
    const char* utf8 = archive_entry_pathname_utf8(entry);
    return utf8 ? utf8 : archive_entry_pathname(entry);
}

// A bare .gz (or .xz, .zst...) holds one file and no name for it: it is
// named after the archive, less the compression's extension.
std::string RawName(const std::string& archivePath)
{
    const std::string name = fs::path(archivePath).filename().string();
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name + ".out" : name.substr(0, dot);
}

void AddTop(ArchiveListing& out, std::map<std::string, size_t>& seen, const std::string& rel, bool isFolder)
{
    const size_t slash = rel.find('/');
    const std::string first = rel.substr(0, slash);
    const bool folder = isFolder || slash != std::string::npos;
    auto [it, added] = seen.emplace(first, out.topLevel.size());
    if (added) out.topLevel.push_back({first, folder});
    else if (folder) out.topLevel[it->second].isFolder = true;
}

struct ReadCloser { void operator()(archive* a) const { archive_read_free(a); } };
struct WriteCloser { void operator()(archive* a) const { archive_write_free(a); } };
using Reader = std::unique_ptr<archive, ReadCloser>;
using Writer = std::unique_ptr<archive, WriteCloser>;

Reader OpenReader(const std::string& path, const std::string& passphrase, std::string& error)
{
    Reader a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());
    archive_read_support_format_raw(a.get());
    if (!passphrase.empty()) archive_read_add_passphrase(a.get(), passphrase.c_str());
    if (archive_read_open_filename(a.get(), path.c_str(), 64 * 1024) != ARCHIVE_OK)
        {
        error = archive_error_string(a.get()) ? archive_error_string(a.get()) : "The archive could not be opened.";
        return nullptr;
        }
    return a;
}

// The raw reader takes anything at all; it only means a compressed file
// when a decompression filter recognised the data first.
bool IsRawFormat(archive* a) { return archive_format(a) == ARCHIVE_FORMAT_RAW; }
bool RawButNotCompressed(archive* a) { return IsRawFormat(a) && archive_filter_count(a) < 2; }

// What libarchive's message means for the caller. Encryption it cannot
// undo, and formats it cannot read, are 7-Zip's to try.
enum class LibFailure { Other, Unreadable, NeedPassphrase, WrongPassphrase };
LibFailure Classify(archive* a, const std::string& passphrase)
{
    const std::string msg = ToLowerAscii(archive_error_string(a) ? archive_error_string(a) : "");
    if (msg.find("incorrect passphrase") != std::string::npos) return LibFailure::WrongPassphrase;
    if (msg.find("passphrase required") != std::string::npos)
        return passphrase.empty() ? LibFailure::NeedPassphrase : LibFailure::WrongPassphrase;
    if (msg.find("unrecognized archive format") != std::string::npos || msg.find("encrypt") != std::string::npos ||
        msg.find("not supported") != std::string::npos || msg.find("unsupported") != std::string::npos)
        return LibFailure::Unreadable;
    return LibFailure::Other;
}

// ---------------------------------------------------------------------------
// 7-Zip, as a child process.

// Runs 7-Zip with the passphrase on its stdin rather than its command line,
// where any user's ps could read it. Output (stdout and stderr together)
// goes to onOutput as it comes; cancel kills the child.
bool Run7z(const std::vector<std::string>& args, const std::string& passphrase,
           const std::function<void(const std::string&)>& onOutput, const std::atomic<bool>* cancel,
           int& exitCode, std::string& output, std::string& error)
{
    std::vector<std::string> argv{SevenZipProgram()};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> cargs;
    for (std::string& s : argv) cargs.push_back(s.data());
    cargs.push_back(nullptr);
    // Names come back in UTF-8 whatever the user's locale.
    gchar** env = g_environ_setenv(g_get_environ(), "LC_ALL", "C.UTF-8", TRUE);
    GPid pid = 0;
    int in = -1, out = -1, err = -1;
    GError* e = nullptr;
    const bool spawned = g_spawn_async_with_pipes(nullptr, cargs.data(), env, G_SPAWN_DO_NOT_REAP_CHILD, nullptr,
                                                  nullptr, &pid, &in, &out, &err, &e);
    g_strfreev(env);
    if (!spawned)
        {
        error = e ? e->message : "7-Zip could not be started.";
        if (e) g_error_free(e);
        return false;
        }
    // One line, then end of input: a second prompt (a wrong passphrase)
    // reads nothing and 7-Zip gives up instead of waiting.
    if (!passphrase.empty())
        {
        const std::string line = passphrase + "\n";
        ssize_t written = write(in, line.data(), line.size());
        (void)written;
        }
    close(in);
    pollfd fds[2] = {{out, POLLIN, 0}, {err, POLLIN, 0}};
    int open = 2;
    bool killed = false;
    char buf[4096];
    while (open > 0)
        {
        if (cancel && *cancel && !killed)
            {
            kill(pid, SIGTERM);
            killed = true;
            }
        if (poll(fds, 2, 100) <= 0) continue;
        for (pollfd& p : fds)
            {
            if (p.fd < 0 || !(p.revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = read(p.fd, buf, sizeof buf);
            if (n <= 0)
                {
                close(p.fd);
                p.fd = -1;
                --open;
                continue;
                }
            const std::string chunk(buf, (size_t)n);
            output += chunk;
            if (onOutput) onOutput(chunk);
            }
        }
    int status = 0;
    waitpid(pid, &status, 0);
    g_spawn_close_pid(pid);
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
    return true;
}

ArchiveFailure SevenZipFailure(const std::string& output, const std::string& passphrase)
{
    const std::string lower = ToLowerAscii(output);
    if (lower.find("wrong password") != std::string::npos || lower.find("enter password") != std::string::npos ||
        lower.find("cannot open encrypted archive") != std::string::npos)
        return passphrase.empty() ? ArchiveFailure::NeedPassphrase : ArchiveFailure::WrongPassphrase;
    return ArchiveFailure::Other;
}

// 7-Zip's own words for what went wrong: its "ERROR:" lines, or the tail.
std::string SevenZipMessage(const std::string& output)
{
    std::string lines;
    size_t a = 0;
    while (a < output.size())
        {
        const size_t b = std::min(output.find('\n', a), output.size());
        const std::string line = output.substr(a, b - a);
        if (line.rfind("ERROR", 0) == 0 || line.find("Error") != std::string::npos) lines += (lines.empty() ? "" : "\n") + line;
        a = b + 1;
        }
    if (lines.empty()) lines = output.size() > 400 ? output.substr(output.size() - 400) : output;
    return "7-Zip: " + lines;
}

bool List7z(const std::string& path, const std::string& passphrase, ArchiveListing& out, ArchiveFailure& why,
            std::string& error)
{
    int code = 0;
    std::string output;
    if (!Run7z({"l", "-slt", "-ba", "--", path}, passphrase, nullptr, nullptr, code, output, error))
        {
        why = ArchiveFailure::Other;
        return false;
        }
    if (code >= 2)
        {
        why = SevenZipFailure(output, passphrase);
        error = SevenZipMessage(output);
        return false;
        }
    // Blocks of "Key = value" lines, one per entry, a blank line between.
    out = {};
    out.via7zip = true;
    std::map<std::string, size_t> seen;
    std::string entryPath;
    bool folder = false;
    uint64_t size = 0;
    auto finish = [&] {
        if (entryPath.empty()) return;
        const std::string rel = SafeRelative(entryPath.c_str());
        if (!rel.empty()) AddTop(out, seen, rel, folder);
        ++out.entries;
        out.bytes += size;
        entryPath.clear();
        folder = false;
        size = 0;
    };
    size_t a = 0;
    while (a <= output.size())
        {
        const size_t b = std::min(output.find('\n', a), output.size());
        std::string line = output.substr(a, b - a);
        a = b + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) { finish(); continue; }
        const size_t eq = line.find(" = ");
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 3);
        if (key == "Path") { finish(); entryPath = value; }
        else if (key == "Folder") folder = folder || value == "+";
        else if (key == "Attributes") folder = folder || (!value.empty() && value[0] == 'D');
        else if (key == "Size") size = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "Encrypted" && value == "+") out.encrypted = true;
        }
    finish();
    // Names in the clear, contents not: asked for now rather than when
    // 7-Zip stops part way to prompt for it.
    if (out.encrypted && passphrase.empty())
        {
        why = ArchiveFailure::NeedPassphrase;
        error = "This archive is protected by a passphrase.";
        return false;
        }
    return true;
}

bool Extract7z(const std::string& path, const std::string& destDir, const std::string& passphrase,
               const ArchiveProgressFn& progress, const std::atomic<bool>& cancel, ArchiveFailure& why,
               std::string& error, std::string& note)
{
    // -bsp1 sends the percentage to stdout; -bso0 keeps the file names
    // quiet. 7-Zip refuses ".." and absolute names, and links pointing out
    // of the destination, unless told otherwise (-snld), which it is not.
    int code = 0;
    std::string output;
    auto onOutput = [&](const std::string& chunk) {
        if (!progress) return;
        for (size_t p = chunk.rfind('%'); p != std::string::npos && p > 0; p = p > 0 ? chunk.rfind('%', p - 1) : std::string::npos)
            {
            size_t s = p;
            while (s > 0 && std::isdigit((unsigned char)chunk[s - 1])) --s;
            if (s == p) continue;
            progress(std::min(1.0f, std::stoi(chunk.substr(s, p - s)) / 100.0f));
            return;
            }
    };
    if (!Run7z({"x", "-y", "-bsp1", "-bso0", "-o" + destDir, "--", path}, passphrase, onOutput, &cancel, code, output, error))
        {
        why = ArchiveFailure::Other;
        return false;
        }
    if (cancel)
        {
        why = ArchiveFailure::Cancelled;
        error = "Cancelled.";
        return false;
        }
    if (code >= 2)
        {
        why = SevenZipFailure(output, passphrase);
        error = why == ArchiveFailure::Other ? SevenZipMessage(output) : "The passphrase does not open this archive.";
        return false;
        }
    if (code == 1) note = "7-Zip finished with warnings; some items may be missing.";
    return true;
}

bool NeedSevenZip(ArchiveFailure& why, std::string& error)
{
    why = ArchiveFailure::Needs7zip;
    error = "This archive needs 7-Zip, which is not installed:\nsudo apt install 7zip";
    return false;
}

} // namespace

std::string SevenZipProgram()
{
    for (const char* name : {"7zz", "7z", "7za"})
        if (char* found = g_find_program_in_path(name))
            {
            std::string path = found;
            g_free(found);
            return path;
            }
    return {};
}

bool ListArchive(const std::string& path, const std::string& passphrase, ArchiveListing& out,
                 ArchiveFailure& why, std::string& error)
{
    why = ArchiveFailure::None;
    if (SevenZipOnly(path)) return SevenZipProgram().empty() ? NeedSevenZip(why, error) : List7z(path, passphrase, out, why, error);
    out = {};
    bool unreadable = false;
    Reader a = OpenReader(path, passphrase, error);
    if (!a) unreadable = true;
    std::map<std::string, size_t> seen;
    for (archive_entry* entry = nullptr; a && !unreadable;)
        {
        const int r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN || RawButNotCompressed(a.get()))
            {
            error = archive_error_string(a.get()) ? archive_error_string(a.get()) : "Unrecognized archive format";
            if (RawButNotCompressed(a.get()) || Classify(a.get(), passphrase) == LibFailure::Unreadable)
                {
                unreadable = true;
                break;
                }
            why = ArchiveFailure::Other;
            return false;
            }
        // Only a zip's encryption is libarchive's to undo.
        if (archive_entry_is_encrypted(entry))
            {
            out.encrypted = true;
            if ((archive_format(a.get()) & ARCHIVE_FORMAT_BASE_MASK) != ARCHIVE_FORMAT_ZIP)
                {
                unreadable = true;
                break;
                }
            }
        const std::string rel = IsRawFormat(a.get()) ? RawName(path) : SafeRelative(EntryPath(entry));
        if (!rel.empty()) AddTop(out, seen, rel, archive_entry_filetype(entry) == AE_IFDIR);
        ++out.entries;
        if (archive_entry_size_is_set(entry)) out.bytes += (uint64_t)archive_entry_size(entry);
        }
    if (unreadable)
        {
        if (SevenZipProgram().empty()) return NeedSevenZip(why, error);
        return List7z(path, passphrase, out, why, error);
        }
    if (out.encrypted && passphrase.empty())
        {
        why = ArchiveFailure::NeedPassphrase;
        error = "This archive is protected by a passphrase.";
        return false;
        }
    return true;
}

bool ExtractArchive(const std::string& path, const std::string& destDir, const std::string& passphrase,
                    const ArchiveProgressFn& progress, const std::atomic<bool>& cancel,
                    ArchiveFailure& why, std::string& error, std::string& note)
{
    why = ArchiveFailure::None;
    note.clear();
    // The listing has already decided who reads it; asking again is cheap
    // next to the unpacking, and keeps this call standing on its own.
    ArchiveListing listing;
    if (!ListArchive(path, passphrase, listing, why, error)) return false;
    if (listing.via7zip) return Extract7z(path, destDir, passphrase, progress, cancel, why, error, note);

    Reader a = OpenReader(path, passphrase, error);
    if (!a)
        {
        why = ArchiveFailure::Other;
        return false;
        }
    // No ARCHIVE_EXTRACT_PERM: the umask applies and set-id bits are
    // dropped. No owners, ACLs or attributes either: what is unpacked
    // belongs to the user, like a copy. The SECURE flags are libarchive's
    // own guard against writing outside destDir, behind ours; not
    // NOABSOLUTEPATHS, since every path handed over is destDir's, made
    // absolute here so the process's working folder never matters. The
    // symlink check walks the whole path, so destDir is its real path:
    // a linked folder above it is the user's, not the archive's.
    std::error_code ec;
    const fs::path real = fs::canonical(destDir, ec);
    const std::string root = ec ? destDir : real.string();
    Writer w(archive_write_disk_new());
    archive_write_disk_set_options(w.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                             ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    struct stat st{};
    const double fileBytes = stat(path.c_str(), &st) == 0 && st.st_size > 0 ? (double)st.st_size : 1.0;
    size_t unsafe = 0, special = 0, unwritten = 0;
    std::string firstUnwritten;
    auto fail = [&](archive* from, bool reading) {
        error = archive_error_string(from) ? archive_error_string(from) : "The archive could not be read.";
        const LibFailure f = reading ? Classify(from, passphrase) : LibFailure::Other;
        why = f == LibFailure::NeedPassphrase ? ArchiveFailure::NeedPassphrase
            : f == LibFailure::WrongPassphrase ? ArchiveFailure::WrongPassphrase
            : ArchiveFailure::Other;
        if (why == ArchiveFailure::WrongPassphrase) error = "The passphrase does not open this archive.";
        return false;
    };
    float lastReported = -1.0f;
    auto report = [&] {
        if (!progress) return;
        const float f = (float)std::min(1.0, (double)archive_filter_bytes(a.get(), -1) / fileBytes);
        if (f - lastReported >= 0.005f) { progress(f); lastReported = f; }
    };
    for (archive_entry* entry = nullptr;;)
        {
        if (cancel)
            {
            why = ArchiveFailure::Cancelled;
            error = "Cancelled.";
            return false;
            }
        const int r = archive_read_next_header(a.get(), &entry);
        if (r == ARCHIVE_EOF) break;
        if (r < ARCHIVE_WARN) return fail(a.get(), true);
        const unsigned type = archive_entry_filetype(entry);
        if (type != AE_IFREG && type != AE_IFDIR && type != AE_IFLNK)
            {
            ++special;
            continue;
            }
        const std::string rel = IsRawFormat(a.get()) ? RawName(path) : SafeRelative(EntryPath(entry));
        if (rel.empty())
            {
            ++unsafe;
            continue;
            }
        archive_entry_copy_pathname(entry, (root + "/" + rel).c_str());
        if (const char* link = archive_entry_hardlink(entry))
            {
            const std::string target = SafeRelative(link);
            if (target.empty())
                {
                ++unsafe;
                continue;
                }
            archive_entry_copy_hardlink(entry, (root + "/" + target).c_str());
            }
        const int h = archive_write_header(w.get(), entry);
        if (h == ARCHIVE_FATAL) return fail(w.get(), false);
        if (h < ARCHIVE_WARN)
            {
            // Refused by the SECURE checks (a path through a link), or a
            // name the filesystem will not take: that item alone is lost.
            if (unwritten++ == 0 && archive_error_string(w.get())) firstUnwritten = archive_error_string(w.get());
            continue;
            }
        for (;;)
            {
            const void* buf = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;
            const int d = archive_read_data_block(a.get(), &buf, &size, &offset);
            if (d == ARCHIVE_EOF) break;
            if (d < ARCHIVE_WARN) return fail(a.get(), true);
            if (archive_write_data_block(w.get(), buf, size, offset) < ARCHIVE_WARN) return fail(w.get(), false);
            if (cancel) break;
            report();
            }
        if (archive_write_finish_entry(w.get()) < ARCHIVE_WARN) return fail(w.get(), false);
        report();
        }
    if (archive_write_close(w.get()) < ARCHIVE_WARN) return fail(w.get(), false);
    if (progress) progress(1.0f);
    auto add = [&](size_t n, const std::string& what) {
        if (n) note += (note.empty() ? "" : "\n") + std::to_string(n) + " " + what;
    };
    add(unsafe, unsafe == 1 ? "item with a path leading outside the folder was skipped." : "items with paths leading outside the folder were skipped.");
    add(special, special == 1 ? "device or pipe was skipped." : "devices or pipes were skipped.");
    add(unwritten, (unwritten == 1 ? "item could not be written: " : "items could not be written, the first: ") + firstUnwritten);
    return true;
}

} // namespace platform
