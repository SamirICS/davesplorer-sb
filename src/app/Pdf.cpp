#include "app/Pdf.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace app::pdf
{

namespace
{

constexpr float kPresets[] = { 0.25f, 0.33f, 0.5f, 0.67f, 0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f,
                               2.0f, 2.5f, 3.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f };

} // namespace

bool IsPdf(std::string_view extension)
{
    return extension == "pdf";
}

float ClampZoom(float zoom)
{
    if (!std::isfinite(zoom) || zoom <= 0) return 1.0f;
    return std::clamp(zoom, kMinZoom, kMaxZoom);
}

float ZoomStep(float zoom, bool in)
{
    // A little slack so a fitted 0.9999 steps to 1.1 rather than to 1.0.
    const float eps = 0.005f;
    if (in)
        {
        for (float preset : kPresets)
            if (preset > zoom + eps) return preset;
        return kMaxZoom;
        }
    for (int i = (int)std::size(kPresets) - 1; i >= 0; --i)
        if (kPresets[(size_t)i] < zoom - eps) return kPresets[(size_t)i];
    return kMinZoom;
}

PageSize ViewState::Rotated(int page) const
{
    PageSize size = pages[(size_t)page];
    if (rotation == 90 || rotation == 270) std::swap(size.width, size.height);
    return size;
}

void ViewState::Rotate(int quarterTurns)
{
    rotation = ((rotation + quarterTurns * 90) % 360 + 360) % 360;
}

Layout LayoutPages(const ViewState& view, float pixelScale)
{
    Layout layout;
    const float k = kPointsToPixels * view.zoom * pixelScale;
    float y = view.margin;
    float widest = 0;
    for (int i = 0; i < view.PageCount(); ++i)
        {
        const PageSize size = view.Rotated(i);
        // Whole pixels: a page is rendered at exactly this size, and a
        // fractional one would be resampled and blur the text.
        const float w = std::max(1.0f, std::round(size.width * k));
        const float h = std::max(1.0f, std::round(size.height * k));
        layout.pages.push_back({ i, { 0, y, w, h } });
        widest = std::max(widest, w);
        y += h + view.gap;
        }
    layout.width = widest + 2 * view.margin;
    layout.height = view.PageCount() ? y - view.gap + view.margin : 0;
    for (PageRect& page : layout.pages) page.rect.x = std::round((layout.width - page.rect.w) / 2);
    return layout;
}

float FitZoom(const ViewState& view, float viewW, float viewH, float pixelScale, FitMode mode)
{
    if (mode == FitMode::Manual || view.PageCount() == 0) return view.zoom;
    const float k = kPointsToPixels * pixelScale;
    const float usableW = std::max(32.0f, viewW - 2 * view.margin);
    if (mode == FitMode::Width)
        {
        float widest = 0;
        for (int i = 0; i < view.PageCount(); ++i) widest = std::max(widest, view.Rotated(i).width);
        return widest > 0 ? ClampZoom(usableW / (widest * k)) : 1.0f;
        }
    const int page = std::clamp(view.currentPage, 0, view.PageCount() - 1);
    const PageSize size = view.Rotated(page);
    if (size.width <= 0 || size.height <= 0) return 1.0f;
    const float usableH = std::max(32.0f, viewH - 2 * view.margin);
    return ClampZoom(std::min(usableW / (size.width * k), usableH / (size.height * k)));
}

int PageAt(const Layout& layout, float scrollY, float viewH)
{
    if (layout.pages.empty()) return 0;
    const float top = scrollY, bottom = scrollY + viewH;
    int best = -1;
    float bestOverlap = 0;
    for (const PageRect& page : layout.pages)
        {
        const float overlap = std::min(bottom, page.rect.y + page.rect.h) - std::max(top, page.rect.y);
        if (overlap > bestOverlap)
            {
            bestOverlap = overlap;
            best = page.page;
            }
        }
    if (best >= 0) return best;
    // Nothing overlaps (the view sits in a gap): the nearest edge wins.
    float bestDistance = 1e30f;
    for (const PageRect& page : layout.pages)
        {
        const float distance = std::min(std::fabs(page.rect.y - bottom), std::fabs(page.rect.y + page.rect.h - top));
        if (distance < bestDistance)
            {
            bestDistance = distance;
            best = page.page;
            }
        }
    return best;
}

float ScrollToPage(const Layout& layout, int page, float gap)
{
    if (page < 0 || page >= (int)layout.pages.size()) return 0;
    return std::max(0.0f, layout.pages[(size_t)page].rect.y - gap);
}

std::vector<int> VisiblePages(const Layout& layout, float scrollY, float viewH, int overscan)
{
    std::vector<int> result;
    const int count = (int)layout.pages.size();
    int first = -1, last = -1;
    for (int i = 0; i < count; ++i)
        {
        const RectF& r = layout.pages[(size_t)i].rect;
        if (r.y + r.h >= scrollY && r.y <= scrollY + viewH)
            {
            if (first < 0) first = i;
            last = i;
            }
        }
    if (first < 0) return result;
    for (int i = std::max(0, first - overscan); i <= std::min(count - 1, last + overscan); ++i) result.push_back(i);
    return result;
}

float ClampScroll(float scroll, float content, float view)
{
    if (!std::isfinite(scroll)) return 0;
    return std::clamp(scroll, 0.0f, std::max(0.0f, content - view));
}

float ScrollAfterZoom(float scroll, float cursorOffset, float oldZoom, float newZoom)
{
    if (oldZoom <= 0) return scroll;
    return (scroll + cursorOffset) * (newZoom / oldZoom) - cursorOffset;
}

RectF RotateNormalized(RectF r, int rotation)
{
    switch (((rotation % 360) + 360) % 360)
        {
        case 90:  return { 1 - (r.y + r.h), r.x, r.h, r.w };
        case 180: return { 1 - (r.x + r.w), 1 - (r.y + r.h), r.w, r.h };
        case 270: return { r.y, 1 - (r.x + r.w), r.h, r.w };
        default:  return r;
        }
}

} // namespace app::pdf
