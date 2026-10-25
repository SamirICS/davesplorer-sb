// The PDF viewer: a file view showing every page of a PDF in one scrolled
// column, rendered by PDFium on a thread of its own so the window keeps
// responding. Zoom, fit to width or page, rotation, find with every match
// lit up, the document's own links and outline, and text selection that
// Ctrl+C copies.
//
// The canvas and the render cache are davepdf's (github.com/evaderkrub/
// davepdf, MIT), moved from one window per document to one state per view.
#include "ui/MainWindow.h"
#include "ui/IconsMaterialDesign.h"
#include "ui/Textures.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include "app/Format.h"
#include "app/Pdf.h"
#include "platform/PdfEngine.h"
#include "platform/Shell.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui
{

namespace pdf = app::pdf;

struct PdfViewUi
{
    std::string loadedPath;   // what `doc` holds; differs from the view's path until loaded
    uint64_t    fileBytes = 0;
    std::string error;        // shown in place of the pages

    // The renderer works on the document, so it is declared after it and
    // destroyed first.
    std::unique_ptr<platform::PdfDocument> doc;
    std::unique_ptr<platform::PdfRenderer> renderer;

    bool        needsPassword = false;
    std::string password;
    bool        passwordWantFocus = false;

    pdf::ViewState                view;
    std::vector<pdf::OutlineItem> outline;
    unsigned                      generation = 0;   // bumped per document: stale renders are dropped

    // Rendered pages, keyed by page, size and rotation. The renderer
    // makes the pixels; the frame uploads them.
    struct CachedPage
    {
        ImTextureData* texture = nullptr;
        int            page = 0, width = 0, height = 0, rotation = 0;
        unsigned       generation = 0;
        int            lastUsed = 0;    // frame
        size_t         bytes = 0;
    };
    std::vector<CachedPage>                         cache;
    size_t                                          cacheBytes = 0;
    std::vector<platform::PdfRenderResult>          inbox;   // finished, waiting to be uploaded
    std::unordered_map<int, std::vector<pdf::LinkBox>> links;
    bool                                            renderPending = false;

    // What the canvas showed last frame, so a zoom made anywhere (toolbar,
    // keys, wheel) keeps the same place in view.
    float    lastZoom = -1;
    int      lastRotation = 0;
    unsigned lastGeneration = ~0u;
    double   viewChangedAt = 0;
    bool     dragging = false;
    ImVec2   viewSize{};

    struct ScrollRequest
        {
        bool  pending = false;
        int   page = 0;
        float fraction = 0;    // 0..1 down the page
        bool  center = false;  // that point mid-view rather than at the top
        } scroll;

    int pageInput = 1;
    // The page a jump went to stays the current one until the reader
    // scrolls: the last pages of a document cannot reach the top of the
    // view, and the page with most of the view would otherwise be named.
    int pinnedPage = -1;

    bool        findOpen = false;
    bool        findWantFocus = false;
    std::string findText;
    std::string findScanned;    // the text the hits are for
    bool        matchCase = false;
    bool        findScannedCase = false;
    int         findProgress = 0;   // pages searched so far
    int         findStartPage = 0;
    std::vector<pdf::SearchHit> hits;
    int         currentHit = -1;
    int         findStep = 0;      // +1/-1 asked for before the scan had found anything

    pdf::TextSelection selection;
    bool               selecting = false;
    bool               selectionDirty = false;   // its boxes wait for the engine
    std::string        status;                    // a passing note for the status bar

    ~PdfViewUi()
    {
        renderer.reset();   // joins the render thread before the document goes
        for (CachedPage& c : cache) textures::Release(c.texture);
    }
};

namespace
{

using Clock = std::chrono::steady_clock;

// Uploads per frame are capped, so a burst of finished renders cannot
// stall a frame. The cache is per view; several PDFs may be open.
constexpr size_t kUploadBudgetBytes = 48ull * 1024 * 1024;
constexpr size_t kCacheBudgetBytes = 192ull * 1024 * 1024;
constexpr int    kMaxTexture = 8192;
// How long a zoom or rotation must hold still before pages re-render at
// the new size: until then the cached page is stretched.
constexpr double kSettleSeconds = 0.12;
// Pages wider than this get a quick half-size pass first when nothing
// cached can stand in for them.
constexpr int kPreviewThreshold = 600;

void ClearCache(PdfViewUi& p)
{
    if (p.renderer) p.renderer->CancelAll();
    p.inbox.clear();
    for (PdfViewUi::CachedPage& c : p.cache) textures::Release(c.texture);
    p.cache.clear();
    p.cacheBytes = 0;
    p.links.clear();
}

uint64_t FileBytes(const std::string& path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    return ec ? 0 : (uint64_t)size;
}

void Load(PdfViewUi& p, const std::string& path)
{
    p.renderer.reset();
    ClearCache(p);
    p.loadedPath = path;
    p.fileBytes = FileBytes(path);
    p.error.clear();
    p.view.pages.clear();
    p.view.currentPage = 0;
    p.view.rotation = 0;
    p.outline.clear();
    p.hits.clear();
    p.currentHit = -1;
    p.findScanned.clear();
    p.selection.clear();
    p.selecting = false;
    p.pinnedPage = -1;
    ++p.generation;
    if (!p.doc) p.doc = std::make_unique<platform::PdfDocument>();
    bool ok = false;
    {
    std::lock_guard<std::mutex> lock(platform::PdfLock());
    ok = p.doc->LoadFile(path, p.password, p.error);
    if (ok)
        {
        p.view.pages = p.doc->PageSizes();
        p.outline = p.doc->Outline();
        }
    }
    p.needsPassword = !ok && p.doc->NeedsPassword();
    if (p.needsPassword)
        {
        if (!p.password.empty()) p.error = "That password was not accepted.";
        else p.error.clear();
        p.password.clear();
        p.passwordWantFocus = true;
        return;
        }
    p.password.clear();
    if (!ok) return;
    if (p.view.PageCount() == 0)
        {
        p.error = "The document has no pages.";
        return;
        }
    p.renderer = std::make_unique<platform::PdfRenderer>(*p.doc);
    p.scroll = { true, 0, 0, false };
}

void UploadResults(PdfViewUi& p)
{
    if (!p.renderer) return;
    for (platform::PdfRenderResult& r : p.renderer->Collect()) p.inbox.push_back(std::move(r));
    size_t uploaded = 0;
    while (!p.inbox.empty() && uploaded < kUploadBudgetBytes)
        {
        platform::PdfRenderResult r = std::move(p.inbox.front());
        p.inbox.erase(p.inbox.begin());
        const platform::PdfRenderJob& j = r.job;
        // A render that outlived its document or rotation is dropped.
        if (j.generation != p.generation || j.rotation != p.view.rotation) continue;
        // Replace the same image rather than keep two.
        for (size_t i = 0; i < p.cache.size(); ++i)
            {
            const PdfViewUi::CachedPage& e = p.cache[i];
            if (e.page == j.page && e.width == j.width && e.height == j.height && e.rotation == j.rotation && e.generation == j.generation)
                {
                textures::Release(e.texture);
                p.cacheBytes -= e.bytes;
                p.cache[i] = p.cache.back();
                p.cache.pop_back();
                break;
                }
            }
        PdfViewUi::CachedPage entry;
        entry.texture = textures::Create(j.width, j.height, r.rgba.data());
        entry.page = j.page;
        entry.width = j.width;
        entry.height = j.height;
        entry.rotation = j.rotation;
        entry.generation = j.generation;
        entry.lastUsed = ImGui::GetFrameCount();
        entry.bytes = r.rgba.size();
        p.cache.push_back(entry);
        p.cacheBytes += entry.bytes;
        uploaded += entry.bytes;
        }
}

void Evict(PdfViewUi& p)
{
    // Renders of another rotation or document can never be shown again.
    for (size_t i = 0; i < p.cache.size();)
        {
        PdfViewUi::CachedPage& e = p.cache[i];
        if (e.rotation != p.view.rotation || e.generation != p.generation)
            {
            textures::Release(e.texture);
            p.cacheBytes -= e.bytes;
            p.cache[i] = p.cache.back();
            p.cache.pop_back();
            }
        else
            {
            ++i;
            }
        }
    const int frame = ImGui::GetFrameCount();
    while (p.cacheBytes > kCacheBudgetBytes)
        {
        size_t oldest = p.cache.size();
        for (size_t i = 0; i < p.cache.size(); ++i)
            if (p.cache[i].lastUsed != frame && (oldest == p.cache.size() || p.cache[i].lastUsed < p.cache[oldest].lastUsed)) oldest = i;
        if (oldest == p.cache.size()) break;   // everything left is on screen
        textures::Release(p.cache[oldest].texture);
        p.cacheBytes -= p.cache[oldest].bytes;
        p.cache[oldest] = p.cache.back();
        p.cache.pop_back();
        }
}

// The texture to draw a page with: the exact size if it is ready, else the
// nearest size of the same page stretched while the exact one renders.
ImTextureData* PageImage(PdfViewUi& p, int page, int width, int height)
{
    if (!p.renderer || !p.view.ValidPage(page) || width < 1 || height < 1) return nullptr;
    width = std::min(width, kMaxTexture);
    height = std::min(height, kMaxTexture);
    const int frame = ImGui::GetFrameCount();
    for (PdfViewUi::CachedPage& e : p.cache)
        if (e.page == page && e.width == width && e.height == height && e.rotation == p.view.rotation && e.generation == p.generation)
            {
            e.lastUsed = frame;
            return e.texture;
            }
    PdfViewUi::CachedPage* best = nullptr;
    for (PdfViewUi::CachedPage& e : p.cache)
        if (e.page == page && e.rotation == p.view.rotation && e.generation == p.generation && (!best || std::abs(e.width - width) < std::abs(best->width - width)))
            best = &e;
    const bool settling = ImGui::GetTime() - p.viewChangedAt < kSettleSeconds;
    // Collected but held back by the upload budget: it is on its way.
    bool arriving = false;
    for (const platform::PdfRenderResult& r : p.inbox)
        if (r.job.page == page && r.job.width == width && r.job.height == height && r.job.rotation == p.view.rotation && r.job.generation == p.generation)
            arriving = true;
    if (!(settling && best) && !arriving)
        {
        // Pages near the one being read come first.
        platform::PdfRenderJob job;
        job.page = page;
        job.width = width;
        job.height = height;
        job.rotation = p.view.rotation;
        job.generation = p.generation;
        job.priority = std::abs(page - p.view.currentPage);
        job.requested = (uint64_t)frame;
        if (!best && width > kPreviewThreshold)
            {
            // Nothing to show yet: a half-size pass lands in a quarter of the time.
            platform::PdfRenderJob preview = job;
            preview.width = std::max(1, width / 2);
            preview.height = std::max(1, height / 2);
            preview.priority -= 1;
            p.renderer->Submit(preview);
            }
        p.renderer->Submit(job);
        }
    p.renderPending = true;
    if (!best) return nullptr;
    best->lastUsed = frame;
    return best->texture;
}

const std::vector<pdf::LinkBox>& PageLinks(PdfViewUi& p, int page)
{
    static const std::vector<pdf::LinkBox> none;
    if (!p.renderer || !p.view.ValidPage(page)) return none;
    auto it = p.links.find(page);
    if (it == p.links.end())
        {
        // A render holds the engine: asked again next frame.
        std::unique_lock<std::mutex> lock(platform::PdfLock(), std::try_to_lock);
        if (!lock.owns_lock()) return none;
        it = p.links.emplace(page, p.doc->Links(page)).first;
        }
    return it->second;
}

int CharIndexAt(PdfViewUi& p, int page, float nx, float ny, bool wait)
{
    if (!p.renderer || !p.view.ValidPage(page)) return -1;
    std::unique_lock<std::mutex> lock(platform::PdfLock(), std::defer_lock);
    if (wait) lock.lock();
    else if (!lock.try_lock()) return -1;   // hovering simply asks again next frame
    return p.doc->CharIndexAt(page, nx, ny);
}

void SetSelection(PdfViewUi& p, int page, int anchor, int focus)
{
    if (!p.renderer || !p.view.ValidPage(page) || anchor < 0 || focus < 0)
        {
        p.selection.clear();
        p.selectionDirty = false;
        return;
        }
    const bool same = p.selection.page == page && p.selection.anchor == anchor && p.selection.focus == focus && !p.selectionDirty;
    p.selection.page = page;
    p.selection.anchor = anchor;
    p.selection.focus = focus;
    if (same) return;
    std::unique_lock<std::mutex> lock(platform::PdfLock(), std::try_to_lock);
    if (!lock.owns_lock())
        {
        p.selectionDirty = true;   // the boxes follow next frame
        return;
        }
    p.selection.rects = p.doc->TextRects(page, p.selection.first(), p.selection.count());
    p.selectionDirty = false;
}

// Ctrl+C: the selection, or the whole of the page being read.
void CopyText(PdfViewUi& p)
{
    if (!p.renderer) return;
    std::string text;
    const bool whole = p.selection.empty();
    {
    std::lock_guard<std::mutex> lock(platform::PdfLock());
    text = whole ? p.doc->PageText(p.view.currentPage) : p.doc->TextRange(p.selection.page, p.selection.first(), p.selection.count());
    }
    if (text.empty())
        {
        p.status = whole ? "Page " + std::to_string(p.view.currentPage + 1) + " has no text to copy" : "The selection has no text";
        return;
        }
    ImGui::SetClipboardText(text.c_str());
    p.status = whole ? "Copied the text of page " + std::to_string(p.view.currentPage + 1) : "Copied the selection";
}

void GoToPage(PdfViewUi& p, int page)
{
    if (p.view.PageCount() == 0) return;
    page = std::clamp(page, 0, p.view.PageCount() - 1);
    p.scroll = { true, page, 0, false };
    p.pinnedPage = page;
}

void JumpToHit(PdfViewUi& p, const pdf::SearchHit& hit)
{
    const pdf::RectF r = pdf::RotateNormalized(hit.rects.front(), p.view.rotation);
    p.scroll = { true, hit.page, r.y + r.h / 2, true };
    p.pinnedPage = hit.page;
}

void StepHit(PdfViewUi& p, int dir)
{
    if (p.hits.empty())
        {
        p.findStep = dir;   // waits for the scan
        return;
        }
    const int n = (int)p.hits.size();
    p.currentHit = ((p.currentHit + dir) % n + n) % n;
    JumpToHit(p, p.hits[(size_t)p.currentHit]);
}

// Searches a few milliseconds a frame, starting at the page being read so
// the first match is the one a reader expects.
void FindPump(PdfViewUi& p)
{
    const int count = p.view.PageCount();
    if (!p.renderer || !p.findOpen || p.findText.empty())
        {
        p.hits.clear();
        p.currentHit = -1;
        p.findScanned.clear();
        p.findProgress = count;
        return;
        }
    if (p.findText != p.findScanned || p.matchCase != p.findScannedCase)
        {
        p.findScanned = p.findText;
        p.findScannedCase = p.matchCase;
        p.hits.clear();
        p.currentHit = -1;
        p.findProgress = 0;
        p.findStartPage = std::clamp(p.view.currentPage, 0, std::max(0, count - 1));
        }
    std::unique_lock<std::mutex> lock(platform::PdfLock(), std::try_to_lock);
    const auto started = Clock::now();
    while (lock.owns_lock() && p.findProgress < count && std::chrono::duration<double, std::milli>(Clock::now() - started).count() < 4.0)
        {
        const int page = (p.findStartPage + p.findProgress) % count;
        const size_t before = p.hits.size();
        p.doc->Search(page, p.findText, p.matchCase, p.hits);
        ++p.findProgress;
        if (p.currentHit < 0 && p.hits.size() > before)
            {
            p.currentHit = (int)before;
            JumpToHit(p, p.hits[before]);
            }
        }
    if (lock.owns_lock()) lock.unlock();
    if (p.findStep != 0 && !p.hits.empty())
        {
        StepHit(p, p.findStep);
        p.findStep = 0;
        }
    if (p.findProgress >= count) p.findStep = 0;
}

void OpenFind(PdfViewUi& p)
{
    p.findOpen = true;
    p.findWantFocus = true;
}

void CloseFind(PdfViewUi& p)
{
    p.findOpen = false;
    p.findText.clear();
    p.hits.clear();
    p.currentHit = -1;
}

void ActivateLink(UiState& ui, PdfViewUi& p, const pdf::LinkBox& link)
{
    if (link.page >= 0)
        {
        GoToPage(p, link.page);
        return;
        }
    std::string error;
    if (!platform::OpenUrl(link.uri, error)) ShowError(ui, error);
}

void SetZoom(PdfViewUi& p, float zoom)
{
    p.view.fit = pdf::FitMode::Manual;
    p.view.zoom = pdf::ClampZoom(zoom);
}

ImU32 CanvasBackground()
{
    const ThemeTokens& t = ThemeAt(CurrentThemeIndex());
    // A touch away from the window colour, so pages read as sheets lying
    // on a desk rather than holes in the window.
    return Lift(t.background, t.isDark ? 0.045f : 0.09f, t.isDark);
}

void DrawOutlinePopup(PdfViewUi& p)
{
    if (!ImGui::BeginPopup("##outline")) return;
    const float indent = ImGui::GetFontSize();
    ImGui::BeginChild("##outlineList", ImVec2(ImGui::GetFontSize() * 22, std::min(ImGui::GetFontSize() * 24, (p.outline.size() + 1) * ImGui::GetFrameHeight())));
    for (size_t i = 0; i < p.outline.size(); ++i)
        {
        const pdf::OutlineItem& item = p.outline[i];
        ImGui::PushID((int)i);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent * (float)std::min(item.depth, 8));
        const std::string label = item.title.empty() ? std::string("(untitled)") : item.title;
        if (ImGui::Selectable(label.c_str(), false, item.page < 0 ? ImGuiSelectableFlags_Disabled : 0))
            {
            GoToPage(p, item.page);
            ImGui::CloseCurrentPopup();
            }
        if (item.page >= 0)
            {
            const std::string num = std::to_string(item.page + 1);
            ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(num.c_str()).x);
            FaintText(num.c_str());
            }
        ImGui::PopID();
        }
    ImGui::EndChild();
    ImGui::EndPopup();
}

