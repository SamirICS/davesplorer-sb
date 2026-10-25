// PDF files for the built-in viewer: what the engine hands back (page sizes,
// links, search hits) as plain structs, and the arithmetic of a
// continuously scrolled stack of pages. No ImGui and no PDFium here, so the
// tests can check the geometry without either.
//
// The geometry is davepdf's (github.com/evaderkrub/davepdf, MIT), renamed
// to this code base's conventions.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace app::pdf
{

bool IsPdf(std::string_view extension);

struct RectF { float x = 0, y = 0, w = 0, h = 0; };
struct PageSize { float width = 0, height = 0; };   // PDF points, 1/72 inch

struct OutlineItem
{
    std::string title;
    int         page = -1;   // -1 when the bookmark has no page destination
    int         depth = 0;
};

struct LinkBox
{
    RectF       rect;        // normalised to the unrotated page (0..1, top-left origin)
    int         page = -1;   // an internal destination, or
    std::string uri;         // an external one
};

struct SearchHit
{
    int                page = 0;
    int                start = 0, count = 0;   // character range on the page
    std::vector<RectF> rects;                  // normalised to the unrotated page
};

// A run of characters on one page, chosen with the mouse.
struct TextSelection
{
    int                page = -1;
    int                anchor = -1;   // character where the drag started
    int                focus = -1;    // character under the pointer
    std::vector<RectF> rects;         // normalised to the unrotated page

    bool empty() const { return page < 0 || anchor < 0 || focus < 0; }
    int  first() const { return anchor < focus ? anchor : focus; }
    int  count() const { return (anchor < focus ? focus - anchor : anchor - focus) + 1; }
    void clear() { page = anchor = focus = -1; rects.clear(); }
};

enum class FitMode { Manual, Width, Page };

// 100% shows a page at its printed size on a 96 dpi screen, which is what
// every mainstream viewer calls "actual size".
inline constexpr float kPointsToPixels = 96.0f / 72.0f;
inline constexpr float kMinZoom = 0.1f;
inline constexpr float kMaxZoom = 16.0f;

float ClampZoom(float zoom);
// The next preset above (in) or below (out) the current zoom.
float ZoomStep(float zoom, bool in);

struct ViewState
{
    std::vector<PageSize> pages;
    float   zoom = 1.0f;
    FitMode fit = FitMode::Width;
    int     rotation = 0;       // 0, 90, 180, 270 clockwise, for every page
    int     currentPage = 0;    // zero based
    float   gap = 14.0f;        // between pages, pixels
    float   margin = 24.0f;     // around the stack, pixels

    int      PageCount() const { return (int)pages.size(); }
    bool     ValidPage(int page) const { return page >= 0 && page < PageCount(); }
    PageSize Rotated(int page) const;
    void     Rotate(int quarterTurns);
};

struct PageRect { int page = 0; RectF rect; };

// Document space: pixels, origin at the top left of the whole stack.
struct Layout
{
    std::vector<PageRect> pages;
    float width = 0, height = 0;
};

Layout LayoutPages(const ViewState& view, float pixelScale);
// The zoom that fits the widest page (Width) or the current page (Page) in
// a viewport of this size. Manual returns the zoom unchanged.
float FitZoom(const ViewState& view, float viewW, float viewH, float pixelScale, FitMode mode);
// The page a reader would call current: the one covering most of the view.
int   PageAt(const Layout& layout, float scrollY, float viewH);
float ScrollToPage(const Layout& layout, int page, float gap);
std::vector<int> VisiblePages(const Layout& layout, float scrollY, float viewH, int overscan);
float ClampScroll(float scroll, float content, float view);
// Zooming about a point keeps the document position under it fixed.
float ScrollAfterZoom(float scroll, float cursorOffset, float oldZoom, float newZoom);

// Boxes come from the engine normalised to the unrotated page; turning the
// view turns them the same way.
RectF RotateNormalized(RectF r, int rotation);

} // namespace app::pdf
