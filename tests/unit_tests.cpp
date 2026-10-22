// Console unit tests for the application layer. No window, no ImGui.
// Exit code is the number of failed checks.

#include "app/AppState.h"
#include "app/FileOps.h"
#include "app/Format.h"
#include "app/GitRemote.h"
#include "app/Image.h"
#include "app/Layout.h"
#include "app/Listing.h"
#include "app/Navigation.h"
#include "app/PathUtil.h"
#include "app/Selection.h"
#include "app/Settings.h"
#include "app/Shortcuts.h"
#include "app/TreeScan.h"
#include "platform/Clipboard.h"
#include "platform/FileSystem.h"
#include "platform/Shell.h"
#include "platform/Strings.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <SDL3/SDL.h>
#endif
#ifdef __linux__
#include <gio/gio.h>
#endif
#include <filesystem>

#include <chrono>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace
{

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                  \
    do                                                                                               \
        {                                                                                            \
        ++g_checks;                                                                                  \
        if (!(cond))                                                                                 \
            {                                                                                        \
            ++g_failures;                                                                            \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                              \
            }                                                                                        \
        }                                                                                            \
    while (0)

#define CHECK_EQ(a, b)                                                                               \
    do                                                                                               \
        {                                                                                            \
        ++g_checks;                                                                                  \
        if (!((a) == (b)))                                                                           \
            {                                                                                        \
            ++g_failures;                                                                            \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                       \
            }                                                                                        \
        }                                                                                            \
    while (0)

// A scratch folder under %TEMP% that is removed when the test ends.
struct TempDir
{
    std::string path;
    TempDir(const char* tag)
    {
#ifdef _WIN32
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        path = platform::WideToUtf8(tmp) + "davesplorer_" + tag + "_" + std::to_string(GetCurrentProcessId());
#else
        path = (std::filesystem::temp_directory_path() / ("davesplorer_" + std::string(tag) + "_" + std::to_string(getpid()))).string();
#endif
        std::string error;
        platform::CreateFolder(path, error);
    }
    ~TempDir()
    {
        std::string error;
        platform::DeletePermanently({ path }, error);
    }
    std::string file(const char* name, const char* content = "x")
    {
        const std::string p = app::JoinPath(path, name);
        std::ofstream(std::filesystem::path(reinterpret_cast<const char8_t*>(p.c_str()))) << content;
        return p;
    }
    std::string dir(const char* name)
    {
        const std::string p = app::JoinPath(path, name);
        std::string error;
        platform::CreateFolder(p, error);
        return p;
    }
};

// The clipboard tests use the real clipboard. Whatever text was there is put
// back afterwards; anything else is lost, which is the price of testing the
// real thing.
#ifdef _WIN32
struct ClipboardKeeper
{
    std::wstring text;
    bool hadText = false;
    ClipboardKeeper()
    {
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(nullptr)) return;
        if (HGLOBAL h = GetClipboardData(CF_UNICODETEXT))
            if (const wchar_t* p = (const wchar_t*)GlobalLock(h))
                {
                text = p;
                hadText = true;
                GlobalUnlock(h);
                }
        CloseClipboard();
    }
    ~ClipboardKeeper()
    {
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        if (hadText)
            {
            const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
            if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes))
                {
                memcpy(GlobalLock(h), text.c_str(), bytes);
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
                }
            }
        CloseClipboard();
    }
};

#else
struct ClipboardKeeper {};
#endif

void TestPathUtil()
{
    using namespace app;
#ifdef _WIN32
    CHECK_EQ(NormalizePath("c:"), "C:\\");
    CHECK_EQ(NormalizePath("c:/users/dave/"), "C:\\users\\dave");
    CHECK_EQ(NormalizePath("  C:\\x\\\\y\\ "), "C:\\x\\y");
    CHECK_EQ(NormalizePath("C:\\"), "C:\\");
    CHECK_EQ(NormalizePath(""), "");

    CHECK_EQ(ParentPath("C:\\Users\\dave"), "C:\\Users");
    CHECK_EQ(ParentPath("C:\\Users"), "C:\\");
    CHECK_EQ(ParentPath("C:\\"), "");
    CHECK_EQ(ParentPath(""), "");

    CHECK_EQ(PathName("C:\\Users\\dave"), "dave");
    CHECK_EQ(PathName("C:\\"), "C:\\");
    CHECK_EQ(PathName(""), "This PC");

    CHECK_EQ(JoinPath("C:\\", "x"), "C:\\x");
    CHECK_EQ(JoinPath("C:\\a", "x"), "C:\\a\\x");
    CHECK_EQ(JoinPath("", "x"), "x");

    CHECK(IsDriveRoot("D:\\"));
    CHECK(!IsDriveRoot("D:\\x"));

    const std::vector<Crumb> crumbs = Breadcrumbs("C:\\Users\\dave");
    CHECK_EQ(crumbs.size(), (size_t)4);
    CHECK_EQ(crumbs[0].label, "This PC");
    CHECK_EQ(crumbs[0].path, "");
    CHECK_EQ(crumbs[1].label, "C:");
    CHECK_EQ(crumbs[1].path, "C:\\");
    CHECK_EQ(crumbs[2].path, "C:\\Users");
    CHECK_EQ(crumbs[3].path, "C:\\Users\\dave");
    CHECK_EQ(Breadcrumbs("").size(), (size_t)1);

    const std::vector<Crumb> unc = Breadcrumbs("\\\\server\\share\\folder");
    CHECK_EQ(unc.size(), (size_t)3);
    CHECK_EQ(unc[1].path, "\\\\server\\share");
    CHECK_EQ(unc[2].path, "\\\\server\\share\\folder");

    std::string reason;
    CHECK(IsValidFileName("hello.txt", reason));
    CHECK(!IsValidFileName("", reason));
    CHECK(!IsValidFileName("a/b", reason));
    CHECK(!IsValidFileName("a:b", reason));
    CHECK(!IsValidFileName("con", reason));
    CHECK(!IsValidFileName("COM1.txt", reason));
    CHECK(!IsValidFileName("trailing.", reason));
    CHECK(!IsValidFileName("..", reason));
#else
    CHECK_EQ(NormalizePath("/tmp//one/../two/"), "/tmp/two");
    CHECK_EQ(NormalizePath("/tmp/with\\backslash "), "/tmp/with\\backslash ");
    CHECK_EQ(NormalizePath(""), "");
    CHECK_EQ(ParentPath("/home"), "/");
    CHECK_EQ(ParentPath("/"), "");
    CHECK_EQ(PathName("/"), "/");
    CHECK_EQ(JoinPath("/", "home"), "/home");
    CHECK(IsDriveRoot("/"));
    const auto crumbs = Breadcrumbs("/home/dave");
    CHECK_EQ(crumbs.size(), (size_t)4);
    CHECK_EQ(crumbs[1].path, "/");
    CHECK_EQ(crumbs[3].path, "/home/dave");
    std::string reason;
    CHECK(IsValidFileName("con", reason));
    CHECK(IsValidFileName("a:b\\c ", reason));
    CHECK(!IsValidFileName("", reason));
    CHECK(!IsValidFileName("a/b", reason));
    CHECK(!IsValidFileName("..", reason));
    CHECK(!IsValidFileName(std::string("a\0b", 3), reason));
#endif
}

#ifndef _WIN32
void TestUnixFileSystem()
{
    TempDir tmp("unix");
    std::string error;
    const std::string source = tmp.dir("Source");
    // A sibling that differs only in case is a second folder on a
    // case-sensitive filesystem and the very same folder on one that folds
    // case, which APFS does by default; dropping onto it is a no-op there.
    const bool caseSensitive = !platform::PathExists(app::JoinPath(tmp.path, "source"));
    const std::string cased = tmp.dir("source");
    const std::string other = tmp.dir("other");
    const std::string nested = app::JoinPath(source, "nested");
    CHECK(platform::CreateFolder(nested, error));
    const std::string file = tmp.file("café #100% \\ file.txt", "original");
    CHECK(platform::CopyPaths({file}, source, false, error));
    const std::string copied = app::JoinPath(source, app::PathName(file));
    CHECK(!platform::CopyPaths({file}, source, false, error));
    CHECK(!error.empty());
    CHECK(!platform::CreateEmptyFile(file, error));
    const std::string collision = tmp.file("collision.txt", "keep me");
    CHECK(!platform::RenamePath(file, collision, error));
    std::string contents;
    std::getline(std::ifstream(collision), contents);
    CHECK_EQ(contents, "keep me");
    CHECK(platform::PathExists(file));
    CHECK_EQ(app::CanDropOn({source}, cased), caseSensitive);
    CHECK(app::CanDropOn({source}, other));
    CHECK(!app::CanDropOn({source}, nested));
    const std::string alias = app::JoinPath(tmp.path, "alias");
    std::filesystem::create_directory_symlink(nested, alias);
    CHECK(!app::CanDropOn({source}, alias));
    CHECK(!platform::CopyPaths({source}, alias, false, error));
    CHECK(!platform::MovePaths({source}, alias, error));
    CHECK(platform::CopyPaths({source}, other, false, error));
    CHECK(platform::PathExists(app::JoinPath(app::JoinPath(other, "Source"), app::PathName(file))));
    const std::string link = app::JoinPath(tmp.path, "dangling");
    std::filesystem::create_symlink("absent-target", link);
    CHECK(platform::PathExists(link));
    CHECK(!platform::IsDirectory(link));
    CHECK(platform::CopyPaths({link}, other, false, error));
    CHECK(std::filesystem::is_symlink(app::JoinPath(other, "dangling")));
    CHECK(platform::MovePaths({copied}, nested, error));
    CHECK(!platform::PathExists(copied));
    CHECK(platform::PathExists(app::JoinPath(nested, app::PathName(file))));
    CHECK_EQ(platform::WideToUtf8(platform::Utf8ToWide("café 日本語")), "café 日本語");
    CHECK(platform::ContainsNoCase("CAFÉ", "café"));
    CHECK(platform::NaturalCompare(L"file2", L"file10") < 0);
    CHECK(platform::SetClipboardFiles({file}, true, error));
    std::vector<std::string> files;
    bool cut = false;
    CHECK(platform::GetClipboardFiles(files, cut));
    CHECK_EQ(files.size(), (size_t)1);
    if (!files.empty()) CHECK_EQ(files[0], file);
    CHECK(cut);
    const auto sequence = platform::ClipboardSequence();
#ifdef __APPLE__
    // macOS keeps the file list on a pasteboard, which SDL's clipboard does
    // not share; clearing it is what supersedes the list there.
    CHECK(platform::ClearClipboard());
    CHECK(platform::ClipboardSequence() != sequence);
    CHECK(!platform::ClipboardHasFiles());
#else
    CHECK(SDL_SetClipboardText("replacement text"));
    CHECK(platform::ClipboardSequence() != sequence);
    CHECK(!platform::ClipboardHasFiles());
    CHECK(platform::ClearClipboard());
#endif

    app::AppState state;
    const std::string config = tmp.dir("config");
    app::InitAppState(state, tmp.path, config);
    CHECK_EQ(state.settingsFile, app::JoinPath(config, "settings.ini"));
    CHECK_EQ(state.layoutFile, app::JoinPath(config, "imgui.ini"));
    CHECK_EQ(state.exeDir, tmp.path);
}
#endif