void DrawToolbar(app::AppState& state, UiState& ui, app::FileView& view, PdfViewUi& p)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonW = [&](const char* icon) { return ImGui::CalcTextSize(icon).x + style.FramePadding.x * 2.0f; };
    const float gap = style.ItemSpacing.x;
    const bool have = p.renderer != nullptr;

    if (IconButton("Contents", ICON_MD_TOC, "Contents", have && !p.outline.empty())) ImGui::OpenPopup("##outline");
    DrawOutlinePopup(p);

    // Pages: previous, the page box, "of N", next.
    const std::string of = "of " + std::to_string(p.view.PageCount());
    const float pageBoxW = ImGui::CalcTextSize("00000").x + style.FramePadding.x * 2;
    ToolbarGroup(buttonW(ICON_MD_KEYBOARD_ARROW_UP) * 2 + pageBoxW + ImGui::CalcTextSize(of.c_str()).x + gap * 3);
    if (IconButton("PrevPage", ICON_MD_KEYBOARD_ARROW_UP, "Previous page", have && p.view.currentPage > 0)) GoToPage(p, p.view.currentPage - 1);
    ImGui::SameLine();
    // The page box shows the page being read, and takes a number to go to.
    const ImGuiID pageId = ImGui::GetID("##page");
    if (ImGui::GetActiveID() != pageId) p.pageInput = p.view.currentPage + 1;
    ImGui::SetNextItemWidth(pageBoxW);
    ImGui::BeginDisabled(!have);
    if (ImGui::InputInt("##page", &p.pageInput, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) GoToPage(p, p.pageInput - 1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    MutedText(of.c_str());
    ImGui::SameLine();
    if (IconButton("NextPage", ICON_MD_KEYBOARD_ARROW_DOWN, "Next page", have && p.view.currentPage + 1 < p.view.PageCount())) GoToPage(p, p.view.currentPage + 1);

    // Zoom: out, the percentage, in.
    const float slotW = ImGui::CalcTextSize("1600%").x;
    ToolbarGroup(buttonW(ICON_MD_ZOOM_OUT) * 2 + slotW + gap * 2);
    if (IconButton("ZoomOut", ICON_MD_ZOOM_OUT, "Zoom out (-)", have)) SetZoom(p, pdf::ZoomStep(p.view.zoom, false));
    ImGui::SameLine();
    char pct[32];
    std::snprintf(pct, sizeof(pct), "%d%%", (int)std::lround(p.view.zoom * 100.0f));
    // Centred in a fixed slot so the buttons either side stay put.
    const float slotX = ImGui::GetCursorPosX();
    ImGui::SetCursorPosX(slotX + std::max(0.0f, (slotW - ImGui::CalcTextSize(pct).x) * 0.5f));
    ImGui::AlignTextToFramePadding();
    MutedText(have ? pct : "");
    ImGui::SameLine();
    ImGui::SetCursorPosX(slotX + slotW + gap);
    if (IconButton("ZoomIn", ICON_MD_ZOOM_IN, "Zoom in (+)", have)) SetZoom(p, pdf::ZoomStep(p.view.zoom, true));

    // Fit, turn, find.
    ToolbarGroup(buttonW(ICON_MD_SWAP_HORIZ) * 4 + gap * 3);
    if (IconButton("FitWidth", ICON_MD_SWAP_HORIZ, "Fit width (W)", have)) p.view.fit = pdf::FitMode::Width;
    ImGui::SameLine();
    if (IconButton("FitPage", ICON_MD_FIT_SCREEN, "Fit page (0)", have)) p.view.fit = pdf::FitMode::Page;
    ImGui::SameLine();
    if (IconButton("Rotate", ICON_MD_ROTATE_RIGHT, "Rotate clockwise (R)", have)) p.view.Rotate(1);
    ImGui::SameLine();
    if (IconButton("Find", ICON_MD_SEARCH, "Find (Ctrl+F)", have))
        {
        if (p.findOpen) CloseFind(p);
        else OpenFind(p);
        }

    // The file's folder view, and the program the desktop would use, on
    // the right.
    const float cluster = buttonW(ICON_MD_FOLDER_OPEN) + buttonW(ICON_MD_OPEN_IN_NEW) + gap;
    ToolbarGroup(cluster);
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - cluster));
    if (IconButton("ShowInFolder", ICON_MD_FOLDER_OPEN, "Show in folder view")) ShowInFolderView(state, ui, view.path);
    ImGui::SameLine();
    if (IconButton("OpenExternal", ICON_MD_OPEN_IN_NEW, "Open with default program"))
        {
        std::string error;
        if (!platform::OpenWithShell(view.path, error)) ShowError(ui, error);
        }
}

