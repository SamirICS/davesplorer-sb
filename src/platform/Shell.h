// Handing things to the Windows shell: launching files, revealing them in
// Explorer, opening a terminal.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace platform
{

// The thumbnail the desktop itself would show for a file (a frame of a
// video, say), fitted within maxDim x maxDim: 8-bit RGBA, premultiplied
// alpha, rows top to bottom. Fails rather than falling back to the file
// type's icon. Blocks; meant for a worker thread.
bool ShellThumbnail(const std::string& path, int maxDim, int& width, int& height,
                    std::vector<uint8_t>& rgba, std::string& error);

// Opens a file with its associated program, or a folder in Explorer.
bool OpenWithShell(const std::string& path, std::string& error);

// Windows Explorer with the item selected.
bool ShowInExplorer(const std::string& path, std::string& error);

// Windows Explorer showing a folder ("" for This PC). Always Explorer
// itself: once Davesplorer is registered as the folder handler, a plain
// "open the folder" request would come straight back here. And once the
// interceptor shim is installed, so would explorer.exe by name; both this
// and ShowInExplorer then run the shim's private copy of the real Explorer.
bool OpenFolderInExplorer(const std::string& folder, std::string& error);

// A command prompt in the given folder.
bool OpenTerminalAt(const std::string& dir, std::string& error);

// Starts a program: the file itself is argv[0], nothing is handed to a
// shell, and it runs in workingDir -- or in its own folder when that is
// empty -- so a program finds the files it keeps beside itself.
bool RunExecutable(const std::string& path, const std::string& workingDir, std::string& error);

// The desktop's list of pinned applications -- the GNOME dash. Only GNOME
// keeps one we can write, so this is false everywhere else and the menu
// entry stays greyed out.
bool CanPinApplications();

// Whether the dash already holds this item. Reads only: nothing is
// installed or generated to answer the question.
bool IsPinned(const std::string& path);

// Adds the item to the dash, first installing or generating whatever
// launcher the dash needs to name it by, or takes it back out again.
// Unpinning leaves a generated launcher on disk: re-pinning is then
// immediate, and nothing the user may have edited is deleted for them.
bool PinApplication(const std::string& path, std::string& error);
bool UnpinApplication(const std::string& path, std::string& error);

} // namespace platform