#ifdef __linux__
void TestLinuxExecutables()
{
    TempDir tmp("exec");
    std::string error;
    const std::string plain  = tmp.file("notes.txt", "just text");
    const std::string folder = tmp.dir("folder");
    // A program that leaves proof of where it ran.
    const std::string program = tmp.file("program", "#!/bin/sh\npwd -P > ran.txt\n");
    std::filesystem::permissions(program, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);

    std::vector<platform::FileEntry> entries;
    CHECK(platform::ListDirectory(tmp.path, entries, error));
    int programs = 0;
    for (const platform::FileEntry& e : entries)
        {
        if (e.isExecutable) ++programs;
        if (e.path == program) CHECK(e.isExecutable);
        // Plain text is not a program, and neither is a folder, which
        // carries the execute bit only to say it may be entered.
        if (e.path == plain || e.path == folder) CHECK(!e.isExecutable);
        }
    CHECK_EQ(programs, 1);

    // Started in the folder it was given, not in this process's own.
    CHECK(platform::RunExecutable(program, folder, error));
    const std::string marker = app::JoinPath(folder, "ran.txt");
    for (int i = 0; i < 400 && !platform::PathExists(marker); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::string ranIn;
    std::getline(std::ifstream(marker), ranIn);
    CHECK_EQ(ranIn, std::filesystem::canonical(folder).string());

    // Nothing to run: a message comes back, and nothing is handed to a shell.
    CHECK(!platform::RunExecutable(plain, tmp.path, error));
    CHECK(!error.empty());
}

// The Programs view's walk: compiled programs only, the noise skipped, and
// grouped by project with the newest build first.
void TestProgramScan()
{
    namespace fs = std::filesystem;
    TempDir tmp("programs");
    std::string error;
    // A real program and a real library to copy, so the ELF check sees
    // what a build produces.
    const std::string elf = "/usr/bin/true";
    std::string lib;
    for (const char* candidate : { "/lib/x86_64-linux-gnu/libz.so.1", "/usr/lib/x86_64-linux-gnu/libz.so.1",
                                   "/lib/aarch64-linux-gnu/libz.so.1", "/usr/lib/libz.so.1", "/usr/lib64/libz.so.1" })
        if (platform::PathExists(candidate)) lib = candidate;
    CHECK(platform::PathExists(elf));
    auto place = [&](const std::string& rel, const std::string& from, bool executable) {
        const fs::path dest = fs::path(tmp.path) / rel;
        fs::create_directories(dest.parent_path());
        fs::copy_file(from, dest, fs::copy_options::overwrite_existing);
        fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write |
                                  (executable ? fs::perms::owner_exec : fs::perms::none));
        return dest.string();
    };
    const std::string app = place("projA/build/stage/app", elf, true);
    const std::string tool = place("projB/out/tool", elf, true);
    const std::string top = place("toplevel", elf, true);
    place("projA/build/CMakeFiles/3.28/CompilerIdC/a.out", elf, true);   // CMake's scratch
    place("projA/build/_deps/sdl3-build/testprog", elf, true);           // someone else's tests
    place("projA/.git/hooks/hook", elf, true);                           // hidden
    place("projA/node_modules/x/bin", elf, true);
    // Cargo: the program beside its profile's lock, its hashed copies and
    // build scripts in deps and build.
    const std::string cargo = place("projC/target/release/cli", elf, true);
    tmp.file("projC/target/release/.cargo-lock", "");
    place("projC/target/release/deps/cli-0123456789abcdef", elf, true);
    place("projC/target/release/build/cli-0123456789abcdef/build-script-build", elf, true);
    const std::string example = place("projC/target/release/examples/demo", elf, true);
    place("projC/target/release/examples/demo-0123456789abcdef", elf, true);
    const std::string notExecutable = place("projA/data/copy", elf, false);
    const std::string script = tmp.file("projA/run.sh", "#!/bin/sh\necho hi\n");
    fs::permissions(script, fs::perms::owner_exec, fs::perm_options::add);
    fs::create_directory_symlink(fs::path(tmp.path) / "projA", fs::path(tmp.path) / "projA/loop");
    fs::create_symlink(app, fs::path(tmp.path) / "projB/app-link");

    // The ELF check on its own.
    CHECK(platform::IsElfProgram(app));
    CHECK(!platform::IsElfProgram(script));
    CHECK(!platform::IsElfProgram(app::JoinPath(tmp.path, "projA")));
    if (!lib.empty())
        {
        // A library is not a program, by its name or, renamed, by having
        // no interpreter.
        CHECK(!platform::IsElfProgram(place("projA/lib/libz.so.1", lib, true)));
        CHECK(!platform::IsElfProgram(place("projA/lib/zlib-renamed", lib, true)));
        }

    // projB was built last.
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(app, now - std::chrono::hours(48));
    fs::last_write_time(top, now - std::chrono::hours(72));
    fs::last_write_time(tool, now - std::chrono::hours(1));
    fs::last_write_time(cargo, now - std::chrono::hours(24));
    fs::last_write_time(example, now - std::chrono::hours(30));

    CHECK(app::SkipFolderForPrograms("CMakeFiles"));
    CHECK(app::SkipFolderForPrograms("_deps"));
    CHECK(!app::SkipFolderForPrograms("build"));
    CHECK(!app::SkipFolderForPrograms("deps"));
    CHECK(app::SkipFolderForPrograms("deps", true));

    app::AppState state;
    CHECK_EQ(app::OpenProgramsView(state, tmp.path), 0);
    CHECK_EQ(app::OpenProgramsView(state, tmp.path), 0);   // the same folder: the same view
    CHECK(state.programViews[0].scanning);
    for (int i = 0; i < 4000 && state.programViews[0].scanning; ++i)
        {
        app::TickAppState(state);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    const app::ProgramsView& view = state.programViews[0];
    CHECK(!view.scanning);
    CHECK(!view.truncated);
    CHECK(view.folders > 5);
    CHECK_EQ(view.programs.size(), (size_t)5);
    for (const app::Program& p : view.programs)
        {
        if (p.path == app)
            {
            CHECK_EQ(p.project, "projA");
            CHECK_EQ(p.relDir, "build/stage");
            CHECK_EQ(p.name, "app");
            }
        if (p.path == tool) CHECK_EQ(p.relDir, "out");
        if (p.path == cargo) CHECK_EQ(p.relDir, "target/release");
        if (p.path == example) CHECK_EQ(p.relDir, "target/release/examples");
        if (p.path == top) CHECK_EQ(p.project, "");
        CHECK(p.path != notExecutable);
        }

    const std::vector<app::ProgramGroup> groups = app::GroupPrograms(view.programs, "");
    CHECK_EQ(groups.size(), (size_t)4);
    if (groups.size() == 4)
        {
        CHECK_EQ(groups[0].project, "projB");
        CHECK_EQ(groups[1].project, "projC");
        CHECK_EQ(groups[2].project, "projA");
        CHECK_EQ(groups[3].project, "");
        }
    // The search looks at the path, not only the name, and ignores case.
    const std::vector<app::ProgramGroup> stage = app::GroupPrograms(view.programs, "STAGE");
    CHECK_EQ(stage.size(), (size_t)1);
    if (stage.size() == 1) CHECK_EQ(view.programs[(size_t)stage[0].rows[0]].path, app);
    CHECK(app::GroupPrograms(view.programs, "nothing-like-this").empty());

    // A rescan starts over and finds the same.
    app::RescanPrograms(state.programViews[0]);
    CHECK(state.programViews[0].programs.empty());
    for (int i = 0; i < 4000 && state.programViews[0].scanning; ++i)
        {
        app::TickAppState(state);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    CHECK_EQ(state.programViews[0].programs.size(), (size_t)5);
    app::CloseProgramsView(state, 0);
    CHECK(state.programViews.empty());
}

// How many times the dash holds this id. Safe to call only once
// CanPinApplications() has said the schema is there: g_settings_new aborts
// the process when it is not.
int FavoriteCount(const std::string& id)
{
    GSettings* settings = g_settings_new("org.gnome.shell");
    gchar** raw = g_settings_get_strv(settings, "favorite-apps");
    int found = 0;
    for (gchar** p = raw; p && *p; ++p)
        if (id == *p) ++found;
    g_strfreev(raw);
    g_object_unref(settings);
    return found;
}

std::string ReadAll(const std::filesystem::path& p)
{
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// These pin and unpin for real. CMake hands the test process a memory
// GSettings backend and a private XDG_DATA_HOME so that "for real" is never
// the developer's own dash; run straight from a shell without them, the test
// steps aside rather than rewriting somebody's desktop.
void TestLinuxDashPinning()
{
    const char* backend = std::getenv("GSETTINGS_BACKEND");
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    if (!backend || std::string(backend) != "memory" || !dataHome)
        {
        std::printf("TestLinuxDashPinning skipped: needs GSETTINGS_BACKEND=memory and XDG_DATA_HOME (run it under ctest)\n");
        return;
        }
    if (!platform::CanPinApplications())
        {
        std::printf("TestLinuxDashPinning skipped: no writable org.gnome.shell favorite-apps here\n");
        return;
        }

    const std::filesystem::path apps = std::filesystem::path(dataHome) / "applications";
    std::filesystem::create_directories(apps);
    TempDir tmp("pin");
    std::string error;

    // A plain program has no launcher, so pinning has to write one. The name
    // has a space in it, which is where the quoting matters.
    const std::string program = tmp.file("my tool", "#!/bin/sh\n:\n");
    std::filesystem::permissions(program, std::filesystem::perms::owner_exec, std::filesystem::perm_options::add);
    CHECK(!platform::IsPinned(program));
    CHECK(platform::PinApplication(program, error));
    CHECK(error.empty());
    CHECK(platform::IsPinned(program));

    // Named for us, not for the program alone: pinning a binary called
    // "code" must not come out as the real code.desktop.
    const std::filesystem::path generated = apps / "davesplorer-my-tool.desktop";
    CHECK(std::filesystem::exists(generated));
    const std::string launcher = ReadAll(generated);
    // Quoted, or a path with a space is read as two arguments.
    CHECK(launcher.find("Exec=\"" + program + "\"") != std::string::npos);
    CHECK(launcher.find("X-Davesplorer-Generated=true") != std::string::npos);

    // Pinning what is already pinned is not an error and leaves one entry.
    CHECK(platform::PinApplication(program, error));
    CHECK_EQ(FavoriteCount("davesplorer-my-tool.desktop"), 1);

    CHECK(platform::UnpinApplication(program, error));
    CHECK(!platform::IsPinned(program));
    CHECK_EQ(FavoriteCount("davesplorer-my-tool.desktop"), 0);
    // The launcher stays behind: re-pinning is then immediate, and nothing
    // the user may have edited is deleted for them.
    CHECK(std::filesystem::exists(generated));

    // A launcher already where the desktop looks pins under its own id, and
    // nothing is copied anywhere.
    const std::filesystem::path installed = apps / "already-here.desktop";
    { std::ofstream(installed) << "[Desktop Entry]\nType=Application\nName=Here\nExec=/bin/true\n"; }
    CHECK(platform::PinApplication(installed.string(), error));
    CHECK(platform::IsPinned(installed.string()));
    CHECK_EQ(FavoriteCount("already-here.desktop"), 1);

    // A launcher from anywhere else is installed first, under a name the
    // shell can look up.
    const std::string loose = tmp.file("loose app.desktop", "[Desktop Entry]\nType=Application\nName=Loose\nExec=/bin/true\n");
    CHECK(platform::PinApplication(loose, error));
    CHECK(std::filesystem::exists(apps / "loose-app.desktop"));
    CHECK_EQ(FavoriteCount("loose-app.desktop"), 1);

    // An installed application that happens to share the name is not ours to
    // overwrite: the pin fails and says so, and the file is left alone.
    const std::string other = tmp.dir("other");
    const std::filesystem::path clash = std::filesystem::path(other) / "already-here.desktop";
    { std::ofstream(clash) << "[Desktop Entry]\nType=Application\nName=Different\nExec=/bin/false\n"; }
    CHECK(!platform::PinApplication(clash.string(), error));
    CHECK(!error.empty());
    CHECK(ReadAll(installed).find("Exec=/bin/true") != std::string::npos);
}
#endif

void TestFormat()
{
    using namespace app;
    CHECK_EQ(WithThousands(0), "0");
    CHECK_EQ(WithThousands(999), "999");
    CHECK_EQ(WithThousands(1000), "1,000");
    CHECK_EQ(WithThousands(1234567), "1,234,567");

    CHECK_EQ(SizeInKB(0), "0 KB");
    CHECK_EQ(SizeInKB(1), "1 KB");
    CHECK_EQ(SizeInKB(1024), "1 KB");
    CHECK_EQ(SizeInKB(1025), "2 KB");
    CHECK_EQ(SizeInKB(2048 * 1024), "2,048 KB");

    CHECK_EQ(HumanSize(0), "0 bytes");
    CHECK_EQ(HumanSize(1), "1 byte");
    CHECK_EQ(HumanSize(512), "512 bytes");
    CHECK_EQ(HumanSize(1536), "1.50 KB");
    CHECK_EQ(HumanSize(15 * 1024 * 1024 + 300 * 1024), "15.3 MB");
    CHECK_EQ(HumanSize(200ull * 1024 * 1024 * 1024), "200 GB");
}

void TestSettings()
{
    app::Settings s;
    s.themeIndex = 2;
    s.uiScale = 1.3f;
    s.showHidden = true;
    s.startPath = "C:\\Users";
    s.windowWidth = 1000;

    app::Settings back;
    const std::string text = app::SerializeSettings(s);
    size_t pos = 0;
    while (pos < text.size())
        {
        const size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl - pos);
        const size_t eq = line.find('=');
        if (eq != std::string::npos) app::ApplySettingLine(back, line.substr(0, eq), line.substr(eq + 1));
        pos = nl + 1;
        }
    CHECK_EQ(back.themeIndex, 2);
    CHECK(back.uiScale > 1.29f && back.uiScale < 1.31f);
    CHECK(back.showHidden);
    CHECK_EQ(back.startPath, "C:\\Users");
    CHECK_EQ(back.windowWidth, 1000);

    // Garbage in the file must not produce an unusable interface.
    app::ApplySettingLine(back, "ui_scale", "0");
    CHECK_EQ(back.uiScale, 1.0f);
    app::ApplySettingLine(back, "ui_scale", "abc");
    CHECK_EQ(back.uiScale, 1.0f);
    app::ApplySettingLine(back, "window_width", "10");
    CHECK_EQ(back.windowWidth, 400);
}

void TestNavigationAndListing()
{
    TempDir tmp("nav");
    tmp.file("alpha.txt", "aaaa");
    tmp.file("Beta.md", "bb");
    tmp.file("gamma.png", "ggggggg");
    tmp.file("file10.txt");
    tmp.file("file2.txt");
    tmp.dir("sub");
    tmp.dir("Another");

    app::Settings settings;
    app::Tab tab;
    std::string error;
    CHECK(app::NavigateTo(tab, tmp.path, error));
    CHECK_EQ(tab.path, tmp.path);
    CHECK(!app::CanGoBack(tab));
    CHECK(app::CanGoUp(tab));

    app::ReloadIfNeeded(tab, settings);
    CHECK_EQ(tab.entries.size(), (size_t)7);
    CHECK_EQ(tab.visible.size(), (size_t)7);
    // Folders first, natural order within each group.
    CHECK(tab.entries[(size_t)tab.visible[0]].isDirectory);
    CHECK(tab.entries[(size_t)tab.visible[1]].isDirectory);
    CHECK_EQ(tab.entries[(size_t)tab.visible[0]].name, "Another");
    CHECK_EQ(tab.entries[(size_t)tab.visible[1]].name, "sub");
    CHECK_EQ(tab.entries[(size_t)tab.visible[2]].name, "alpha.txt");
    CHECK_EQ(tab.entries[(size_t)tab.visible[3]].name, "Beta.md");
    CHECK_EQ(tab.entries[(size_t)tab.visible[4]].name, "file2.txt");
    CHECK_EQ(tab.entries[(size_t)tab.visible[5]].name, "file10.txt");
    CHECK_EQ(tab.entries[(size_t)tab.visible[6]].name, "gamma.png");

    // Type names come from the shell; folders are always "File folder".
    CHECK_EQ(tab.typeNames[(size_t)tab.visible[0]], "File folder");
    CHECK(!tab.typeNames[(size_t)tab.visible[2]].empty());

    // Sort by size descending: folders still first, then biggest file.
    tab.sort.column = app::SortColumn::Size;
    tab.sort.ascending = false;
    app::ApplyView(tab, settings);
    CHECK(tab.entries[(size_t)tab.visible[0]].isDirectory);
    CHECK_EQ(tab.entries[(size_t)tab.visible[2]].name, "gamma.png");

    // Filter is case-insensitive.
    tab.sort = app::SortSpec{};
    tab.filter = "BETA";
    app::ApplyView(tab, settings);
    CHECK_EQ(tab.visible.size(), (size_t)1);
    CHECK_EQ(tab.entries[(size_t)tab.visible[0]].name, "Beta.md");
    tab.filter.clear();
    app::ApplyView(tab, settings);

    // Navigate into a subfolder and back.
    const std::string sub = app::JoinPath(tmp.path, "sub");
    CHECK(app::NavigateTo(tab, sub, error));
    CHECK(app::CanGoBack(tab));
    app::ReloadIfNeeded(tab, settings);
    CHECK_EQ(tab.entries.size(), (size_t)0);
    app::GoBack(tab);
    CHECK_EQ(tab.path, tmp.path);
    CHECK(app::CanGoForward(tab));
    app::GoForward(tab);
    CHECK_EQ(tab.path, sub);
    app::GoUp(tab);
    CHECK_EQ(tab.path, tmp.path);
    // Going somewhere new discards the forward branch.
    CHECK(!app::CanGoForward(tab));

    // Bad paths fail with a message and leave the tab alone.
    CHECK(!app::NavigateTo(tab, app::JoinPath(tmp.path, "nope"), error));
    CHECK(!error.empty());
    CHECK_EQ(tab.path, tmp.path);
    CHECK(!app::NavigateTo(tab, app::JoinPath(tmp.path, "alpha.txt"), error));

    // This PC lists drives.
    CHECK(app::NavigateTo(tab, "", error));
    app::ReloadIfNeeded(tab, settings);
    CHECK(!tab.entries.empty());
    CHECK(tab.entries[0].isDirectory);
    CHECK(!app::CanGoUp(tab));

    // Hidden files are excluded unless asked for.
#ifdef _WIN32
    const std::string hidden = tmp.file("hidden.txt");
    SetFileAttributesW(platform::Utf8ToWide(hidden).c_str(), FILE_ATTRIBUTE_HIDDEN);
#else
    tmp.file(".hidden.txt");
#endif
    CHECK(app::NavigateTo(tab, tmp.path, error));
    app::ReloadIfNeeded(tab, settings);
    CHECK_EQ(tab.entries.size(), (size_t)8);
    CHECK_EQ(tab.visible.size(), (size_t)7);
    settings.showHidden = true;
    app::ApplyView(tab, settings);
    CHECK_EQ(tab.visible.size(), (size_t)8);
}

void TestSelection()
{
    TempDir tmp("sel");
    for (int i = 0; i < 6; ++i) tmp.file(("f" + std::to_string(i) + ".txt").c_str());
    app::Settings settings;
    app::Tab tab;
    std::string error;
    CHECK(app::NavigateTo(tab, tmp.path, error));
    app::ReloadIfNeeded(tab, settings);
    CHECK_EQ(tab.visible.size(), (size_t)6);

    app::ClickSelect(tab, 1, false, false);
    CHECK_EQ(app::SelectedCount(tab), 1);
    CHECK_EQ(tab.focused, tab.visible[1]);
    app::ClickSelect(tab, 3, true, false);
    CHECK_EQ(app::SelectedCount(tab), 2);
    app::ClickSelect(tab, 3, true, false);   // ctrl again toggles off, anchor stays at 3
    CHECK_EQ(app::SelectedCount(tab), 1);
    app::ClickSelect(tab, 5, false, true);   // shift from anchor 3 to 5
    CHECK_EQ(app::SelectedCount(tab), 3);
    app::ClickSelect(tab, 1, false, false);
    app::ClickSelect(tab, 4, false, true);   // shift from anchor 1 to 4
    CHECK_EQ(app::SelectedCount(tab), 4);
    app::ClickSelect(tab, 0, false, false);
    CHECK_EQ(app::SelectedCount(tab), 1);

    app::MoveFocus(tab, 1, false, false, false);
    CHECK_EQ(tab.focused, tab.visible[1]);
    CHECK_EQ(app::SelectedCount(tab), 1);
    app::MoveFocus(tab, 1, false, true, false);
    CHECK_EQ(app::SelectedCount(tab), 2);
    app::MoveFocus(tab, 1, true, false, false);
    CHECK_EQ(tab.focused, tab.visible[5]);
    app::MoveFocus(tab, 1, false, false, false);   // clamps at the end
    CHECK_EQ(tab.focused, tab.visible[5]);

    app::SelectAll(tab);
    CHECK_EQ(app::SelectedCount(tab), 6);
    CHECK_EQ(app::SelectedPaths(tab).size(), (size_t)6);
    app::InvertSelection(tab);
    CHECK_EQ(app::SelectedCount(tab), 0);
    app::SelectAll(tab);
    CHECK_EQ(app::SelectedBytes(tab), (uint64_t)6);

    // A selection survives a reload of the same folder.
    app::ClearSelection(tab);
    app::ClickSelect(tab, 2, false, false);
    app::Refresh(tab);
    app::ReloadIfNeeded(tab, settings);
    CHECK_EQ(app::SelectedCount(tab), 1);
    CHECK_EQ(tab.entries[(size_t)tab.visible[2]].name, "f2.txt");
    CHECK(tab.selected[(size_t)tab.visible[2]]);
}

void TestFileOps()
{
    TempDir tmp("ops");
    tmp.file("a.txt", "hello");
    app::AppState state;
    state.tabs.emplace_back();
    app::Tab& tab = state.active();
    std::string error;
    CHECK(app::NavigateTo(tab, tmp.path, error));
    app::ReloadIfNeeded(tab, state.settings);

    CHECK_EQ(app::UniqueNewName(tab, "New folder"), "New folder");
    CHECK(app::CreateFolderIn(state, tab, "New folder", error));
    app::ReloadIfNeeded(tab, state.settings);
    CHECK_EQ(app::UniqueNewName(tab, "New folder"), "New folder (2)");
    CHECK_EQ(app::UniqueNewName(tab, "a.txt"), "a (2).txt");
    CHECK(!app::CreateFolderIn(state, tab, "bad/name", error));
    CHECK(!error.empty());

    CHECK(app::CreateFileIn(state, tab, "b.txt", error));
    app::ReloadIfNeeded(tab, state.settings);
    CHECK(platform::PathExists(app::JoinPath(tmp.path, "b.txt")));

    int idx = -1;
    for (size_t i = 0; i < tab.entries.size(); ++i)
        if (tab.entries[i].name == "a.txt") idx = (int)i;
    CHECK(idx >= 0);
    CHECK(app::RenameEntry(state, tab, idx, "renamed.txt", error));
    app::ReloadIfNeeded(tab, state.settings);
    CHECK(platform::PathExists(app::JoinPath(tmp.path, "renamed.txt")));
    CHECK(!platform::PathExists(app::JoinPath(tmp.path, "a.txt")));

    // Copy within the same folder makes " - Copy". This goes through the
    // real Windows clipboard, whose previous contents are put back after.
    [[maybe_unused]] ClipboardKeeper keeper;
    app::ClearSelection(tab);
    for (size_t i = 0; i < tab.entries.size(); ++i)
        if (tab.entries[i].name == "renamed.txt") tab.selected[i] = 1;
    CHECK(app::CopySelection(state, tab, false, error));
    CHECK(app::CanPaste(state, tab));
    CHECK(state.clipboard.paths.size() == 1 && !state.clipboard.cut);
    CHECK(app::Paste(state, tab, error));
    app::ReloadIfNeeded(tab, state.settings);
    CHECK(platform::PathExists(app::JoinPath(tmp.path, "renamed - Copy.txt")));

    // Cut and paste into the subfolder moves the file and empties the clipboard.
    app::ClearSelection(tab);
    for (size_t i = 0; i < tab.entries.size(); ++i)
        if (tab.entries[i].name == "renamed - Copy.txt") tab.selected[i] = 1;
    CHECK(app::CopySelection(state, tab, true, error));
    CHECK(state.clipboard.cut);
    CHECK(app::NavigateTo(tab, app::JoinPath(tmp.path, "New folder"), error));
    app::ReloadIfNeeded(tab, state.settings);
    CHECK(app::Paste(state, tab, error));
    CHECK(!app::CanPaste(state, tab));
    CHECK(!platform::ClipboardHasFiles());
    app::ReloadIfNeeded(tab, state.settings);
    CHECK(platform::PathExists(app::JoinPath(tab.path, "renamed - Copy.txt")));
    CHECK(!platform::PathExists(app::JoinPath(tmp.path, "renamed - Copy.txt")));

    // A file list placed on the clipboard by another program pastes too.
    CHECK(platform::SetClipboardFiles({ app::JoinPath(tmp.path, "renamed.txt") }, false, error));
    app::TickAppState(state);
    CHECK(state.clipboard.paths.empty());   // not ours, so nothing is ghosted
    CHECK(app::CanPaste(state, tab));
    CHECK(app::Paste(state, tab, error));
    app::ReloadIfNeeded(tab, state.settings);
    CHECK(platform::PathExists(app::JoinPath(tab.path, "renamed.txt")));
    CHECK(platform::PathExists(app::JoinPath(tmp.path, "renamed.txt")));

    // Permanent delete of the selection.
    app::SelectAll(tab);
    CHECK(app::DeleteSelected(state, tab, true, error));
    app::ReloadIfNeeded(tab, state.settings);
    CHECK_EQ(tab.entries.size(), (size_t)0);

    // Nothing can be created or deleted in This PC.
    CHECK(app::NavigateTo(tab, "", error));
    CHECK(!app::CreateFolderIn(state, tab, "x", error));
    CHECK(!app::DeleteSelected(state, tab, true, error));
}

void TestClipboardAndDrop()
{
    [[maybe_unused]] ClipboardKeeper keeper;
    TempDir tmp("drop");
    const std::string a = tmp.file("a.txt", "aaa");
    const std::string b = tmp.file("b.txt", "bb");
    const std::string sub = tmp.dir("sub");
    const std::string deep = app::JoinPath(sub, "deep");
    std::string error;
    platform::CreateFolder(deep, error);

    // Clipboard round trip with the cut flag.
    std::vector<std::string> back;
    bool cut = true;
    CHECK(platform::SetClipboardFiles({ a, b }, false, error));
    CHECK(platform::ClipboardHasFiles());
    CHECK(platform::GetClipboardFiles(back, cut));
    CHECK_EQ(back.size(), (size_t)2);
    CHECK_EQ(back[0], a);
    CHECK(!cut);
    CHECK(platform::SetClipboardFiles({ b }, true, error));
    CHECK(platform::GetClipboardFiles(back, cut));
    CHECK_EQ(back.size(), (size_t)1);
    CHECK(cut);
    CHECK(platform::ClearClipboard());
    CHECK(!platform::ClipboardHasFiles());
    CHECK(!platform::GetClipboardFiles(back, cut));

    // Explorer's drop rule.
    CHECK(app::DefaultDropAction({ a }, sub, false, false) == app::DropAction::Move);
    CHECK(app::DefaultDropAction({ a }, sub, true, false) == app::DropAction::Copy);
    CHECK(app::DefaultDropAction({ a }, "Z:\\elsewhere", false, false) == app::DropAction::Copy);
    CHECK(app::DefaultDropAction({ a }, "Z:\\elsewhere", false, true) == app::DropAction::Move);

    CHECK(app::CanDropOn({ a }, sub));
    CHECK(!app::CanDropOn({ sub }, sub));
    CHECK(!app::CanDropOn({ sub }, deep));       // a folder into its own subtree
    CHECK(app::CanDropOn({ a }, tmp.path));      // same folder is allowed (copy makes " - Copy")
    CHECK(!app::CanDropOn({ a }, ""));

    app::AppState state;
    state.tabs.emplace_back();
    CHECK(app::NavigateTo(state.tabs[0], tmp.path, error));
    app::TickAppState(state);

    CHECK(app::DropPaths(state, { a }, sub, app::DropAction::Move, error));
    CHECK(platform::PathExists(app::JoinPath(sub, "a.txt")));
    CHECK(!platform::PathExists(a));
    CHECK(state.tabs[0].needsReload);

    CHECK(app::DropPaths(state, { b }, sub, app::DropAction::Copy, error));
    CHECK(platform::PathExists(app::JoinPath(sub, "b.txt")));
    CHECK(platform::PathExists(b));

    // Moving onto the folder the file is in is a quiet no-op.
    CHECK(app::DropPaths(state, { b }, tmp.path, app::DropAction::Move, error));
    CHECK(platform::PathExists(b));

    CHECK(!app::DropPaths(state, { sub }, deep, app::DropAction::Move, error));
    CHECK(!error.empty());
    CHECK(!app::DropPaths(state, { b }, "", app::DropAction::Copy, error));
}

// Every kind of entry undone against the real disk, and the refusals that
// keep undo from overwriting or throwing away what changed since. The
// Recycle Bin is the test's own (ctest points XDG_DATA_HOME at the build).
void TestUndo()
{
    TempDir tmp("undo");
    const std::string a = tmp.file("a.txt", "aaa");
    const std::string sub = tmp.dir("sub");
    app::AppState state;
    state.tabs.emplace_back();
    app::Tab& tab = state.active();
    std::string error;
    CHECK(app::NavigateTo(tab, tmp.path, error));
    app::ReloadIfNeeded(tab, state.settings);
    auto select = [&](const char* name) {
        app::ReloadIfNeeded(tab, state.settings);
        app::ClearSelection(tab);
        for (size_t i = 0; i < tab.entries.size(); ++i)
            if (tab.entries[i].name == name) tab.selected[i] = 1;
    };
    auto undoNewest = [&]() { return app::Undo(state, state.undoHistory.size() - 1, error); };

    // A new folder goes while empty, and stays once something is in it.
    CHECK(app::CreateFolderIn(state, tab, "fresh", error));
    CHECK_EQ(state.undoHistory.size(), (size_t)1);
    CHECK(state.undoHistory.back().kind == app::UndoKind::Create);
    CHECK(undoNewest());
    CHECK(!platform::PathExists(app::JoinPath(tmp.path, "fresh")));
    CHECK(state.undoHistory.empty());
    CHECK(app::CreateFolderIn(state, tab, "kept", error));
    tmp.file("kept/inside.txt");
    CHECK(!undoNewest());
    CHECK(!error.empty());
    CHECK(platform::PathExists(app::JoinPath(tmp.path, "kept/inside.txt")));
    CHECK(state.undoHistory.empty());   // tried once, then gone either way

    // Rename, and its question.
    app::ReloadIfNeeded(tab, state.settings);
    int idx = -1;
    for (size_t i = 0; i < tab.entries.size(); ++i)
        if (tab.entries[i].name == "a.txt") idx = (int)i;
    CHECK(app::RenameEntry(state, tab, idx, "b.txt", error));
    CHECK_EQ(state.undoHistory.back().label, "Rename 'a.txt' to 'b.txt'");
    CHECK_EQ(app::UndoQuestion(state, state.undoHistory.back()), "Rename 'b.txt' back to 'a.txt'?");
    CHECK(undoNewest());
    CHECK(platform::PathExists(a));
    CHECK(!platform::PathExists(app::JoinPath(tmp.path, "b.txt")));

    // A move goes back, unless something has taken its old place.
    CHECK(app::DropPaths(state, { a }, sub, app::DropAction::Move, error));
    CHECK_EQ(state.undoHistory.back().label, "Move 'a.txt' to sub");
    CHECK(undoNewest());
    CHECK(platform::PathExists(a));
    CHECK(!platform::PathExists(app::JoinPath(sub, "a.txt")));
    CHECK(app::DropPaths(state, { a }, sub, app::DropAction::Move, error));
    tmp.file("a.txt", "a newcomer");
    CHECK(!undoNewest());
    CHECK(platform::PathExists(app::JoinPath(sub, "a.txt")));
    // A move onto the folder it is in did nothing and records nothing.
    CHECK(app::DropPaths(state, { a }, tmp.path, app::DropAction::Move, error));
    CHECK(state.undoHistory.empty());

    // Copies go to the Recycle Bin, including the renamed " - Copy" one;
    // the originals stay.
    CHECK(app::DropPaths(state, { a }, tmp.path, app::DropAction::Copy, error));
    const std::string copy = app::JoinPath(tmp.path, "a - Copy.txt");
    CHECK(platform::PathExists(copy));
    CHECK_EQ(state.undoHistory.back().items.size(), (size_t)1);
    CHECK_EQ(state.undoHistory.back().items[0].to, copy);
    CHECK(undoNewest());
    CHECK(!platform::PathExists(copy));
    CHECK(platform::PathExists(a));

    // The Recycle Bin gives back what it took, contents and all; a
    // permanent delete leaves nothing to undo.
    select("a.txt");
    CHECK(app::DeleteSelected(state, tab, false, error));
    CHECK(!platform::PathExists(a));
    CHECK_EQ(state.undoHistory.back().label, "Delete 'a.txt'");
    CHECK(undoNewest());
    CHECK(platform::PathExists(a));
    {
        std::ifstream in(a);
        std::string content;
        std::getline(in, content);
        CHECK_EQ(content, "a newcomer");
    }
    CHECK(!app::Undo(state, 0, error));   // nothing left
    select("a.txt");
    CHECK(app::DeleteSelected(state, tab, true, error));
    CHECK(state.undoHistory.empty());

    // Out of order: the older of two entries can go first.
    const std::string c = tmp.file("c.txt");
    const std::string d = tmp.file("d.txt");
    CHECK(app::DropPaths(state, { c }, sub, app::DropAction::Move, error));
    CHECK(app::DropPaths(state, { d }, sub, app::DropAction::Move, error));
    CHECK(app::Undo(state, 0, error));
    CHECK(platform::PathExists(c));
    CHECK(!platform::PathExists(d));
    CHECK_EQ(state.undoHistory.size(), (size_t)1);
    CHECK_EQ(state.undoHistory[0].label, "Move 'd.txt' to sub");
    state.undoHistory.clear();

    // Only the last ten are kept.
    for (int i = 0; i < 12; ++i)
        CHECK(app::CreateFolderIn(state, tab, "n" + std::to_string(i), error));
    CHECK_EQ(state.undoHistory.size(), app::kUndoDepth);
    CHECK_EQ(state.undoHistory.front().label, "New folder 'n2'");
    CHECK_EQ(state.undoHistory.back().label, "New folder 'n11'");
}

void TestShortcuts()
{
    TempDir tmp("shortcuts");
    const std::string sub = tmp.dir("sub");
    tmp.file("a.txt");

    app::AppState state;
    app::InitAppState(state, tmp.path);
    app::Tab& tab = state.active();
    std::string error;
    CHECK(app::NavigateTo(tab, tmp.path, error));
    app::ReloadIfNeeded(tab, state.settings);

    // Nothing selected: the folder being shown. One folder selected: that
    // folder. A file selected: back to the folder being shown.
    CHECK_EQ(app::ShortcutCandidate(tab), tmp.path);
    for (size_t i = 0; i < tab.entries.size(); ++i)
        if (tab.entries[i].name == "sub") app::ClickSelect(tab, app::VisiblePosOf(tab, (int)i), false, false);
    CHECK_EQ(app::ShortcutCandidate(tab), sub);
    app::SelectAll(tab);
    CHECK_EQ(app::ShortcutCandidate(tab), tmp.path);

    CHECK(!app::ShortcutAssigned(state, 0));
    CHECK(!app::ShortcutAssigned(state, 99));
    app::AssignShortcut(state, 3, sub);
    CHECK(app::ShortcutAssigned(state, 3));
    CHECK_EQ(app::ShortcutPath(state, 3), sub);
    app::AssignShortcut(state, 99, sub);   // out of range is ignored
    app::AssignShortcut(state, 4, "");     // empty is ignored
    CHECK(!app::ShortcutAssigned(state, 4));

    // Assigning writes settings.ini right away; the slot survives a reload.
    app::Settings loaded;
    CHECK(app::LoadSettings(state.settingsFile, loaded));
    CHECK_EQ(loaded.shortcuts.size(), (size_t)app::kShortcutSlots);
    CHECK_EQ(loaded.shortcuts[3], sub);
    CHECK(loaded.shortcuts[0].empty());

    app::ClearShortcut(state, 3);
    CHECK(!app::ShortcutAssigned(state, 3));
    app::Settings reloaded;   // a load overlays the file onto defaults, so start fresh
    CHECK(app::LoadSettings(state.settingsFile, reloaded));
    CHECK(reloaded.shortcuts[3].empty());
}

// A real 2x2 RGBA PNG: red, green / blue, half-transparent white.
const unsigned char kTinyPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d,
    0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
    0x1f, 0x0c, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78, 0x28, 0xa0, 0xdb, 0x77,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };

void WriteBytes(const std::string& path, const unsigned char* bytes, size_t count)
{
    std::ofstream out(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes), (std::streamsize)count);
}

