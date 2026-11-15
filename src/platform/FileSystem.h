// Directory listing, drives, known folders and file operations on Win32.
//
// Failures come back as false plus a UTF-8 message the interface can show.
// No exceptions cross this boundary.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform
{

struct FileEntry
{
    std::string  name;          // UTF-8 display name
    std::wstring nameWide;      // kept for natural-order sorting without re-conversion
    std::string  path;          // full UTF-8 path
    std::string  extension;     // lower-case, no dot, empty for folders
    uint64_t     size = 0;
    int64_t      modified = 0;  // UTC: FILETIME ticks on Windows, Unix seconds on Linux
    bool         isDirectory = false;
    bool         isHidden = false;
    bool         isSystem = false;
    bool         isReadOnly = false;
    bool         isReparsePoint = false;
    bool         isExecutable = false;  // a program this user can run; only the Linux listing fills it in
};

struct DriveInfo
{
    std::string root;           // "C:\"
    std::string letter;         // "C:"
    std::string label;          // volume label, may be empty
    std::string displayName;    // "Local Disk (C:)" the way Explorer shows it
    std::string typeName;       // "Local Disk", "Removable Disk", "CD Drive", "Network Drive"
    uint64_t    totalBytes = 0;
    uint64_t    freeBytes = 0;
    bool        ready = false;  // media present and readable
};

enum class KnownFolderKind
{
    Home,
    Desktop,
    Documents,
    Downloads,
    Pictures,
    Music,
    Videos,
};

struct KnownFolder
{
    KnownFolderKind kind;
    std::string     name;
    std::string     path;
};

bool ListDirectory(const std::string& path, std::vector<FileEntry>& out, std::string& error);

std::vector<DriveInfo>   GetDrives();
// Whether drives were mounted or unmounted since the last call (false on
// the first). Cheap enough to ask every frame; never blocks.
bool                     DrivesChanged();
std::vector<KnownFolder> GetKnownFolders();

bool PathExists(const std::string& path);
bool IsDirectory(const std::string& path);

// A compiled program rather than a script, a library or data with the
// execute bit: on Linux an ELF file that is an executable, or a
// position-independent one (a shared object with an interpreter). The
// caller has checked the execute bit already. False on other platforms.
bool IsElfProgram(const std::string& path);

// What a folder is, whatever path reached it: two paths with the same
// identity are the same folder (a link and its target, say). Empty when it
// cannot be told, and always on platforms other than Linux.
std::string FolderIdentity(const std::string& path);

// One entry as the folder-size walk sees it: no type name, no wide string,
// no permission checks, so a large tree costs one stat per entry at most.
struct SizeChild
{
    std::string name;
    uint64_t    size = 0;       // regular files only; 0 for anything else
    int64_t     modified = 0;   // same units as FileEntry::modified
    uint64_t    device = 0;     // 0 where the platform cannot say cheaply
    uint64_t    inode = 0;
    uint32_t    links = 1;      // hard links to this file
    bool        isDirectory = false;   // a real folder, not a link to one
    bool        isLink = false;        // a link of any kind; never followed
};

// The children of one folder for summing sizes. Links are reported, not
// followed. False when the folder cannot be read.
bool ListSizes(const std::string& path, std::vector<SizeChild>& out);

// Where a folder physically lives, so a size walk can go into a USB disk
// mounted under it but not into a network share or /proc.
enum class MountKind
{
    Local,
    Network,
    Pseudo,
};
MountKind ClassifyMount(const std::string& path);

#ifdef __linux__
// The rule behind ClassifyMount, by filesystem type name as
// /proc/self/mountinfo spells it ("ext4", "fuseblk", "fuse.sshfs", "nfs4").
MountKind ClassifyFsType(const std::string& fsType);
#endif

// The device a folder is on, as ListSizes reports it; 0 when unknown.
uint64_t FolderDevice(const std::string& path);

// Drops the calling thread to the lowest CPU and disk priority the OS
// offers, for background work nobody is waiting on.
void LowerCurrentThreadPriority();

// "Text Document", "File folder", "PNG File"... as the shell names them.
std::string FileTypeName(const std::string& extension, bool isDirectory);

// Local time in the user's short date and time format, like Explorer's
// "Date modified" column.
std::string FormatDateTime(int64_t fileTimeTicks);

// Human-readable message for a Win32 error code.
std::string WinErrorMessage(unsigned long code);

bool CreateFolder(const std::string& path, std::string& error);
bool CreateEmptyFile(const std::string& path, std::string& error);
bool RenamePath(const std::string& from, const std::string& to, std::string& error);

bool DeletePermanently(const std::vector<std::string>& paths, std::string& error);
bool MoveToRecycleBin(const std::vector<std::string>& paths, std::string& error);
// Puts the item last sent to the Recycle Bin from originalPath back there.
// Never overwrites: fails when something already sits at originalPath.
bool RestoreFromRecycleBin(const std::string& originalPath, std::string& error);

// Shell copy/move: the OS shows its own progress and conflict dialogs, which
// is exactly the behaviour Explorer users expect. renameOnCollision makes a
// copy into the same folder produce "name - Copy".
bool CopyPaths(const std::vector<std::string>& sources, const std::string& destDir,
               bool renameOnCollision, std::string& error);
bool MovePaths(const std::vector<std::string>& sources, const std::string& destDir, std::string& error);

} // namespace platform
