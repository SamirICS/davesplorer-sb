// The Programs view: every compiled program under a folder, found off the
// interface thread and listed by project, so build outputs scattered over
// many projects are in one place.
//
// The walk skips what holds no programs worth listing -- hidden folders
// (.git among them), symbolic links, node_modules, the build systems' own
// scratch (CMakeFiles, FetchContent's _deps, Cargo's deps and build) -- and
// keeps only files that are ELF programs, not scripts or libraries. What it
// has found comes back while it is still going, so a big tree fills in
// rather than appearing at the end.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app
{

struct AppState;

struct Program
{
    std::string path;       // full path
    std::string name;
    std::string project;    // first folder under the scanned root; empty for a program in the root itself
    std::string relDir;     // its folder below the project, "" when it sits in the project folder
    uint64_t    size = 0;
    int64_t     modified = 0;
};

// One Programs window. Its id comes from the same counter as the folder
// views, so window ids never collide.
struct ProgramsView
{
    int                  id = 0;
    std::string          root;
    std::vector<Program> programs;   // in the order found
    size_t               folders = 0;       // walked so far
    bool                 scanning = false;
    bool                 truncated = false; // stopped at a limit; the list is not complete
    std::string          filter;            // matched against name and path
};

inline constexpr int    kMaxProgramDepth = 24;
inline constexpr size_t kMaxProgramFolders = 300000;

// Folders the walk never enters, by name. Inside a Cargo profile folder
// (target/debug, target/release: the one holding .cargo-lock) it also
// skips deps, build and incremental, where every test binary and build
// script lands under a hashed name; the programs themselves sit beside them.
bool SkipFolderForPrograms(const std::string& name, bool inCargoProfile = false);

// Fills in project and relDir from the path, relative to root.
void PlaceProgram(const std::string& root, Program& program);

// One project's programs: indices into ProgramsView::programs, newest first.
struct ProgramGroup
{
    std::string      project;
    std::vector<int> rows;
    int64_t          newest = 0;
};

// The programs matching the filter (case-insensitive, name or path), by
// project; the project with the newest build first.
std::vector<ProgramGroup> GroupPrograms(const std::vector<Program>& programs, const std::string& filter);

// Opens a Programs view on root and starts its walk, or returns the view
// already showing root. Returns the index into state.programViews.
int  OpenProgramsView(AppState& state, const std::string& root);
void CloseProgramsView(AppState& state, int index);
// Forgets what was found and walks again.
void RescanPrograms(ProgramsView& view);
// Once per frame: what the walks found since the last frame.
void PumpProgramScans(AppState& state);

namespace programscan
{

// Stops the worker. Before the program exits.
void Shutdown();

} // namespace programscan

} // namespace app