void TestImage()
{
    using namespace app;
    CHECK(IsViewableImage("png"));
    CHECK(IsViewableImage("jpg"));
    CHECK(IsViewableImage("gif"));
    CHECK(!IsViewableImage("webp"));   // stb_image has no webp; the shell gets it
    CHECK(!IsViewableImage("txt"));
    CHECK(!IsViewableImage(""));
    CHECK(IsVideo("mp4"));
    CHECK(IsVideo("mkv"));
    CHECK(!IsVideo("png"));
    CHECK(!IsVideo("ts"));             // TypeScript far more often than MPEG-TS
    CHECK(HasThumbnail("mov"));
    CHECK(HasThumbnail("jpg"));
    CHECK(!HasThumbnail("txt"));

    TempDir dir("image");
    const std::string png = JoinPath(dir.path, "tiny.png");
    WriteBytes(png, kTinyPng, sizeof(kTinyPng));

    DecodedImage img;
    std::string error;
    CHECK(DecodeImageFile(png, 0, img, error));
    CHECK_EQ(img.width, 2);
    CHECK_EQ(img.height, 2);
    CHECK_EQ(img.sourceWidth, 2);
    CHECK(img.hasAlpha);
    CHECK_EQ(img.rgba.size(), (size_t)16);
    CHECK_EQ((int)img.rgba[0], 255);    // top-left red
    CHECK_EQ((int)img.rgba[1], 0);
    CHECK_EQ((int)img.rgba[5], 255);    // top-right green
    CHECK_EQ((int)img.rgba[10], 255);   // bottom-left blue
    CHECK_EQ((int)img.rgba[15], 128);   // bottom-right alpha

    // Reduced to fit a 1-pixel limit: one box average of the four.
    DecodedImage small;
    CHECK(DecodeImageFile(png, 1, small, error));
    CHECK_EQ(small.width, 1);
    CHECK_EQ(small.height, 1);
    CHECK_EQ(small.sourceWidth, 2);
    CHECK_EQ(small.sourceHeight, 2);
    CHECK_EQ((int)small.rgba[0], 128);
    CHECK_EQ((int)small.rgba[1], 128);
    CHECK_EQ((int)small.rgba[2], 128);
    CHECK_EQ((int)small.rgba[3], 223);

    // Not an image at all.
    const std::string fake = JoinPath(dir.path, "fake.png");
    std::ofstream(std::filesystem::path(reinterpret_cast<const char8_t*>(fake.c_str()))) << "not really a png";
    DecodedImage bad;
    CHECK(!DecodeImageFile(fake, 0, bad, error));
    CHECK(!error.empty());
    CHECK(!DecodeImageFile(JoinPath(dir.path, "missing.png"), 0, bad, error));

    // Thumbnails: an image goes through the decoder; a "video" the
    // platform's thumbnailer cannot read fails cleanly.
    DecodedImage thumb;
    CHECK(DecodeThumbnail(png, 1, thumb, error));
    CHECK_EQ(thumb.width, 1);
    CHECK_EQ(thumb.sourceWidth, 2);
    const std::string fakeVideo = JoinPath(dir.path, "fake.mp4");
    std::ofstream(std::filesystem::path(reinterpret_cast<const char8_t*>(fakeVideo.c_str()))) << "not really a film";
    CHECK(!DecodeThumbnail(fakeVideo, 64, bad, error));
    CHECK(!error.empty());

    // Siblings: the folder's images in natural name order, nothing else.
    WriteBytes(JoinPath(dir.path, "img10.png"), kTinyPng, sizeof(kTinyPng));
    WriteBytes(JoinPath(dir.path, "img2.png"), kTinyPng, sizeof(kTinyPng));
    WriteBytes(JoinPath(dir.path, "a.jpg"), kTinyPng, sizeof(kTinyPng));
    std::ofstream(std::filesystem::path(reinterpret_cast<const char8_t*>(JoinPath(dir.path, "notes.txt").c_str()))) << "x";
    const std::vector<std::string> siblings = ImageSiblings(png);
    CHECK_EQ(siblings.size(), (size_t)5);
    if (siblings.size() == 5)
        {
        CHECK_EQ(PathName(siblings[0]), "a.jpg");
        CHECK_EQ(PathName(siblings[1]), "fake.png");
        CHECK_EQ(PathName(siblings[2]), "img2.png");
        CHECK_EQ(PathName(siblings[3]), "img10.png");
        CHECK_EQ(PathName(siblings[4]), "tiny.png");
        }

    // File views: one per path, ids never clash with folder views.
    AppState state;
    OpenTab(state, dir.path);
    const int first = OpenFileView(state, png);
    CHECK_EQ(first, 0);
    CHECK_EQ(OpenFileView(state, png), 0);
    CHECK_EQ(state.fileViews.size(), (size_t)1);
    CHECK(state.fileViews[0].id != state.tabs[0].id);
    OpenFileView(state, siblings[0]);
    CHECK_EQ(state.fileViews.size(), (size_t)2);
    CHECK_EQ(FindFileView(state, siblings[0]), 1);
    CloseFileView(state, 0);
    CHECK_EQ(state.fileViews.size(), (size_t)1);
    CHECK_EQ(FindFileView(state, png), -1);
    CHECK_EQ(FindFileView(state, siblings[0]), 0);

    // The preferences round-trip through settings; a thumbnail size the
    // menu does not offer is ignored.
    Settings s;
    CHECK(s.openImagesInApp);
    ApplySettingLine(s, "open_images_in_app", "0");
    CHECK(!s.openImagesInApp);
    CHECK(SerializeSettings(s).find("open_images_in_app=0") != std::string::npos);
    CHECK(s.viewMode == ViewMode::Details);
    ApplySettingLine(s, "view_mode", "1");
    CHECK(s.viewMode == ViewMode::Thumbnails);
    ApplySettingLine(s, "thumbnail_size", "7");
    CHECK_EQ(s.thumbnailSize, 96);
    ApplySettingLine(s, "thumbnail_size", "128");
    CHECK_EQ(s.thumbnailSize, 128);
    CHECK(SerializeSettings(s).find("view_mode=1") != std::string::npos);
    CHECK(SerializeSettings(s).find("thumbnail_size=128") != std::string::npos);

    // A new tab starts in the remembered mode.
    AppState remembered;
    remembered.settings.viewMode = ViewMode::Thumbnails;
    OpenTab(remembered, dir.path);
    CHECK(remembered.active().viewMode == ViewMode::Thumbnails);
}