void DrawFindBar(PdfViewUi& p)
{
    if (p.findWantFocus)
        {
        ImGui::SetKeyboardFocusHere();
        p.findWantFocus = false;
        }
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.5f, ImGui::GetFontSize() * 18));
    if (ImGui::InputTextWithHint("##find", "Find in document", &p.findText, ImGuiInputTextFlags_EnterReturnsTrue))
        {
        StepHit(p, ImGui::GetIO().KeyShift ? -1 : 1);
        p.findWantFocus = true;   // Enter leaves the field; keep typing in it
        }
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) CloseFind(p);
    ImGui::SameLine();
    if (IconButton("FindPrev", ICON_MD_KEYBOARD_ARROW_UP, "Previous match (Shift+Enter)", !p.findText.empty())) StepHit(p, -1);
    ImGui::SameLine();
    if (IconButton("FindNext", ICON_MD_KEYBOARD_ARROW_DOWN, "Next match (Enter)", !p.findText.empty())) StepHit(p, 1);
    ToolbarGroup(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Match case").x);
    ImGui::Checkbox("Match case", &p.matchCase);
    if (!p.findText.empty())
        {
        std::string count;
        if (p.hits.empty()) count = p.findProgress < p.view.PageCount() ? "Searching..." : "No matches";
        else count = std::to_string(p.currentHit + 1) + " of " + std::to_string(p.hits.size()) + (p.findProgress < p.view.PageCount() ? "+" : "");
        ToolbarGroup(ImGui::CalcTextSize(count.c_str()).x);
        ImGui::AlignTextToFramePadding();
        MutedText(count.c_str());
        }
    ToolbarGroup(ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()));
    if (IconButton("FindClose", ICON_MD_CLOSE, "Close (Escape)")) CloseFind(p);
}

