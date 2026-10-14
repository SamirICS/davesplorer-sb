// A saved layout: the folder views that were open, each with its pin, and
// how the window was split between them. Written as a small text file the
// user names, so several arrangements can be kept and switched between.
#pragma once

#include "app/Settings.h"

#include <string>
#include <vector>

namespace app
{

struct LayoutView
{
    std::string path;              // "" is This PC
    ViewMode    viewMode = ViewMode::Details;
    bool        pinned = false;
};

enum class LayoutSplit
{
    None,          // a leaf: a stack of tabs
    LeftRight,     // children side by side
    TopBottom,     // children one above the other
};

// One node of the docking tree. A split has exactly two children; a leaf
// holds the navigation pane and/or views, by index into Layout::views.
struct LayoutNode
{
    LayoutSplit split = LayoutSplit::None;
    float       ratio = 0.5f;      // the first child's share of the split
    std::vector<LayoutNode> children;

    bool             nav = false;
    std::vector<int> views;
    int              selected = -1;   // the view index whose tab is in front, -1 for the first
};

struct Layout
{
    std::vector<LayoutView> views;
    LayoutNode root;
    int active = 0;                // index into views: the view that has focus
};

// The file's extension, without the dot.
inline constexpr const char* kLayoutExtension = "dslayout";

std::string SerializeLayout(const Layout& layout);
bool ParseLayout(const std::string& text, Layout& out, std::string& error);

bool SaveLayoutFile(const std::string& file, const Layout& layout, std::string& error);
bool LoadLayoutFile(const std::string& file, Layout& out, std::string& error);

} // namespace app