void TestGitRemotes()
{
    // Remote URLs to web pages.
    CHECK_EQ(app::RemoteWebUrl("git@github.com:SamirICS/davesplorer-sb.git"), "https://github.com/SamirICS/davesplorer-sb");
    CHECK_EQ(app::RemoteWebUrl("https://github.com/evaderkrub/davesplorer"), "https://github.com/evaderkrub/davesplorer");
    CHECK_EQ(app::RemoteWebUrl("https://github.com/a/b.git/"), "https://github.com/a/b");
    CHECK_EQ(app::RemoteWebUrl("https://user:s3cret@gitlab.example.com/g/sub/p.git"), "https://gitlab.example.com/g/sub/p");
    CHECK_EQ(app::RemoteWebUrl("http://intranet:8080/r.git"), "http://intranet:8080/r");
    CHECK_EQ(app::RemoteWebUrl("ssh://git@gitlab.example.com:2222/group/proj.git"), "https://gitlab.example.com/group/proj");
    CHECK_EQ(app::RemoteWebUrl("git://example.org/x/y.git"), "https://example.org/x/y");
    CHECK_EQ(app::RemoteWebUrl("bitbucket.org:team/repo"), "https://bitbucket.org/team/repo");
    CHECK_EQ(app::RemoteWebUrl("git@ssh.dev.azure.com:v3/org/proj/repo"), "https://dev.azure.com/org/proj/_git/repo");
    // Local paths and anything that is not a network remote have no page.
    CHECK_EQ(app::RemoteWebUrl("/srv/git/repo.git"), "");
    CHECK_EQ(app::RemoteWebUrl("../other"), "");
    CHECK_EQ(app::RemoteWebUrl("file:///srv/git/repo.git"), "");
    CHECK_EQ(app::RemoteWebUrl("C:/repos/x.git"), "");
    CHECK_EQ(app::RemoteWebUrl("C:\\repos\\x.git"), "");
    CHECK_EQ(app::RemoteWebUrl("dir/with:colon"), "");
    // Whatever a config file says, what comes out is an https page or
    // nothing -- never another scheme handed to the browser.
    CHECK_EQ(app::RemoteWebUrl("javascript:alert(1)"), "https://javascript/alert(1)");
    CHECK_EQ(app::RemoteWebUrl("ftp://host/x"), "");
    CHECK_EQ(app::RemoteWebUrl(""), "");
    CHECK_EQ(app::StripUrlCredentials("https://u:tok@host/p"), "https://host/p");
    CHECK_EQ(app::StripUrlCredentials("https://host/p@v2"), "https://host/p@v2");
    CHECK_EQ(app::StripUrlCredentials("git@host:p"), "git@host:p");
    // The opener refuses anything but a web page, before reaching the desktop.
    std::string openError;
    CHECK(!platform::OpenUrl("file:///etc/passwd", openError));
    CHECK(!openError.empty());
    CHECK(!platform::OpenUrl("javascript:alert(1)", openError));

    // A plain repository, found from a folder deep inside it. Quoting,
    // comments, the old [remote.x] spelling and a second url line.
    TempDir tmp("git");
    const std::string repo = tmp.dir("repo");
    std::string error;
    platform::CreateFolder(app::JoinPath(repo, ".git"), error);
    {
        std::ofstream cfg(app::JoinPath(app::JoinPath(repo, ".git"), "config"));
        cfg << "[core]\n\tbare = false\n"
            << "[remote \"origin\"]\n\turl = https://bob:tok@github.com/o/r.git  # inline\n\turl = https://mirror.example/r\n"
            << "\tfetch = +refs/heads/*:refs/remotes/origin/*\n"
            << "; a comment\n[Remote \"fork\"]\n\tURL = \"git@github.com:me/r.git\"\n"
            << "[remote.local]\n\turl = /srv/r.git\n"
            << "[branch \"main\"]\n\tremote = origin\n";
    }
    const std::string deep = app::JoinPath(app::JoinPath(repo, "src"), "ui");
    std::filesystem::create_directories(deep);
    std::string root;
    std::vector<app::GitRemote> r = app::FindGitRemotes(deep, &root);
    CHECK_EQ(root, repo);
    CHECK_EQ(r.size(), (size_t)3);
    CHECK_EQ(r[0].name, "origin");
    CHECK_EQ(r[0].url, "https://github.com/o/r.git");   // the token never leaves the file
    CHECK_EQ(r[0].webUrl, "https://github.com/o/r");
    CHECK_EQ(r[1].name, "fork");
    CHECK_EQ(r[1].webUrl, "https://github.com/me/r");
    CHECK_EQ(r[2].name, "local");
    CHECK_EQ(r[2].webUrl, "");
    CHECK_EQ(app::FindGitRemotes(repo).size(), (size_t)3);

    // A linked worktree: .git is a file naming the git directory, whose
    // commondir leads back to the main repository's config.
    const std::string wtGit = app::JoinPath(app::JoinPath(app::JoinPath(repo, ".git"), "worktrees"), "feature");
    std::filesystem::create_directories(wtGit);
    std::ofstream(app::JoinPath(wtGit, "commondir")) << "../..\n";
    const std::string wt = tmp.dir("feature");
    std::ofstream(app::JoinPath(wt, ".git")) << "gitdir: " << wtGit << "\n";
    r = app::FindGitRemotes(wt);
    CHECK_EQ(r.size(), (size_t)3);

    // A submodule: .git names its own git directory, relative, with its
    // own remotes.
    const std::string modGit = app::JoinPath(app::JoinPath(app::JoinPath(repo, ".git"), "modules"), "lib");
    std::filesystem::create_directories(modGit);
    std::ofstream(app::JoinPath(modGit, "config")) << "[remote \"origin\"]\n\turl = https://gitlab.example.com/x/lib.git\n";
    const std::string lib = app::JoinPath(repo, "lib");
    platform::CreateFolder(lib, error);
    std::ofstream(app::JoinPath(lib, ".git")) << "gitdir: ../.git/modules/lib\n";
    r = app::FindGitRemotes(lib, &root);
    CHECK_EQ(root, lib);
    CHECK_EQ(r.size(), (size_t)1);
    CHECK_EQ(r[0].webUrl, "https://gitlab.example.com/x/lib");

    // Outside any repository, and This PC: nothing.
    CHECK(app::FindGitRemotes(tmp.dir("plain")).empty());
    CHECK(app::FindGitRemotes("").empty());
    // A repository with no remotes.
    const std::string bare = tmp.dir("noremotes");
    platform::CreateFolder(app::JoinPath(bare, ".git"), error);
    std::ofstream(app::JoinPath(app::JoinPath(bare, ".git"), "config")) << "[core]\n";
    CHECK(app::FindGitRemotes(bare).empty());
}