void DrawPage(PdfViewUi& p, ImDrawList* dl, const pdf::PageRect& pr, ImVec2 origin, float us, float fbScale)
{
    const Palette& pal = CurrentPalette();
    const ImVec2 a(origin.x + pr.rect.x, origin.y + pr.rect.y);
    const ImVec2 b(a.x + pr.rect.w, a.y + pr.rect.h);
    // A soft shadow: three widening translucent rectangles.
    for (int i = 3; i >= 1; --i)
        {
        const float spread = (float)i * 2.0f * us;
        dl->AddRectFilled(ImVec2(a.x - spread + 1, a.y - spread + 3 * us), ImVec2(b.x + spread - 1, b.y + spread + 2 * us), IM_COL32(0, 0, 0, 22), 4 * us);
        }
    // Rendered at the framebuffer's resolution, so a Retina display gets
    // its own pixels rather than a stretched page.
    ImTextureData* tex = PageImage(p, pr.page, (int)std::lround(pr.rect.w * fbScale), (int)std::lround(pr.rect.h * fbScale));
    if (tex) dl->AddImage(tex->GetTexRef(), a, b);
    else dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, 255));
    dl->AddRect(a, b, WithAlpha(pal.border, 160), 0, 0, 1.0f);

    auto box = [&](const pdf::RectF& normalized, float grow) {
        const pdf::RectF r = pdf::RotateNormalized(normalized, p.view.rotation);
        return ImRect(ImVec2(a.x + r.x * pr.rect.w - grow, a.y + r.y * pr.rect.h - grow),
                      ImVec2(a.x + (r.x + r.w) * pr.rect.w + grow, a.y + (r.y + r.h) * pr.rect.h + grow));
    };
    if (p.selection.page == pr.page)
        {
        const ImU32 fill = ThemeAt(CurrentThemeIndex()).selection;
        for (const pdf::RectF& nr : p.selection.rects)
            {
            const ImRect r = box(nr, 0);
            dl->AddRectFilled(r.Min, r.Max, fill);
            }
        }
    for (size_t i = 0; i < p.hits.size(); ++i)
        {
        const pdf::SearchHit& hit = p.hits[i];
        if (hit.page != pr.page) continue;
        const bool current = (int)i == p.currentHit;
        for (const pdf::RectF& nr : hit.rects)
            {
            const ImRect r = box(nr, 1);
            dl->AddRectFilled(r.Min, r.Max, WithAlpha(current ? pal.accent : pal.warning, current ? 120 : 80), 2 * us);
            if (current) dl->AddRect(r.Min, r.Max, pal.accent, 2 * us, 0, 1.5f * us);
            }
        }
}

