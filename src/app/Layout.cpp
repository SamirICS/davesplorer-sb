#include "app/Layout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace app
{

// The file is key=value lines, like settings.ini:
//
//   version=1
//   active=0
//   view=<pinned>|<view mode>|<path>       one per view, in order
//   dock=(x 0.220 (nav) (y 0.500 (*0 1) (2)))
//
// The dock line is the tree as nested lists: "(x r A B)" splits left/right
// with A taking r of the width, "(y r A B)" splits top/bottom, and any other
// list is a leaf of tabs -- "nav" for the navigation pane, a number for a
// view, the one marked * in front.

namespace
{

constexpr int kMaxDepth = 32;
constexpr size_t kMaxViews = 256;

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void WriteNode(std::ostringstream& out, const LayoutNode& node)
{
    out << '(';
    if (node.split != LayoutSplit::None && node.children.size() == 2)
        {
        char ratio[16];
        std::snprintf(ratio, sizeof ratio, "%.3f", node.ratio);
        out << (node.split == LayoutSplit::LeftRight ? "x " : "y ") << ratio << ' ';
        WriteNode(out, node.children[0]);
        out << ' ';
        WriteNode(out, node.children[1]);
        }
    else
        {
        bool first = true;
        auto item = [&](const std::string& s) {
            if (!first) out << ' ';
            out << s;
            first = false;
        };
        if (node.nav) item("nav");
        for (int v : node.views) item((v == node.selected ? "*" : "") + std::to_string(v));
        }
    out << ')';
}

// Recursive descent over the dock line. `used` catches a view placed twice,
// which would ask the docking code to put one window in two places.
struct DockParser
{
    const std::string& text;
    size_t pos = 0;
    size_t viewCount = 0;
    std::vector<uint8_t> used;
    bool navSeen = false;
    std::string error;

    DockParser(const std::string& dock, size_t views) : text(dock), viewCount(views), used(views, 0) {}

    void SkipSpace()
    {
        while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    }

    std::string Word()
    {
        SkipSpace();
        const size_t start = pos;
        while (pos < text.size() && text[pos] != ' ' && text[pos] != '\t' && text[pos] != '(' && text[pos] != ')') ++pos;
        return text.substr(start, pos - start);
    }

    bool Fail(const std::string& why)
    {
        if (error.empty()) error = why;
        return false;
    }

    bool Node(LayoutNode& node, int depth)
    {
        if (depth > kMaxDepth) return Fail("the dock tree is nested too deeply");
        SkipSpace();
        if (pos >= text.size() || text[pos] != '(') return Fail("expected '(' in the dock tree");
        ++pos;
        SkipSpace();
        const size_t mark = pos;
        const std::string head = Word();
        if (head == "x" || head == "y")
            {
            node.split = head == "x" ? LayoutSplit::LeftRight : LayoutSplit::TopBottom;
            const std::string ratio = Word();
            char* end = nullptr;
            const float r = std::strtof(ratio.c_str(), &end);
            if (ratio.empty() || end != ratio.c_str() + ratio.size() || std::isnan(r)) return Fail("a split has no ratio");
            node.ratio = std::clamp(r, 0.05f, 0.95f);
            node.children.resize(2);
            if (!Node(node.children[0], depth + 1) || !Node(node.children[1], depth + 1)) return false;
            }
        else
            {
            pos = mark;
            for (;;)
                {
                SkipSpace();
                if (pos < text.size() && text[pos] == ')') break;
                std::string w = Word();
                if (w.empty()) return Fail("unexpected end of the dock tree");
                if (w == "nav")
                    {
                    if (navSeen) return Fail("the navigation pane is placed twice");
                    navSeen = node.nav = true;
                    continue;
                    }
                const bool selected = w[0] == '*';
                if (selected) w.erase(0, 1);
                char* end = nullptr;
                const long v = std::strtol(w.c_str(), &end, 10);
                if (w.empty() || end != w.c_str() + w.size() || v < 0 || (size_t)v >= viewCount)
                    return Fail("the dock tree names a view that is not in the file");
                if (used[(size_t)v]) return Fail("a view is placed twice in the dock tree");
                used[(size_t)v] = 1;
                node.views.push_back((int)v);
                if (selected) node.selected = (int)v;
                }
            }
        SkipSpace();
        if (pos >= text.size() || text[pos] != ')') return Fail("expected ')' in the dock tree");
        ++pos;
        return true;
    }
};

} // namespace

std::string SerializeLayout(const Layout& layout)
{
    std::ostringstream out;
    out << "# Davesplorer layout\n";
    out << "version=1\n";
    out << "active=" << layout.active << '\n';
    for (const LayoutView& v : layout.views)
        out << "view=" << (v.pinned ? 1 : 0) << '|' << (int)v.viewMode << '|' << v.path << '\n';
    out << "dock=";
    WriteNode(out, layout.root);
    out << '\n';
    return out.str();
}

bool ParseLayout(const std::string& text, Layout& out, std::string& error)
{
    Layout layout;
    bool versionOk = false;
    std::string dock;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
        {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        const std::string value = Trim(line.substr(eq + 1));
        if (key == "version")     versionOk = value == "1";
        else if (key == "active") layout.active = std::atoi(value.c_str());
        else if (key == "dock")   dock = value;
        else if (key == "view")
            {
            const size_t a = value.find('|');
            const size_t b = a == std::string::npos ? a : value.find('|', a + 1);
            if (b == std::string::npos)
                {
                error = "A view line is malformed.";
                return false;
                }
            if (layout.views.size() >= kMaxViews)
                {
                error = "The layout has too many views.";
                return false;
                }
            LayoutView v;
            v.pinned = value.substr(0, a) == "1";
            v.viewMode = std::atoi(value.substr(a + 1, b - a - 1).c_str()) == 1 ? ViewMode::Thumbnails : ViewMode::Details;
            v.path = value.substr(b + 1);
            layout.views.push_back(std::move(v));
            }
        }
    if (!versionOk)
        {
        error = "This is not a Davesplorer layout, or it was written by a newer version.";
        return false;
        }
    if (layout.views.empty())
        {
        error = "The layout has no views.";
        return false;
        }
    if (!dock.empty())
        {
        DockParser p(dock, layout.views.size());
        if (!p.Node(layout.root, 0))
            {
            error = "The layout's dock tree is damaged: " + p.error + ".";
            return false;
            }
        }
    layout.active = std::clamp(layout.active, 0, (int)layout.views.size() - 1);
    out = std::move(layout);
    return true;
}

bool SaveLayoutFile(const std::string& file, const Layout& layout, std::string& error)
{
    std::ofstream out(std::filesystem::path(reinterpret_cast<const char8_t*>(file.c_str())), std::ios::trunc);
    if (!out)
        {
        error = "Could not write " + file;
        return false;
        }
    out << SerializeLayout(layout);
    out.close();
    if (!out)
        {
        error = "Could not write " + file;
        return false;
        }
    return true;
}

bool LoadLayoutFile(const std::string& file, Layout& out, std::string& error)
{
    std::ifstream in(std::filesystem::path(reinterpret_cast<const char8_t*>(file.c_str())));
    if (!in)
        {
        error = "Could not open " + file;
        return false;
        }
    std::ostringstream text;
    text << in.rdbuf();
    std::string why;
    if (!ParseLayout(text.str(), out, why))
        {
        error = file + ": " + why;
        return false;
        }
    return true;
}

} // namespace app