void TestPinsAndLayouts()
{
    // Pinned views and recent layouts survive the settings round trip; a
    // pinned path may hold the separator itself.
    app::Settings s;
    s.pinnedViews.push_back(app::PinnedView{ 4, "/a|b", app::ViewMode::Thumbnails });
    s.pinnedViews.push_back(app::PinnedView{ 7, "", app::ViewMode::Details });
    app::AddRecentLayout(s, "/l/one.dslayout");
    app::AddRecentLayout(s, "/l/two.dslayout");
    app::AddRecentLayout(s, "/l/one.dslayout");   // again: back on top, not twice
    CHECK_EQ(s.recentLayouts.size(), (size_t)2);
    CHECK_EQ(s.recentLayouts[0], "/l/one.dslayout");
    for (int i = 0; i < 20; ++i) app::AddRecentLayout(s, "/l/" + std::to_string(i));
    CHECK_EQ(s.recentLayouts.size(), (size_t)app::kRecentLayouts);
    CHECK_EQ(s.recentLayouts[0], "/l/19");
    app::RemoveRecentLayout(s, "/l/19");
    CHECK_EQ(s.recentLayouts[0], "/l/18");

    app::Settings back;
    const std::string text = app::SerializeSettings(s);
    size_t pos = 0;
    while (pos < text.size())
        {
        const size_t nl = text.find('\n', pos);
        const std::string line = text.substr(pos, nl - pos);
        const size_t eq = line.find('=');
        if (eq != std::string::npos) app::ApplySettingLine(back, line.substr(0, eq), line.substr(eq + 1));
        pos = nl + 1;
        }
    CHECK_EQ(back.pinnedViews.size(), (size_t)2);
    CHECK_EQ(back.pinnedViews[0].id, 4);
    CHECK_EQ(back.pinnedViews[0].path, "/a|b");
    CHECK(back.pinnedViews[0].viewMode == app::ViewMode::Thumbnails);
    CHECK_EQ(back.pinnedViews[1].id, 7);
    CHECK_EQ(back.pinnedViews[1].path, "");
    CHECK(back.recentLayouts == s.recentLayouts);
    // A malformed or id-less pin is dropped, not half-read.
    app::ApplySettingLine(back, "pinned_view_9", "nonsense");
    app::ApplySettingLine(back, "pinned_view_9", "0|0|/x");
    CHECK_EQ(back.pinnedViews.size(), (size_t)2);

    // A layout round-trips: views with their pins, and the tree.
    app::Layout layout;
    layout.views = { { "/one", app::ViewMode::Details, true }, { "/two", app::ViewMode::Thumbnails, false }, { "", app::ViewMode::Details, false } };
    layout.active = 1;
    layout.root.split = app::LayoutSplit::LeftRight;
    layout.root.ratio = 0.25f;
    layout.root.children.resize(2);
    layout.root.children[0].nav = true;
    app::LayoutNode& right = layout.root.children[1];
    right.split = app::LayoutSplit::TopBottom;
    right.children.resize(2);
    right.children[0].views = { 0, 1 };
    right.children[0].selected = 1;
    right.children[1].views = { 2 };
    app::Layout parsed;
    std::string error;
    CHECK(app::ParseLayout(app::SerializeLayout(layout), parsed, error));
    CHECK_EQ(parsed.views.size(), (size_t)3);
    CHECK_EQ(parsed.views[0].path, "/one");
    CHECK(parsed.views[0].pinned && !parsed.views[1].pinned);
    CHECK(parsed.views[1].viewMode == app::ViewMode::Thumbnails);
    CHECK_EQ(parsed.views[2].path, "");
    CHECK_EQ(parsed.active, 1);
    CHECK(parsed.root.split == app::LayoutSplit::LeftRight);
    CHECK(parsed.root.ratio > 0.24f && parsed.root.ratio < 0.26f);
    CHECK(parsed.root.children[0].nav);
    CHECK(parsed.root.children[1].split == app::LayoutSplit::TopBottom);
    CHECK(parsed.root.children[1].children[0].views == std::vector<int>({ 0, 1 }));
    CHECK_EQ(parsed.root.children[1].children[0].selected, 1);
    CHECK(parsed.root.children[1].children[1].views == std::vector<int>({ 2 }));

    // Damaged files are refused with a reason, never half-applied.
    const std::string views = "version=1\nview=0|0|/a\nview=0|0|/b\n";
    CHECK(!app::ParseLayout("view=0|0|/a\ndock=(0)\n", parsed, error));               // no version
    CHECK(!app::ParseLayout("version=2\nview=0|0|/a\n", parsed, error));              // newer
    CHECK(!app::ParseLayout("version=1\n", parsed, error));                           // no views
    CHECK(!app::ParseLayout(views + "dock=(0 5)\n", parsed, error));                  // no such view
    CHECK(!app::ParseLayout(views + "dock=(x 0.5 (0) (0))\n", parsed, error));        // placed twice
    CHECK(!app::ParseLayout(views + "dock=(x 0.5 (0)\n", parsed, error));             // unbalanced
    CHECK(!app::ParseLayout(views + "dock=(x (0) (1))\n", parsed, error));            // no ratio
    CHECK(!app::ParseLayout(views + "dock=(nav nav)\n", parsed, error));
    CHECK(!error.empty());
    std::string deep = "dock=";
    for (int i = 0; i < 40; ++i) deep += "(x 0.5 ";
    CHECK(!app::ParseLayout(views + deep + "\n", parsed, error));
    // A tree that leaves a view out is fine: it is docked with the others.
    CHECK(app::ParseLayout(views + "dock=(x 9 (0) ())\nactive=99\n", parsed, error));
    CHECK(parsed.root.ratio < 0.951f);   // clamped to something usable
    CHECK_EQ(parsed.active, 1);

    TempDir tmp("pins");
    const std::string file = app::JoinPath(tmp.path, "work.dslayout");
    CHECK(app::SaveLayoutFile(file, layout, error));
    CHECK(app::LoadLayoutFile(file, parsed, error));
    CHECK_EQ(parsed.views.size(), (size_t)3);
    CHECK(!app::LoadLayoutFile(app::JoinPath(tmp.path, "missing.dslayout"), parsed, error));

    // Startup: pinned views reopen under the ids they had, the start folder
    // is not opened twice when a pin already shows it, and a pin whose
    // folder is gone is kept for next time rather than forgotten.
    const std::string a = tmp.dir("a");
    const std::string b = tmp.dir("b");
    {
        std::ofstream ini(app::JoinPath(tmp.path, "settings.ini"));
        ini << "start_path=" << b << "\n"
            << "pinned_view_0=5|1|" << a << "\n"
            << "pinned_view_1=3|0|" << b << "\n"
            << "pinned_view_2=9|0|" << app::JoinPath(tmp.path, "gone") << "\n"
            << "pinned_view_3=5|0|" << b << "\n";   // a duplicate id is ignored
    }
    app::AppState state;
    app::InitAppState(state, tmp.path);
    CHECK_EQ(state.tabs.size(), (size_t)2);
    CHECK_EQ(state.tabs[0].id, 5);
    CHECK_EQ(state.tabs[0].path, a);
    CHECK(state.tabs[0].pinned);
    CHECK(state.tabs[0].viewMode == app::ViewMode::Thumbnails);
    CHECK_EQ(state.tabs[1].id, 3);
    CHECK_EQ(state.activeTab, 1);                 // the start folder, already pinned
    CHECK_EQ(state.unavailablePins.size(), (size_t)1);
    const int fresh = app::OpenTab(state, a);
    CHECK(state.tabs[(size_t)fresh].id > 9);      // no new view takes a pinned id
    CHECK(!state.tabs[(size_t)fresh].pinned);

    // A command-line folder replaces only the unpinned views.
    CHECK(app::ApplyStartArgument(state, tmp.path));
    CHECK_EQ(state.tabs.size(), (size_t)3);
    CHECK_EQ(state.active().path, tmp.path);
    CHECK(state.tabs[0].pinned && state.tabs[1].pinned);

    // Unpinning is written at once; the missing folder's pin stays.
    app::SetPinned(state, 0, false);
    app::SetPinned(state, 2, true);
    app::Settings onDisk;
    CHECK(app::LoadSettings(state.settingsFile, onDisk));
    CHECK_EQ(onDisk.pinnedViews.size(), (size_t)3);
    CHECK_EQ(onDisk.pinnedViews[0].id, 3);
    CHECK_EQ(onDisk.pinnedViews[1].path, tmp.path);
    CHECK_EQ(onDisk.pinnedViews[2].id, 9);

    // With no pin on it, the start folder gets a view of its own, after
    // the pinned ones.
    state.activeTab = 0;   // a, unpinned above
    app::SaveAppState(state);
    app::AppState again;
    app::InitAppState(again, tmp.path);
    CHECK_EQ(again.tabs.size(), (size_t)3);
    CHECK(again.tabs[0].pinned && again.tabs[1].pinned);
    CHECK_EQ(again.tabs[1].path, tmp.path);
    CHECK_EQ(again.tabs[2].path, a);
    CHECK(!again.tabs[2].pinned);
    CHECK_EQ(again.activeTab, 2);
    CHECK_EQ(again.layoutsDir, app::JoinPath(tmp.path, "layouts"));
}