void DrawCanvas(UiState& ui, PdfViewUi& p, ImVec2 size)
{
    const float us = ImGui::GetFontSize() / 16.0f;   // davepdf's spacing was laid out at a 16px font
    const float pixelScale = ui.dpiScale;
    const float fbScale = std::max(1.0f, ImGui::GetIO().DisplayFramebufferScale.x);
    ImGuiIO& io = ImGui::GetIO();
    p.view.gap = std::round(14 * us);
    p.view.margin = std::round(20 * us);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, CanvasBackground());
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // The vertical scrollbar is always there: fit-to-width would otherwise
    // flip between "fits, no scrollbar" and "scrollbar, no longer fits".
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysVerticalScrollbar |
                                   ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNavInputs;
    const bool open = ImGui::BeginChild("##pages", size, ImGuiChildFlags_None, flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!open)
        {
        ImGui::EndChild();
        return;
        }

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const ImVec2 viewSize(window->InnerRect.GetWidth(), window->InnerRect.GetHeight());
    p.viewSize = viewSize;

    // 1. This frame's zoom. Fit page fits the page being read.
    if (p.pinnedPage >= 0 && p.view.ValidPage(p.pinnedPage)) p.view.currentPage = p.pinnedPage;
    if (p.view.fit != pdf::FitMode::Manual) p.view.zoom = pdf::FitZoom(p.view, viewSize.x, viewSize.y, pixelScale, p.view.fit);
    float scrollY = ImGui::GetScrollY();
    float scrollX = ImGui::GetScrollX();
    const bool sameDoc = p.lastGeneration == p.generation;
    const bool zoomChanged = p.lastZoom > 0 && std::fabs(p.lastZoom - p.view.zoom) > 1e-6f && sameDoc;
    if (zoomChanged && !p.scroll.pending && p.view.fit == pdf::FitMode::Page)
        {
        // A page at a time: the page being read stays at the top. Keeping
        // the middle still instead could land on a neighbour of another
        // size, whose own fit would move it back.
        GoToPage(p, p.view.currentPage);
        }
    else if (zoomChanged && !p.scroll.pending)
        {
        // Keep the point at the middle of the view where it was.
        scrollY = pdf::ScrollAfterZoom(scrollY, viewSize.y / 2, p.lastZoom, p.view.zoom);
        scrollX = pdf::ScrollAfterZoom(scrollX, viewSize.x / 2, p.lastZoom, p.view.zoom);
        }
    if ((zoomChanged || p.lastRotation != p.view.rotation) && sameDoc) p.viewChangedAt = ImGui::GetTime();
    p.lastZoom = p.view.zoom;
    p.lastRotation = p.view.rotation;
    p.lastGeneration = p.generation;

    const pdf::Layout layout = pdf::LayoutPages(p.view, pixelScale);
    const float contentW = std::max(layout.width, viewSize.x);
    const float contentH = std::max(layout.height, viewSize.y);
    const float originX = (contentW - layout.width) / 2;   // a narrow stack sits in the middle

    // 2. A jump asked for by the toolbar, a link, the outline or find.
    if (p.scroll.pending && p.view.ValidPage(p.scroll.page))
        {
        const pdf::RectF& r = layout.pages[(size_t)p.scroll.page].rect;
        scrollY = r.y + p.scroll.fraction * r.h - (p.scroll.center ? viewSize.y / 2 : p.view.gap);
        if (p.scroll.page == 0 && !p.scroll.center && p.scroll.fraction <= 0) scrollY = 0;
        p.scroll.pending = false;
        }

    // The reader's own scrolling, from here on, lets go of a pinned page.
    const float scrollBeforeInput = scrollY;
    const bool scrollbarHeld = ImGui::GetActiveID() == ImGui::GetWindowScrollbarID(window, ImGuiAxis_Y);

    // 3. The wheel scrolls; Shift scrolls sideways; Ctrl zooms about the pointer.
    const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (hovered && io.MouseWheel != 0.0f)
        {
        if (io.KeyCtrl)
            {
            const float oldZoom = p.view.zoom;
            const float newZoom = pdf::ClampZoom(oldZoom * std::pow(1.15f, io.MouseWheel));
            if (newZoom != oldZoom)
                {
                const ImVec2 rel(io.MousePos.x - window->InnerRect.Min.x, io.MousePos.y - window->InnerRect.Min.y);
                SetZoom(p, newZoom);
                scrollY = pdf::ScrollAfterZoom(scrollY, rel.y, oldZoom, newZoom);
                scrollX = pdf::ScrollAfterZoom(scrollX, rel.x, oldZoom, newZoom);
                p.lastZoom = newZoom;
                p.viewChangedAt = ImGui::GetTime();
                }
            ui.wheelTaken = true;   // not the interface zoom as well
            }
        else if (io.KeyShift)
            {
            scrollX -= io.MouseWheel * 90 * us;
            }
        else
            {
            scrollY -= io.MouseWheel * 90 * us;
            }
        }
    if (hovered && io.MouseWheelH != 0.0f) scrollX += io.MouseWheelH * 90 * us;

    // 4. Keys, while this view has the focus and no text field does.
    const bool keys = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !io.WantTextInput && !io.KeyCtrl;
    if (keys && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        {
        const float line = 60 * us, pageStep = viewSize.y * 0.9f;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) scrollY += line;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) scrollY -= line;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            {
            if (layout.width > viewSize.x) scrollX += line;
            else GoToPage(p, p.view.currentPage + 1);
            }
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            {
            if (layout.width > viewSize.x) scrollX -= line;
            else GoToPage(p, p.view.currentPage - 1);
            }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) || (ImGui::IsKeyPressed(ImGuiKey_Space) && !io.KeyShift)) scrollY += pageStep;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp) || (ImGui::IsKeyPressed(ImGuiKey_Space) && io.KeyShift)) scrollY -= pageStep;
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) scrollY = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End)) scrollY = contentH;
        if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) SetZoom(p, pdf::ZoomStep(p.view.zoom, true));
        if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) SetZoom(p, pdf::ZoomStep(p.view.zoom, false));
        if (ImGui::IsKeyPressed(ImGuiKey_0) || ImGui::IsKeyPressed(ImGuiKey_Keypad0)) p.view.fit = pdf::FitMode::Page;
        if (ImGui::IsKeyPressed(ImGuiKey_1) || ImGui::IsKeyPressed(ImGuiKey_Keypad1)) SetZoom(p, 1.0f);
        if (ImGui::IsKeyPressed(ImGuiKey_W)) p.view.fit = pdf::FitMode::Width;
        if (ImGui::IsKeyPressed(ImGuiKey_R)) p.view.Rotate(io.KeyShift ? -1 : 1);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            {
            if (p.findOpen) CloseFind(p);
            else p.selection.clear();
            }
        }

    // 5. One item the size of the stack: it makes the scrollbars right and
    //    is what a drag pans and a click follows a link through.
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##stack", ImVec2(contentW, contentH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
    const bool itemActive = ImGui::IsItemActive();
    const bool itemHovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated();
    const bool deactivated = ImGui::IsItemDeactivated();
    if (activated) p.dragging = false;
    // A drag that did not start on text pans, as does any middle drag.
    if (itemActive && !p.selecting && (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 4.0f)))
        {
        p.dragging = true;
        scrollX -= io.MouseDelta.x;
        scrollY -= io.MouseDelta.y;
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        }

    if (scrollbarHeld || scrollY != scrollBeforeInput) p.pinnedPage = -1;
    scrollX = pdf::ClampScroll(scrollX, contentW, viewSize.x);
    scrollY = pdf::ClampScroll(scrollY, contentH, viewSize.y);
    ImGui::SetScrollX(scrollX);
    ImGui::SetScrollY(scrollY);
    p.view.currentPage = p.view.ValidPage(p.pinnedPage) ? p.pinnedPage : pdf::PageAt(layout, scrollY, viewSize.y);

    // 6. The pages in view, and what is under the pointer.
    const ImVec2 origin(window->InnerRect.Min.x - scrollX + originX, window->InnerRect.Min.y - scrollY);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const pdf::LinkBox* hoveredLink = nullptr;
    int hoveredPage = -1;
    ImVec2 hoveredNorm;   // the pointer on the hovered page, 0..1 as shown
    for (int page : pdf::VisiblePages(layout, scrollY, viewSize.y, 1))
        {
        const pdf::PageRect& pr = layout.pages[(size_t)page];
        DrawPage(p, dl, pr, origin, us, fbScale);
        if (!itemHovered && !(itemActive && p.selecting)) continue;
        const ImVec2 a(origin.x + pr.rect.x, origin.y + pr.rect.y);
        if (io.MousePos.x < a.x || io.MousePos.y < a.y || io.MousePos.x > a.x + pr.rect.w || io.MousePos.y > a.y + pr.rect.h) continue;
        hoveredPage = page;
        hoveredNorm = ImVec2((io.MousePos.x - a.x) / pr.rect.w, (io.MousePos.y - a.y) / pr.rect.h);
        for (const pdf::LinkBox& link : PageLinks(p, page))
            {
            const pdf::RectF r = pdf::RotateNormalized(link.rect, p.view.rotation);
            const ImVec2 la(a.x + r.x * pr.rect.w, a.y + r.y * pr.rect.h);
            const ImVec2 lb(la.x + r.w * pr.rect.w, la.y + r.h * pr.rect.h);
            if (io.MousePos.x >= la.x && io.MousePos.x <= lb.x && io.MousePos.y >= la.y && io.MousePos.y <= lb.y)
                {
                hoveredLink = &link;
                dl->AddRectFilled(la, lb, WithAlpha(CurrentPalette().link, 40), 2 * us);
                dl->AddRect(la, lb, WithAlpha(CurrentPalette().link, 160), 2 * us);
                break;
                }
            }
        }
    // The character under the pointer: the view's rotation is undone to ask the engine.
    int hoveredChar = -1;
    if (hoveredPage >= 0 && !hoveredLink && !p.dragging)
        {
        const pdf::RectF point = pdf::RotateNormalized({ hoveredNorm.x, hoveredNorm.y, 0, 0 }, (360 - p.view.rotation) % 360);
        // The press frame waits for the engine, so a click on text is never taken for a pan.
        hoveredChar = CharIndexAt(p, hoveredPage, point.x, point.y, activated);
        if (hoveredChar >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
    if (activated)
        {
        // A press on text starts a selection; anywhere else clears it and pans.
        p.selection.clear();
        p.selecting = false;
        if (hoveredChar >= 0 && !hoveredLink && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            {
            p.selecting = true;
            SetSelection(p, hoveredPage, hoveredChar, hoveredChar);
            }
        }
    if (p.selectionDirty && !p.selection.empty()) SetSelection(p, p.selection.page, p.selection.anchor, p.selection.focus);
    if (itemActive && p.selecting && hoveredPage == p.selection.page && hoveredChar >= 0) SetSelection(p, p.selection.page, p.selection.anchor, hoveredChar);
    if (p.selecting) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    if (hoveredLink && !p.dragging)
        {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const std::string tip = hoveredLink->page >= 0 ? "Go to page " + std::to_string(hoveredLink->page + 1) : hoveredLink->uri;
        ImGui::SetTooltip("%s", tip.c_str());
        }
    if (deactivated && !p.dragging && hoveredLink && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
        const pdf::LinkBox link = *hoveredLink;   // a copy: following it may drop the cache
        ActivateLink(ui, p, link);
        }
    if (deactivated)
        {
        p.dragging = false;
        p.selecting = false;
        // A plain click on text selects nothing.
        if (!p.selection.empty() && p.selection.count() <= 1 && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left, 4.0f)) p.selection.clear();
        }
    ImGui::EndChild();
}

void DrawPassword(PdfViewUi& p, ImVec2 size)
{
    ImGui::BeginChild("##password", size);
    const float w = std::min(size.x - 20.0f, ImGui::GetFontSize() * 20);
    ImGui::SetCursorPos(ImVec2((size.x - w) * 0.5f, size.y * 0.35f));
    ImGui::BeginGroup();
    ImGui::TextUnformatted(ICON_MD_LOCK " This document is protected by a password.");
    if (!p.error.empty())
        {
        ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().danger);
        ImGui::TextUnformatted(p.error.c_str());
        ImGui::PopStyleColor();
        }
    if (p.passwordWantFocus)
        {
        ImGui::SetKeyboardFocusHere();
        p.passwordWantFocus = false;
        }
    ImGui::SetNextItemWidth(w - ImGui::CalcTextSize("Open").x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x);
    const bool enter = ImGui::InputTextWithHint("##pw", "Password", &p.password, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    // The next draw loads the file again, with the password.
    if ((ImGui::Button("Open") || enter) && !p.password.empty()) p.loadedPath.clear();
    ImGui::EndGroup();
    ImGui::EndChild();
}

void DrawMessage(const std::string& text, ImVec2 size)
{
    ImGui::BeginChild("##message", size);
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str(), nullptr, false, size.x - 40.0f);
    ImGui::SetCursorPos(ImVec2((size.x - textSize.x) * 0.5f, (size.y - textSize.y) * 0.5f));
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textSize.x);
    ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().danger);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void DrawStatusBar(const PdfViewUi& p)
{
    std::string text;
    if (p.view.PageCount() > 0)
        text = std::to_string(p.view.PageCount()) + (p.view.PageCount() == 1 ? " page" : " pages");
    text += (text.empty() ? "" : "    ") + std::string("PDF document");
    if (p.fileBytes > 0) text += "    " + app::HumanSize(p.fileBytes);
    if (!p.status.empty()) text += "    " + p.status;
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    MutedText(text.c_str());
    if (p.renderPending)
        {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Rendering...").x));
        FaintText("Rendering...");
        }
}

} // namespace

