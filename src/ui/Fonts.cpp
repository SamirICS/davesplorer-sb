#include "ui/Fonts.h"
#include "ui/IconsMaterialDesign.h"
#include "platform/FileSystem.h"

#include <algorithm>
#include <vector>

namespace ui::fonts
{

namespace
{

ImFont* g_ui = nullptr;
ImFont* g_bold = nullptr;
ImFont* g_bundledUi = nullptr;     // Open Sans, the fallback for any family
ImFont* g_bundledBold = nullptr;
ImFont* g_italic = nullptr;
ImFont* g_mono = nullptr;
float   g_size = 18.0f;

// ImGui keeps a pointer to these arrays, so they must outlive the atlas.
const ImWchar kTextRanges[] = {
    0x0020, 0x00FF,   // Basic Latin + Latin Supplement
    0x0100, 0x017F,   // Latin Extended-A, for European file names
    0x2000, 0x206F,   // General Punctuation (bullet, dashes, ellipsis)
    0x2190, 0x21FF,   // Arrows
    0x2200, 0x22FF,   // Mathematical Operators
    0,
};
const ImWchar kIconRanges[] = { ICON_MIN_MD, ICON_MAX_16_MD, 0 };

ImFont* AddText(ImGuiIO& io, const std::string& file, float size)
{
    if (!platform::PathExists(file)) return nullptr;
    ImFontConfig cfg;
    return io.Fonts->AddFontFromFileTTF(file.c_str(), size, &cfg, kTextRanges);
}

void MergeIcons(ImGuiIO& io, const std::string& file, float size)
{
    if (!platform::PathExists(file)) return;
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    // Give every icon a full em advance so icon columns line up, and nudge
    // them down: Material glyphs sit high on the text line and look
    // top-heavy inside framed buttons otherwise.
    cfg.GlyphMinAdvanceX = size;
    cfg.GlyphOffset.y = size * 0.18f;
    io.Fonts->AddFontFromFileTTF(file.c_str(), size, &cfg, kIconRanges);
}

// Families looked for on the system, regular face first then bold.
struct SystemFamily
{
    const char* name;
    const char* regular;
    const char* bold;
};
const SystemFamily kSystemFamilies[] = {
    { "Ubuntu",          "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",            "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf" },
    { "Noto Sans",       "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",      "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf" },
    { "DejaVu Sans",     "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf" },
    { "Liberation Sans", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf" },
    { "Fira Code",       nullptr, nullptr },   // the bundled monospace face, as the interface face
};

struct LoadedFamily
{
    std::string name;
    ImFont*     regular = nullptr;
    ImFont*     bold = nullptr;
};
std::vector<LoadedFamily> g_families;
std::vector<std::string>  g_familyNames;

} // namespace

bool Load(const std::string& assetsDir, float sizePx, std::string& error)
{
    ImGuiIO& io = ImGui::GetIO();
    g_size = sizePx;
    const std::string dir = assetsDir + "/fonts/";
    const std::string icons = dir + "MaterialIcons-Regular.ttf";

    g_ui = AddText(io, dir + "OpenSans-Regular.ttf", sizePx);
    if (!g_ui)
        {
        error = "Missing font: " + dir + "OpenSans-Regular.ttf (using the built-in font)";
        g_ui = io.Fonts->AddFontDefault();
        g_bold = g_italic = g_mono = g_ui;
        return false;
        }
    MergeIcons(io, icons, sizePx);

    g_bold = AddText(io, dir + "OpenSans-SemiBold.ttf", sizePx);
    if (g_bold) MergeIcons(io, icons, sizePx); else g_bold = g_ui;

    g_italic = AddText(io, dir + "OpenSans-Italic.ttf", sizePx);
    if (g_italic) MergeIcons(io, icons, sizePx); else g_italic = g_ui;

    g_mono = AddText(io, dir + "FiraCode-Regular.ttf", sizePx);
    if (!g_mono) g_mono = g_ui;

    g_bundledUi = g_ui;
    g_bundledBold = g_bold;
    g_families.push_back({ kDefaultFamily, g_ui, g_bold });
    for (const SystemFamily& f : kSystemFamilies)
        {
        if (!f.regular)
            {
            if (g_mono != g_ui) g_families.push_back({ f.name, g_mono, g_mono });
            continue;
            }
        ImFont* regular = AddText(io, f.regular, sizePx);
        if (!regular) continue;
        MergeIcons(io, icons, sizePx);
        ImFont* bold = AddText(io, f.bold, sizePx);
        if (bold) MergeIcons(io, icons, sizePx);
        g_families.push_back({ f.name, regular, bold ? bold : regular });
        }
    for (const LoadedFamily& f : g_families) g_familyNames.push_back(f.name);

    io.FontDefault = g_ui;
    return true;
}

const std::vector<std::string>& Families()
{
    return g_familyNames;
}

void Apply(const std::string& family, float sizePx)
{
    if (g_families.empty()) return;   // the built-in font: nothing to choose between
    const LoadedFamily* chosen = &g_families.front();
    for (const LoadedFamily& f : g_families)
        if (f.name == family) chosen = &f;
    g_ui = chosen->regular;
    g_bold = chosen->bold;
    ImGui::GetIO().FontDefault = g_ui;
    g_size = std::clamp(sizePx, kMinSize, kMaxSize);
    // Taken up as the next frame starts; FontSizeBase itself is rewritten
    // from the font stack, so setting it directly does not last.
    if (ImGui::GetStyle().FontSizeBase != g_size) ImGui::GetStyle()._NextFrameFontSizeBase = g_size;
}

ImFont* Ui()     { return g_ui; }
ImFont* Bold()   { return g_bold; }
ImFont* Italic() { return g_italic; }
ImFont* Mono()   { return g_mono; }
float   Size()   { return g_size; }

} // namespace ui::fonts