void TestAppState()
{
    TempDir tmp("state");
    app::AppState state;
    app::InitAppState(state, tmp.path);
    CHECK_EQ(state.tabs.size(), (size_t)1);
    CHECK(!state.drives.empty());
    CHECK(!state.quickAccess.empty());
    CHECK_EQ(state.settingsFile, app::JoinPath(tmp.path, "settings.ini"));

    const int second = app::OpenTab(state, "");
    CHECK_EQ(second, 1);
    CHECK_EQ(state.activeTab, 1);
    CHECK(state.tabs[0].id != state.tabs[1].id);
    app::CloseTab(state, 0);
    CHECK_EQ(state.tabs.size(), (size_t)1);
    CHECK_EQ(state.activeTab, 0);
    CHECK(!state.quitRequested);
    app::CloseTab(state, 0);
    CHECK(state.quitRequested);   // last tab closing means exit, like Explorer

    state.settings.startPath = "";
    app::SaveAppState(state);
    CHECK(platform::PathExists(state.settingsFile));
    app::Settings loaded;
    CHECK(app::LoadSettings(state.settingsFile, loaded));
    CHECK_EQ(loaded.startPath, state.active().path);

    CHECK_EQ(app::LocationTitle(state, ""), "This PC");
    CHECK_EQ(app::LocationTitle(state, state.drives[0].root), state.drives[0].displayName);

    // Command-line argument: a folder opens there, a file opens its folder
    // with the file selected, and a quoted path is unwrapped. Anything
    // that is not a path lands on This PC.
    const std::string sub = tmp.dir("arg");
    const std::string file = tmp.file("arg.txt", "x");
    CHECK(app::ApplyStartArgument(state, sub));
    CHECK_EQ(state.tabs.size(), (size_t)1);
    CHECK_EQ(state.active().path, sub);
    CHECK(app::ApplyStartArgument(state, "\"" + sub + "/\""));
    CHECK_EQ(state.active().path, sub);
    CHECK(app::ApplyStartArgument(state, file));
    CHECK_EQ(state.active().path, tmp.path);
    app::TickAppState(state);
    CHECK_EQ(app::SelectedCount(state.active()), 1);
    CHECK_EQ(app::SelectedPaths(state.active())[0], file);
    CHECK_EQ(state.active().entries[(size_t)state.active().focused].name, "arg.txt");
    // Explorer's own switches are accepted, so davesplorer.exe can be run
    // with the arguments a program would give explorer.exe.
    CHECK(app::ApplyStartArgument(state, "/select,\"" + file + "\""));
    CHECK_EQ(state.active().path, tmp.path);
    CHECK_EQ(state.active().selectOnLoad, "arg.txt");
    CHECK(app::ApplyStartArgument(state, "/SELECT," + sub));   // a folder: shown selected in its parent
    CHECK_EQ(state.active().path, tmp.path);
    CHECK_EQ(state.active().selectOnLoad, "arg");
    CHECK(app::ApplyStartArgument(state, "/e," + sub));
    CHECK_EQ(state.active().path, sub);
    CHECK(!app::ApplyStartArgument(state, "::{20D04FE0-3AEA-1069-A2D8-08002B30309D}"));
    CHECK_EQ(state.active().path, "");
    CHECK(!app::ApplyStartArgument(state, app::JoinPath(tmp.path, "missing")));
    CHECK_EQ(state.active().path, "");
}