PdfViewInfo DescribePdfView(const FileViewUi& v)
{
    PdfViewInfo info;
    if (!v.pdf) return info;
    const PdfViewUi& p = *v.pdf;
    info.pages = p.view.PageCount();
    info.currentPage = p.view.currentPage;
    for (const PdfViewUi::CachedPage& c : p.cache)
        if (c.texture && c.texture->Status == ImTextureStatus_OK) ++info.texturesReady;
    info.hits = (int)p.hits.size();
    info.needsPassword = p.needsPassword;
    info.error = p.error;
    return info;
}

void DrawPdfFile(app::AppState& state, UiState& ui, app::FileView& view, FileViewUi& v)
{
    if (!v.pdf) v.pdf = std::make_shared<PdfViewUi>();
    PdfViewUi& p = *v.pdf;
    if (p.loadedPath != view.path)
        {
        // A new file forgets the old one's password: it would not be its.
        if (!p.needsPassword) p.password.clear();
        Load(p, view.path);
        }
    p.renderPending = false;
    UploadResults(p);

    // Ctrl+F and Ctrl+C belong to this view while it has the focus; the
    // application's own Ctrl+F stays out of the way (see HandleGlobalShortcuts).
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && p.renderer && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) OpenFind(p);
    if (focused && p.renderer && !ImGui::GetIO().WantTextInput && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) CopyText(p);
    if (focused && p.renderer && (ImGui::IsKeyPressed(ImGuiKey_F3) || ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F3)))
        {
        if (!p.findOpen) OpenFind(p);
        else StepHit(p, ImGui::GetIO().KeyShift ? -1 : 1);
        }

    DrawToolbar(state, ui, view, p);
    if (p.findOpen && p.renderer) DrawFindBar(p);

    const float statusHeight = state.settings.showStatusBar ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2 : 0.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(avail.x, std::max(avail.y - statusHeight, 1.0f));
    if (p.needsPassword) DrawPassword(p, size);
    else if (!p.renderer) DrawMessage(p.error.empty() ? std::string("The document could not be opened.") : p.error, size);
    else DrawCanvas(ui, p, size);
    if (state.settings.showStatusBar) DrawStatusBar(p);

    FindPump(p);
    Evict(p);
    if (p.renderer)
        {
        p.renderer->Prune((uint64_t)ImGui::GetFrameCount(), p.generation, p.view.rotation);
        if (p.renderer->Pending() > 0 || !p.inbox.empty()) p.renderPending = true;
        }
    if (p.renderPending || (p.findOpen && p.findProgress < p.view.PageCount())) ui.animating = true;
}

} // namespace ui
