#include "platform/Shell.h"
#include <gio/gio.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
namespace platform
{
namespace
{
bool Finish(bool ok, GError* e, std::string& error)
{
    error = e ? e->message : "";
    if (e) g_error_free(e);
    return ok;
}
}
bool OpenWithShell(const std::string& path, std::string& error)
{
    GFile* file = g_file_new_for_path(path.c_str());
    char* uri = g_file_get_uri(file);
    GError* e = nullptr;
    const bool ok = g_app_info_launch_default_for_uri(uri, nullptr, &e);
    g_free(uri); g_object_unref(file);
    return Finish(ok, e, error);
}
bool OpenUrl(const std::string& url, std::string& error)
{
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
        {
        error = "Only web addresses can be opened: " + url;
        return false;
        }
    GError* e = nullptr;
    const bool ok = g_app_info_launch_default_for_uri(url.c_str(), nullptr, &e);
    return Finish(ok, e, error);
}
bool OpenFolderInExplorer(const std::string& folder, std::string& error)
{ return OpenWithShell(folder.empty() ? "/" : folder, error); }
bool ShowInExplorer(const std::string& path, std::string& error)
{
    GError* e = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
    if (bus)
        {
        GFile* file = g_file_new_for_path(path.c_str());
        char* uri = g_file_get_uri(file);
        const char* uris[] = {uri, nullptr};
        GVariant* result = g_dbus_connection_call_sync(bus, "org.freedesktop.FileManager1",
            "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems",
            g_variant_new("(^ass)", uris, ""), nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &e);
        g_free(uri); g_object_unref(file); g_object_unref(bus);
        if (result) { g_variant_unref(result); return true; }
        }
    if (e) g_error_free(e);
    return OpenWithShell(std::filesystem::path(path).parent_path().string(), error);
}
bool OpenTerminalAt(const std::string& dir, std::string& error)
{
    // Launch argv directly: filenames must never be interpreted as shell code.
    for (const char* program : {"xdg-terminal-exec", "x-terminal-emulator", "gnome-terminal", "konsole", "xterm"})
        {
        char* executable = g_find_program_in_path(program);
        if (!executable) continue;
        char* args[] = {executable, nullptr};
        GError* e = nullptr;
        const bool ok = g_spawn_async(dir.c_str(), args, nullptr, G_SPAWN_DEFAULT, nullptr, nullptr, nullptr, &e);
        g_free(executable);
        return Finish(ok, e, error);
        }
    error = "No supported terminal emulator was found.";
    return false;
}
bool RunExecutable(const std::string& path, const std::string& workingDir, std::string& error)
{
    const std::string dir = workingDir.empty() ? std::filesystem::path(path).parent_path().string() : workingDir;
    // argv, not a command line: the program's own name is all the child
    // gets, so nothing in it can be read back as shell code.
    std::string program = path;
    char* args[] = {program.data(), nullptr};
    GError* e = nullptr;
    const bool ok = g_spawn_async(dir.empty() ? nullptr : dir.c_str(), args, nullptr, G_SPAWN_DEFAULT,
                                  nullptr, nullptr, nullptr, &e);
    return Finish(ok, e, error);
}

// ---------------------------------------------------------------------------
// The GNOME dash.
//
// The dash is the GSettings key org.gnome.shell favorite-apps: a list of
// desktop ids, not of paths. An id only means anything if the .desktop file
// it names sits in one of the desktop's applications directories, so
// anything outside one has to be put there before it can be pinned.

namespace
{

const char* const kSchema = "org.gnome.shell";
const char* const kKey = "favorite-apps";
// Our own mark on a launcher we wrote, so a later pin knows it may replace it.
const char* const kGenerated = "X-Davesplorer-Generated";
const char* const kNoDash = "This desktop has no dash Davesplorer can change; pinning needs GNOME.";
const char* const kRefused = "The desktop refused to change the list of pinned applications.";

// g_settings_new() aborts the process when the schema is missing -- which on
// a KDE or Xfce box, or a bare build machine, it is. Looking the schema up
// first is the only way to ask the question and live. Returns null unless the
// key is really there and really writable, which is also what makes
// CanPinApplications() honest away from GNOME.
GSettings* OpenFavorites()
{
    GSettingsSchemaSource* source = g_settings_schema_source_get_default();
    if (!source) return nullptr;
    GSettingsSchema* schema = g_settings_schema_source_lookup(source, kSchema, TRUE);
    if (!schema) return nullptr;
    GSettings* settings = g_settings_schema_has_key(schema, kKey) ? g_settings_new_full(schema, nullptr, nullptr) : nullptr;
    g_settings_schema_unref(schema);
    if (settings && !g_settings_is_writable(settings, kKey))
        {
        g_object_unref(settings);
        return nullptr;
        }
    return settings;
}

// Symlinks resolved where they can be, so that "is this file under that
// directory" survives a ~/.local that is itself a link.
std::filesystem::path Normalized(const std::filesystem::path& p)
{
    std::error_code ec;
    const std::filesystem::path c = std::filesystem::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

// Every directory the desktop looks in for launchers, the user's own first.
std::vector<std::filesystem::path> ApplicationDirs()
{
    std::vector<std::filesystem::path> dirs;
    dirs.push_back(std::filesystem::path(g_get_user_data_dir()) / "applications");
    for (const char* const* d = g_get_system_data_dirs(); d && *d; ++d)
        dirs.push_back(std::filesystem::path(*d) / "applications");
    return dirs;
}

// "foo.desktop" or "kde4/foo.desktop" when the file is under dir, else empty.
std::string RelativeUnder(const std::filesystem::path& file, const std::filesystem::path& dir)
{
    const std::filesystem::path rel = Normalized(file).lexically_relative(Normalized(dir));
    if (rel.empty() || rel == "." || *rel.begin() == "..") return {};
    return rel.generic_string();
}

// The directory it is already in, or nothing if it is not in one at all.
bool InApplicationDir(const std::string& path, std::string& id)
{
    for (const std::filesystem::path& dir : ApplicationDirs())
        {
        const std::string rel = RelativeUnder(std::filesystem::path(path), dir);
        if (rel.empty()) continue;
        // Subdirectories fold into the id with a dash: the desktop-entry
        // spec's rule, and how kde4/foo.desktop becomes kde4-foo.desktop.
        id = rel;
        std::replace(id.begin(), id.end(), '/', '-');
        return true;
        }
    return false;
}

bool IsLauncherFile(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = (char)g_ascii_tolower((guchar)c);
    return ext == ".desktop";
}

// A desktop id is a filename the shell has to be able to look up, so keep it
// to the characters that cannot be misread.
std::string Sanitize(const std::string& name)
{
    std::string out = name;
    for (char& c : out)
        if (!g_ascii_isalnum((guchar)c) && c != '_' && c != '.' && c != '-') c = '-';
    return out;
}

// What the dash would call this item. A launcher already in a search path
// keeps its own id. Anything else gets an id of ours, prefixed: pinning
// ~/bin/code must not come out as the real code.desktop.
std::string DesktopIdFor(const std::string& path)
{
    const std::string name = std::filesystem::path(path).filename().string();
    if (IsLauncherFile(path))
        {
        std::string id;
        if (InApplicationDir(path, id)) return id;
        return Sanitize(name);
        }
    return "davesplorer-" + Sanitize(name) + ".desktop";
}

bool SameContents(const std::filesystem::path& a, const std::filesystem::path& b)
{
    std::error_code ec;
    const auto sizeA = std::filesystem::file_size(a, ec);
    if (ec) return false;
    const auto sizeB = std::filesystem::file_size(b, ec);
    if (ec || sizeA != sizeB) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    if (!fa || !fb) return false;
    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(fb));
}

// Whether we may write dest. A launcher of ours may be replaced, and so may
// one that already says exactly what we were going to say; a real
// application's entry that happens to share the name is not ours to destroy.
bool MayWrite(const std::filesystem::path& dest, const std::string& source, std::string& error)
{
    std::error_code ec;
    if (!std::filesystem::exists(dest, ec)) return true;

    GKeyFile* existing = g_key_file_new();
    const bool loaded = g_key_file_load_from_file(existing, dest.c_str(), G_KEY_FILE_NONE, nullptr);
    const bool ours = loaded && g_key_file_get_boolean(existing, G_KEY_FILE_DESKTOP_GROUP, kGenerated, nullptr);
    g_key_file_free(existing);
    if (ours) return true;

    if (std::filesystem::equivalent(dest, source, ec) && !ec) return true;
    if (SameContents(dest, source)) return true;

    error = "An application named " + dest.filename().string() + " is already installed, so pinning "
            + std::filesystem::path(source).filename().string() + " would overwrite it.";
    return false;
}

// The desktop-entry spec's quoting for Exec, which is not the escaping
// GKeyFile does for values: the whole argument in quotes, with the reserved
// characters escaped inside them. Without it a path with a space in it is
// read as two arguments.
std::string QuoteExec(const std::string& path)
{
    std::string out = "\"";
    for (char c : path)
        {
        if (c == '"' || c == '\\' || c == '$' || c == '`') out += '\\';
        out += c;
        }
    return out + '"';
}

bool WriteGeneratedLauncher(const std::string& path, const std::filesystem::path& dest, std::string& error)
{
    const std::filesystem::path file(path);
    const char* group = G_KEY_FILE_DESKTOP_GROUP;
    GKeyFile* key = g_key_file_new();
    g_key_file_set_string(key, group, G_KEY_FILE_DESKTOP_KEY_TYPE, "Application");
    g_key_file_set_string(key, group, G_KEY_FILE_DESKTOP_KEY_VERSION, "1.0");
    g_key_file_set_string(key, group, G_KEY_FILE_DESKTOP_KEY_NAME, file.filename().string().c_str());
    g_key_file_set_string(key, group, G_KEY_FILE_DESKTOP_KEY_EXEC, QuoteExec(path).c_str());
    // Started where it lives, the same bargain RunExecutable strikes.
    g_key_file_set_string(key, group, G_KEY_FILE_DESKTOP_KEY_PATH, file.parent_path().string().c_str());
    g_key_file_set_string(key, group, G_KEY_FILE_DESKTOP_KEY_ICON, "application-x-executable");
    g_key_file_set_boolean(key, group, G_KEY_FILE_DESKTOP_KEY_TERMINAL, FALSE);
    g_key_file_set_boolean(key, group, kGenerated, TRUE);
    GError* e = nullptr;
    const bool ok = g_key_file_save_to_file(key, dest.c_str(), &e);
    g_key_file_free(key);
    if (!ok) return Finish(false, e, error);
    return true;
}

// Puts a launcher the dash can find at `id` in place: nothing to do when the
// item is already a launcher in a search path, a copy when it is a launcher
// somewhere else, and a generated entry when it is a plain program.
bool EnsureLauncher(const std::string& path, const std::string& id, std::string& error)
{
    std::string existing;
    if (IsLauncherFile(path) && InApplicationDir(path, existing)) return true;

    const std::filesystem::path dest = std::filesystem::path(g_get_user_data_dir()) / "applications" / id;
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
    if (ec)
        {
        error = "Could not create " + dest.parent_path().string() + ": " + ec.message();
        return false;
        }
    if (!MayWrite(dest, path, error)) return false;

    if (IsLauncherFile(path))
        {
        std::filesystem::copy_file(path, dest, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec)
            {
            error = "Could not install " + id + ": " + ec.message();
            return false;
            }
        return true;
        }
    return WriteGeneratedLauncher(path, dest, error);
}

std::vector<std::string> ReadFavorites(GSettings* settings)
{
    std::vector<std::string> ids;
    gchar** raw = g_settings_get_strv(settings, kKey);
    for (gchar** p = raw; p && *p; ++p) ids.push_back(*p);
    g_strfreev(raw);
    return ids;
}

bool WriteFavorites(GSettings* settings, const std::vector<std::string>& ids)
{
    std::vector<const gchar*> raw;
    raw.reserve(ids.size() + 1);
    for (const std::string& id : ids) raw.push_back(id.c_str());
    raw.push_back(nullptr);
    const bool ok = g_settings_set_strv(settings, kKey, raw.data());
    // The shell is another process: the write has to have left ours.
    g_settings_sync();
    return ok;
}

} // namespace

bool CanPinApplications()
{
    GSettings* settings = OpenFavorites();
    if (!settings) return false;
    g_object_unref(settings);
    return true;
}

bool IsPinned(const std::string& path)
{
    GSettings* settings = OpenFavorites();
    if (!settings) return false;
    const std::vector<std::string> ids = ReadFavorites(settings);
    g_object_unref(settings);
    return std::find(ids.begin(), ids.end(), DesktopIdFor(path)) != ids.end();
}

bool PinApplication(const std::string& path, std::string& error)
{
    GSettings* settings = OpenFavorites();
    if (!settings)
        {
        error = kNoDash;
        return false;
        }
    const std::string id = DesktopIdFor(path);
    if (!EnsureLauncher(path, id, error))
        {
        g_object_unref(settings);
        return false;
        }
    std::vector<std::string> ids = ReadFavorites(settings);
    // Pinning what is already pinned is not an error, and must not put it in
    // the dash twice.
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    const bool ok = WriteFavorites(settings, ids);
    g_object_unref(settings);
    if (!ok) error = kRefused;
    return ok;
}

bool UnpinApplication(const std::string& path, std::string& error)
{
    GSettings* settings = OpenFavorites();
    if (!settings)
        {
        error = kNoDash;
        return false;
        }
    const std::string id = DesktopIdFor(path);
    std::vector<std::string> ids = ReadFavorites(settings);
    ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
    const bool ok = WriteFavorites(settings, ids);
    g_object_unref(settings);
    if (!ok) error = kRefused;
    return ok;
}

// No thumbnailer is wired up here yet; the grid shows the file's icon.
bool ShellThumbnail(const std::string&, int, int&, int&, std::vector<uint8_t>&, std::string& error)
{
    error = "Thumbnails of this kind of file are not available on this platform.";
    return false;
}
}
