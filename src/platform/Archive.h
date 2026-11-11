// Reading archives: what is in one, and unpacking it into a folder.
// libarchive does the work in-process; a 7-Zip program on the PATH takes
// over for what libarchive cannot read (encrypted 7z and RAR, disk images,
// WIM, MSI...). Linux only for now; elsewhere every call fails with a
// message saying so.
//
// Failures come back as false plus a UTF-8 message, and `why` says which
// failures the interface can do something about.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace platform
{

enum class ArchiveFailure
{
    None,
    NeedPassphrase,    // encrypted, and no passphrase was given
    WrongPassphrase,   // the passphrase given does not open it
    Needs7zip,         // only 7-Zip reads this, and none is installed
    Cancelled,
    Other,
};

// One name at the archive's top level: what extracting it puts beside
// everything already in the destination.
struct ArchiveTopItem
{
    std::string name;
    bool        isFolder = false;
};

struct ArchiveListing
{
    std::vector<ArchiveTopItem> topLevel;   // in the order first seen
    size_t   entries = 0;
    uint64_t bytes = 0;                     // uncompressed, where the format says
    bool     encrypted = false;
    bool     via7zip = false;               // 7-Zip read it, and will extract it
};

// Fraction done, 0 to 1. Called from the extracting thread.
using ArchiveProgressFn = std::function<void(float)>;

// The 7-Zip program to hand archives to, or "" when none is installed:
// 7zz (7-Zip's own Linux build), then 7z and 7za.
std::string SevenZipProgram();

// Reads the archive's table of contents without unpacking anything.
bool ListArchive(const std::string& path, const std::string& passphrase, ArchiveListing& out,
                 ArchiveFailure& why, std::string& error);

// Unpacks everything into destDir, which must exist and should be empty.
// Entries that would land outside it (absolute paths, "..") and device
// files are skipped, and `note` says how many. Returns as soon as it can
// once `cancel` is set, leaving destDir partly filled for the caller to
// throw away.
bool ExtractArchive(const std::string& path, const std::string& destDir, const std::string& passphrase,
                    const ArchiveProgressFn& progress, const std::atomic<bool>& cancel,
                    ArchiveFailure& why, std::string& error, std::string& note);

} // namespace platform
