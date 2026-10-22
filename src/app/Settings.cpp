#include "app/Settings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace app
{

namespace
{

bool ParseBool(const std::string& v) { return v == "1" || v == "true" || v == "yes"; }

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

} // namespace

void ApplySettingLine(Settings& s, const std::string& key, const std::string& value)
{
    if (key == "theme")               s.themeIndex = std::atoi(value.c_str());
    else if (key == "ui_scale")
        {
        const float f = std::strtof(value.c_str(), nullptr);
        // Guard the file against a stray zero or garbage: a zero scale would
        // make every widget vanish and the user could not reach the menu.
        s.uiScale = (f >= 0.5f && f <= 3.0f) ? f : 1.0f;
        }
    else if (key == "show_hidden")        s.showHidden = ParseBool(value);
    else if (key == "show_extensions")    s.showExtensions = ParseBool(value);
    else if (key == "show_nav_pane")      s.showNavPane = ParseBool(value);
    else if (key == "show_shortcut_bar")  s.showShortcutBar = ParseBool(value);
    else if (key == "show_status_bar")    s.showStatusBar = ParseBool(value);
    else if (key == "show_details_pane")  s.showDetailsPane = ParseBool(value);
    else if (key == "open_images_in_app") s.openImagesInApp = ParseBool(value);
    else if (key == "view_mode")          s.viewMode = std::atoi(value.c_str()) == 1 ? ViewMode::Thumbnails : ViewMode::Details;
    else if (key == "thumbnail_size")
        {
        // Only the sizes the menu offers, so the file cannot ask for a
        // one-pixel or a ten-thousand-pixel grid.
        const int n = std::atoi(value.c_str());
        for (int allowed : kThumbnailSizes)
            if (n == allowed) s.thumbnailSize = n;
        }
    else if (key == "start_path")         s.startPath = value;
    else if (key == "remember_last_path") s.rememberLastPath = ParseBool(value);
    else if (key == "programs_root")      s.programsRoot = value;
    else if (key == "window_width")       s.windowWidth = std::max(400, std::atoi(value.c_str()));
    else if (key == "window_height")      s.windowHeight = std::max(300, std::atoi(value.c_str()));
    else if (key == "window_maximized")   s.windowMaximized = ParseBool(value);
    else if (key.rfind("pinned_view_", 0) == 0)
        {
        // id|view mode|path, the path last so a '|' in it survives.
        const size_t a = value.find('|');
        const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
        if (b == std::string::npos) return;
        PinnedView p;
        p.id = std::atoi(value.substr(0, a).c_str());
        p.viewMode = std::atoi(value.substr(a + 1, b - a - 1).c_str()) == 1 ? ViewMode::Thumbnails : ViewMode::Details;
        p.path = value.substr(b + 1);
        if (p.id > 0) s.pinnedViews.push_back(std::move(p));
        }
    else if (key.rfind("recent_layout_", 0) == 0)
        {
        if (!value.empty() && s.recentLayouts.size() < (size_t)kRecentLayouts) s.recentLayouts.push_back(value);
        }
    else if (key.rfind("shortcut_", 0) == 0)
        {
        const int slot = std::atoi(key.c_str() + 9);
        if (slot >= 0 && slot < kShortcutSlots)
            {
            if (s.shortcuts.size() != (size_t)kShortcutSlots) s.shortcuts.assign((size_t)kShortcutSlots, "");
            s.shortcuts[(size_t)slot] = value;
            }
        }
}

std::string SerializeSettings(const Settings& s)
{
    std::ostringstream out;
    out << "theme=" << s.themeIndex << '\n';
    out << "ui_scale=" << s.uiScale << '\n';
    out << "show_hidden=" << (s.showHidden ? 1 : 0) << '\n';
    out << "show_extensions=" << (s.showExtensions ? 1 : 0) << '\n';
    out << "show_nav_pane=" << (s.showNavPane ? 1 : 0) << '\n';
    out << "show_shortcut_bar=" << (s.showShortcutBar ? 1 : 0) << '\n';
    out << "show_status_bar=" << (s.showStatusBar ? 1 : 0) << '\n';
    out << "show_details_pane=" << (s.showDetailsPane ? 1 : 0) << '\n';
    out << "open_images_in_app=" << (s.openImagesInApp ? 1 : 0) << '\n';
    out << "view_mode=" << (int)s.viewMode << '\n';
    out << "thumbnail_size=" << s.thumbnailSize << '\n';
    out << "start_path=" << s.startPath << '\n';
    out << "remember_last_path=" << (s.rememberLastPath ? 1 : 0) << '\n';
    if (!s.programsRoot.empty()) out << "programs_root=" << s.programsRoot << '\n';
    out << "window_width=" << s.windowWidth << '\n';
    out << "window_height=" << s.windowHeight << '\n';
    out << "window_maximized=" << (s.windowMaximized ? 1 : 0) << '\n';
    for (size_t i = 0; i < s.shortcuts.size() && i < (size_t)kShortcutSlots; ++i)
        if (!s.shortcuts[i].empty()) out << "shortcut_" << i << '=' << s.shortcuts[i] << '\n';
    for (size_t i = 0; i < s.pinnedViews.size(); ++i)
        out << "pinned_view_" << i << '=' << s.pinnedViews[i].id << '|' << (int)s.pinnedViews[i].viewMode << '|' << s.pinnedViews[i].path << '\n';
    for (size_t i = 0; i < s.recentLayouts.size() && i < (size_t)kRecentLayouts; ++i)
        out << "recent_layout_" << i << '=' << s.recentLayouts[i] << '\n';
    return out.str();
}

void AddRecentLayout(Settings& s, const std::string& path)
{
    RemoveRecentLayout(s, path);
    s.recentLayouts.insert(s.recentLayouts.begin(), path);
    if (s.recentLayouts.size() > (size_t)kRecentLayouts) s.recentLayouts.resize((size_t)kRecentLayouts);
}

void RemoveRecentLayout(Settings& s, const std::string& path)
{
    s.recentLayouts.erase(std::remove(s.recentLayouts.begin(), s.recentLayouts.end(), path), s.recentLayouts.end());
}

bool LoadSettings(const std::string& file, Settings& out)
{
    std::ifstream in(file);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line))
        {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        ApplySettingLine(out, Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
        }
    return true;
}

bool SaveSettings(const std::string& file, const Settings& settings, std::string& error)
{
    std::ofstream out(file, std::ios::trunc);
    if (!out)
        {
        error = "Could not write " + file;
        return false;
        }
    out << SerializeSettings(settings);
    return true;
}

} // namespace app