// Subfolders opened in place: the walk runs on a worker thread, so the
// tab is ticked until it settles, exactly as a frame would.
void PumpUntilQuiet(app::AppState& state)
{
    for (int i = 0; i < 4000; ++i)
        {
        app::TickAppState(state);
        bool busy = false;
        for (const app::Tab& t : state.tabs) busy = busy || t.scanning;
        if (!busy) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    CHECK(false);   // the walk never finished
}

void TestExpandedTree()
{
    TempDir tmp("tree");
    tmp.file("alpha.txt", "aaa");
    const std::string sub = tmp.dir("sub");
    std::string error;
    platform::CreateFolder(app::JoinPath(sub, "deeper"), error);
    std::ofstream(std::filesystem::path(reinterpret_cast<const char8_t*>(app::JoinPath(sub, "inner.txt").c_str()))) << "i";
    std::ofstream(std::filesystem::path(reinterpret_cast<const char8_t*>(
        app::JoinPath(app::JoinPath(sub, "deeper"), "leaf.txt").c_str()))) << "l";

    app::AppState state;
    app::InitAppState(state, tmp.path);
    state.tabs.clear();
    app::OpenTab(state, tmp.path);
    app::Tab& tab = state.active();
    app::TickAppState(state);
    CHECK_EQ(tab.entries.size(), (size_t)2);
    CHECK(!tab.expandAll);

    app::SetExpandAll(tab, state.settings, true);
    PumpUntilQuiet(state);
    CHECK(tab.expandAll);
    CHECK(!tab.scanTruncated);
    CHECK_EQ(tab.entries.size(), (size_t)5);   // alpha.txt, sub, inner.txt, deeper, leaf.txt
    CHECK_EQ(tab.visible.size(), (size_t)5);

    auto pos = [&](const char* name) {
        for (size_t p = 0; p < tab.visible.size(); ++p)
            if (tab.entries[(size_t)tab.visible[p]].name == name) return (int)p;
        return -1;
    };
    auto entry = [&](const char* name) {
        const int p = pos(name);
        return p < 0 ? -1 : tab.visible[(size_t)p];
    };
    // Depth first, siblings sorted among themselves: everything under sub
    // comes before the root's own file, and leaf.txt sits under deeper.
    CHECK_EQ(pos("sub"), 0);
    CHECK_EQ(pos("deeper"), 1);
    CHECK_EQ(pos("leaf.txt"), 2);
    CHECK_EQ(pos("inner.txt"), 3);
    CHECK_EQ(pos("alpha.txt"), 4);
    CHECK_EQ(tab.nodes[(size_t)entry("deeper")].depth, 1);
    CHECK_EQ(tab.nodes[(size_t)entry("leaf.txt")].depth, 2);
    CHECK_EQ(tab.nodes[(size_t)entry("leaf.txt")].parent, entry("deeper"));

    // A search reaches into the tree and keeps the folders that lead to a
    // match, so the answer can be seen where it lives.
    tab.filter = "leaf";
    app::ApplyView(tab, state.settings);
    CHECK_EQ(tab.visible.size(), (size_t)3);
    CHECK_EQ(tab.entries[(size_t)tab.visible[0]].name, "sub");
    CHECK_EQ(tab.entries[(size_t)tab.visible[2]].name, "leaf.txt");
    tab.filter.clear();
    app::ApplyView(tab, state.settings);

    // Closing one folder takes its whole subtree out of view and drops the
    // selection inside it; re-opening is instant, nothing is re-read.
    const int subIndex = entry("sub");
    tab.selected[(size_t)entry("leaf.txt")] = 1;
    tab.focused = entry("leaf.txt");
    app::CollapseEntry(tab, state.settings, subIndex);
    CHECK_EQ(tab.visible.size(), (size_t)2);
    CHECK_EQ(app::SelectedCount(tab), 0);
    CHECK_EQ(tab.focused, subIndex);
    CHECK_EQ(tab.entries.size(), (size_t)5);   // kept, just not shown
    app::ExpandEntry(tab, state.settings, subIndex);
    CHECK_EQ(tab.visible.size(), (size_t)5);
    CHECK_EQ(tab.entries.size(), (size_t)5);

    // A name that only exists in a subfolder says nothing about what this
    // folder can hold.
    CHECK_EQ(app::UniqueNewName(tab, "leaf.txt"), "leaf.txt");
    CHECK_EQ(app::UniqueNewName(tab, "alpha.txt"), "alpha (2).txt");

    // Off again: exactly the folder's own listing.
    app::SetExpandAll(tab, state.settings, false);
    CHECK(!tab.expandAll);
    CHECK_EQ(tab.entries.size(), (size_t)2);
    CHECK_EQ(tab.visible.size(), (size_t)2);
    CHECK_EQ(tab.nodes.size(), (size_t)2);

    // Expansion belongs to the folder it was asked for: navigating away
    // never starts a walk of wherever the user landed.
    app::SetExpandAll(tab, state.settings, true);
    PumpUntilQuiet(state);
    CHECK_EQ(tab.entries.size(), (size_t)5);
    CHECK(app::NavigateTo(tab, sub, error));
    app::TickAppState(state);
    CHECK(!tab.expandAll);
    CHECK_EQ(tab.entries.size(), (size_t)2);   // inner.txt and deeper, nothing below
}

} // namespace

int main()
{
#ifndef _WIN32
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) return 1;
#endif
    TestPathUtil();
    TestFormat();
#ifndef _WIN32
    TestUnixFileSystem();
#endif
#ifdef __linux__
    TestLinuxExecutables();
    TestProgramScan();
    TestLinuxDashPinning();
#endif
    TestSettings();
    TestNavigationAndListing();
    TestSelection();
    TestFileOps();
    TestClipboardAndDrop();
    TestUndo();
    TestShortcuts();
    TestImage();
    TestAppState();
    TestPinsAndLayouts();
    TestGitRemotes();
    TestExpandedTree();
    app::treescan::Shutdown();   // joins the folder-walking thread
    app::programscan::Shutdown();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
