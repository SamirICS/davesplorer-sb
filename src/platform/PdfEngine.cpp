#include "platform/PdfEngine.h"

#include <fpdf_doc.h>
#include <fpdf_text.h>
#include <fpdfview.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

namespace platform
{

namespace
{

using app::pdf::RectF;

// Initialised on the first document, and left up for the life of the
// process: tearing it down while another view still has a document open
// would pull it from under that one.
void EnsureLibrary()
{
    static bool initialised = false;
    if (initialised) return;
    FPDF_LIBRARY_CONFIG config{};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    initialised = true;
}

// PDFium speaks UTF-16LE with a terminating null; this program, UTF-8.
std::string Utf16ToUtf8(const unsigned short* text, size_t units)
{
    std::string out;
    out.reserve(units);
    for (size_t i = 0; i < units; ++i)
        {
        unsigned int c = text[i];
        if (c == 0) break;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
            {
            c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
            ++i;
            }
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        }
    return out;
}

std::vector<unsigned short> Utf8ToUtf16(const std::string& text)
{
    std::vector<unsigned short> out;
    for (size_t i = 0; i < text.size();)
        {
        const unsigned char b = (unsigned char)text[i];
        unsigned int c = b;
        size_t extra = 0;
        if (b >= 0xF0) { c = b & 0x07; extra = 3; }
        else if (b >= 0xE0) { c = b & 0x0F; extra = 2; }
        else if (b >= 0xC0) { c = b & 0x1F; extra = 1; }
        if (i + extra >= text.size()) { extra = 0; c = b; }
        for (size_t k = 1; k <= extra; ++k) c = (c << 6) | ((unsigned char)text[i + k] & 0x3F);
        i += extra + 1;
        if (c >= 0x10000)
            {
            c -= 0x10000;
            out.push_back((unsigned short)(0xD800 + (c >> 10)));
            out.push_back((unsigned short)(0xDC00 + (c & 0x3FF)));
            }
        else
            {
            out.push_back((unsigned short)c);
            }
        }
    out.push_back(0);
    return out;
}

// PDFium's "get text" calls: once for the length, once for the text.
template <class Getter>
std::string ReadWide(Getter get)
{
    const unsigned long bytes = get(nullptr, 0);
    if (bytes <= 2) return {};
    std::vector<unsigned short> buffer(bytes / 2 + 1, 0);
    get(buffer.data(), bytes);
    return Utf16ToUtf8(buffer.data(), bytes / 2);
}

std::string DescribeError(unsigned long code)
{
    switch (code)
        {
        case FPDF_ERR_FILE:     return "The file could not be opened.";
        case FPDF_ERR_FORMAT:   return "This is not a PDF file, or the file is damaged.";
        case FPDF_ERR_PASSWORD: return "This document needs a password.";
        case FPDF_ERR_SECURITY: return "The document uses a security scheme this viewer does not support.";
        case FPDF_ERR_PAGE:     return "A page in the document could not be read.";
        default:                return "PDFium could not open the document (error " + std::to_string(code) + ").";
        }
}

// A fixed device grid over the unrotated page, so boxes come back
// resolution independent; the view scales them to whatever it draws.
constexpr int kGrid = 4096;

RectF Normalize(FPDF_PAGE page, double left, double top, double right, double bottom)
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    FPDF_PageToDevice(page, 0, 0, kGrid, kGrid, 0, left, top, &x1, &y1);
    FPDF_PageToDevice(page, 0, 0, kGrid, kGrid, 0, right, bottom, &x2, &y2);
    RectF r;
    r.x = (float)std::min(x1, x2) / kGrid;
    r.y = (float)std::min(y1, y2) / kGrid;
    r.w = (float)std::abs(x2 - x1) / kGrid;
    r.h = (float)std::abs(y2 - y1) / kGrid;
    return r;
}

void WalkOutline(FPDF_DOCUMENT doc, FPDF_BOOKMARK bookmark, int depth, std::vector<app::pdf::OutlineItem>& out)
{
    for (; bookmark && out.size() < 20000; bookmark = FPDFBookmark_GetNextSibling(doc, bookmark))
        {
        app::pdf::OutlineItem item;
        item.depth = depth;
        item.title = ReadWide([&](void* buffer, unsigned long length) { return FPDFBookmark_GetTitle(bookmark, buffer, length); });
        FPDF_DEST dest = FPDFBookmark_GetDest(doc, bookmark);
        if (!dest)
            {
            FPDF_ACTION action = FPDFBookmark_GetAction(bookmark);
            if (action && FPDFAction_GetType(action) == PDFACTION_GOTO) dest = FPDFAction_GetDest(doc, action);
            }
        if (dest) item.page = FPDFDest_GetDestPageIndex(doc, dest);
        out.push_back(item);
        // A damaged file can nest bookmarks in a loop; the depth cap ends it.
        if (depth < 32) WalkOutline(doc, FPDFBookmark_GetFirstChild(doc, bookmark), depth + 1, out);
        }
}

} // namespace

std::mutex& PdfLock()
{
    static std::mutex lock;
    return lock;
}

const char* PdfiumVersion()
{
#ifdef DSP_PDFIUM_VERSION
    return DSP_PDFIUM_VERSION;
#else
    return "unknown";
#endif
}

struct PdfDocument::Impl
{
    std::string   bytes;
    FPDF_DOCUMENT doc = nullptr;
    bool          needsPassword = false;
    // Pages stay loaded while they are drawn, searched or hovered; a small
    // cap keeps a thousand-page document's memory bounded.
    std::map<int, FPDF_PAGE>     pages;
    std::map<int, FPDF_TEXTPAGE> textPages;

    void DropPages()
    {
        for (auto& [index, text] : textPages) FPDFText_ClosePage(text);
        textPages.clear();
        for (auto& [index, page] : pages) FPDF_ClosePage(page);
        pages.clear();
    }

    FPDF_PAGE Page(int index)
    {
        if (!doc || index < 0 || index >= FPDF_GetPageCount(doc)) return nullptr;
        const auto it = pages.find(index);
        if (it != pages.end()) return it->second;
        if (pages.size() >= 16) DropPages();
        FPDF_PAGE p = FPDF_LoadPage(doc, index);
        if (p) pages[index] = p;
        return p;
    }

    FPDF_TEXTPAGE TextPage(int index)
    {
        const auto it = textPages.find(index);
        if (it != textPages.end()) return it->second;
        FPDF_PAGE p = Page(index);
        if (!p) return nullptr;
        FPDF_TEXTPAGE t = FPDFText_LoadPage(p);
        if (t) textPages[index] = t;
        return t;
    }
};

PdfDocument::PdfDocument() : impl_(std::make_unique<Impl>()) {}

PdfDocument::~PdfDocument()
{
    std::lock_guard<std::mutex> lock(PdfLock());
    Close();
}

bool PdfDocument::LoadFile(const std::string& path, const std::string& password, std::string& error)
{
    std::ifstream stream(std::filesystem::path(std::u8string(path.begin(), path.end())), std::ios::binary | std::ios::ate);
    if (!stream)
        {
        error = "The file could not be opened.";
        return false;
        }
    const auto length = stream.tellg();
    if (length < 0 || length > (1ll << 31))
        {
        error = length < 0 ? "The file could not be read." : "The file is too large to open (2 GB limit).";
        return false;
        }
    std::string bytes((size_t)length, '\0');
    stream.seekg(0);
    if (length > 0) stream.read(bytes.data(), length);
    if (!stream)
        {
        error = "The file could not be read.";
        return false;
        }
    return LoadMemory(std::move(bytes), password, error);
}

bool PdfDocument::LoadMemory(std::string bytes, const std::string& password, std::string& error)
{
    Close();
    EnsureLibrary();
    impl_->bytes = std::move(bytes);
    impl_->doc = FPDF_LoadMemDocument64(impl_->bytes.data(), impl_->bytes.size(), password.empty() ? nullptr : password.c_str());
    if (!impl_->doc)
        {
        const unsigned long code = FPDF_GetLastError();
        impl_->needsPassword = code == FPDF_ERR_PASSWORD;
        error = DescribeError(code);
        impl_->bytes.clear();
        return false;
        }
    impl_->needsPassword = false;
    error.clear();
    return true;
}

void PdfDocument::Close()
{
    impl_->DropPages();
    if (impl_->doc) FPDF_CloseDocument(impl_->doc);
    impl_->doc = nullptr;
    impl_->bytes.clear();
    impl_->bytes.shrink_to_fit();
}

bool PdfDocument::IsOpen() const { return impl_->doc != nullptr; }
bool PdfDocument::NeedsPassword() const { return impl_->needsPassword; }
int  PdfDocument::PageCount() const { return impl_->doc ? FPDF_GetPageCount(impl_->doc) : 0; }

std::vector<app::pdf::PageSize> PdfDocument::PageSizes() const
{
    std::vector<app::pdf::PageSize> sizes;
    const int count = PageCount();
    for (int i = 0; i < count; ++i)
        {
        FS_SIZEF size{};
        app::pdf::PageSize page{ 612, 792 };   // US Letter stands in for a page PDFium cannot measure
        if (FPDF_GetPageSizeByIndexF(impl_->doc, i, &size) && size.width > 0 && size.height > 0) page = { size.width, size.height };
        sizes.push_back(page);
        }
    return sizes;
}

bool PdfDocument::Render(int index, int width, int height, int rotation, std::vector<unsigned char>& rgba, std::string& error)
{
    FPDF_PAGE page = impl_->Page(index);
    if (!page)
        {
        error = "The page could not be loaded.";
        return false;
        }
    if (width < 1 || height < 1)
        {
        error = "Invalid render size.";
        return false;
        }
    rgba.assign((size_t)width * (size_t)height * 4, 0xFF);
    FPDF_BITMAP bitmap = FPDFBitmap_CreateEx(width, height, FPDFBitmap_BGRA, rgba.data(), width * 4);
    if (!bitmap)
        {
        error = "Out of memory while rendering.";
        return false;
        }
    FPDFBitmap_FillRect(bitmap, 0, 0, width, height, 0xFFFFFFFFu);
    // FPDF_REVERSE_BYTE_ORDER makes the BGRA bitmap RGBA, the order ImGui's
    // textures take.
    FPDF_RenderPageBitmap(bitmap, page, 0, 0, width, height, ((rotation / 90) % 4 + 4) % 4, FPDF_ANNOT | FPDF_REVERSE_BYTE_ORDER);
    FPDFBitmap_Destroy(bitmap);
    return true;
}

std::vector<app::pdf::OutlineItem> PdfDocument::Outline() const
{
    std::vector<app::pdf::OutlineItem> items;
    if (impl_->doc) WalkOutline(impl_->doc, FPDFBookmark_GetFirstChild(impl_->doc, nullptr), 0, items);
    return items;
}

std::vector<app::pdf::LinkBox> PdfDocument::Links(int index)
{
    std::vector<app::pdf::LinkBox> boxes;
    FPDF_PAGE page = impl_->Page(index);
    if (!page) return boxes;
    int position = 0;
    FPDF_LINK link = nullptr;
    while (FPDFLink_Enumerate(page, &position, &link) && boxes.size() < 10000)
        {
        FS_RECTF rect{};
        if (!FPDFLink_GetAnnotRect(link, &rect)) continue;
        app::pdf::LinkBox box;
        box.rect = Normalize(page, rect.left, rect.top, rect.right, rect.bottom);
        FPDF_DEST dest = FPDFLink_GetDest(impl_->doc, link);
        if (!dest)
            {
            FPDF_ACTION action = FPDFLink_GetAction(link);
            if (action)
                {
                const unsigned long type = FPDFAction_GetType(action);
                if (type == PDFACTION_GOTO)
                    {
                    dest = FPDFAction_GetDest(impl_->doc, action);
                    }
                else if (type == PDFACTION_URI)
                    {
                    const unsigned long length = FPDFAction_GetURIPath(impl_->doc, action, nullptr, 0);
                    if (length > 1)
                        {
                        std::string uri(length, '\0');
                        FPDFAction_GetURIPath(impl_->doc, action, uri.data(), length);
                        uri.resize(strnlen(uri.c_str(), length));
                        box.uri = uri;
                        }
                    }
                }
            }
        if (dest) box.page = FPDFDest_GetDestPageIndex(impl_->doc, dest);
        if (box.page >= 0 || !box.uri.empty()) boxes.push_back(box);
        }
    return boxes;
}

bool PdfDocument::Search(int index, const std::string& needle, bool matchCase, std::vector<app::pdf::SearchHit>& out)
{
    if (needle.empty()) return true;
    FPDF_TEXTPAGE text = impl_->TextPage(index);
    FPDF_PAGE page = impl_->Page(index);
    if (!text || !page) return false;
    const std::vector<unsigned short> wide = Utf8ToUtf16(needle);
    FPDF_SCHHANDLE handle = FPDFText_FindStart(text, wide.data(), matchCase ? FPDF_MATCHCASE : 0, 0);
    if (!handle) return false;
    while (FPDFText_FindNext(handle))
        {
        app::pdf::SearchHit hit;
        hit.page = index;
        hit.start = FPDFText_GetSchResultIndex(handle);
        hit.count = FPDFText_GetSchCount(handle);
        const int rects = FPDFText_CountRects(text, hit.start, hit.count);
        for (int i = 0; i < rects; ++i)
            {
            double left = 0, top = 0, right = 0, bottom = 0;
            if (FPDFText_GetRect(text, i, &left, &top, &right, &bottom)) hit.rects.push_back(Normalize(page, left, top, right, bottom));
            }
        if (!hit.rects.empty()) out.push_back(std::move(hit));
        }
    FPDFText_FindClose(handle);
    return true;
}

int PdfDocument::CharIndexAt(int index, float nx, float ny)
{
    FPDF_TEXTPAGE text = impl_->TextPage(index);
    FPDF_PAGE page = impl_->Page(index);
    if (!text || !page) return -1;
    double px = 0, py = 0;
    if (!FPDF_DeviceToPage(page, 0, 0, kGrid, kGrid, 0, (int)(nx * kGrid), (int)(ny * kGrid), &px, &py)) return -1;
    // A few points of slack, so a drag starting just beside a glyph takes it.
    const int found = FPDFText_GetCharIndexAtPos(text, px, py, 3.0, 3.0);
    return found >= 0 ? found : -1;
}

std::vector<RectF> PdfDocument::TextRects(int index, int start, int count)
{
    std::vector<RectF> rects;
    FPDF_TEXTPAGE text = impl_->TextPage(index);
    FPDF_PAGE page = impl_->Page(index);
    if (!text || !page || count <= 0) return rects;
    const int n = FPDFText_CountRects(text, start, count);
    for (int i = 0; i < n; ++i)
        {
        double left = 0, top = 0, right = 0, bottom = 0;
        if (FPDFText_GetRect(text, i, &left, &top, &right, &bottom)) rects.push_back(Normalize(page, left, top, right, bottom));
        }
    return rects;
}

std::string PdfDocument::TextRange(int index, int start, int count)
{
    FPDF_TEXTPAGE text = impl_->TextPage(index);
    if (!text || count <= 0) return {};
    std::vector<unsigned short> buffer((size_t)count + 1, 0);
    const int written = FPDFText_GetText(text, start, count, buffer.data());
    return Utf16ToUtf8(buffer.data(), written > 0 ? (size_t)written : 0);
}

std::string PdfDocument::PageText(int index)
{
    FPDF_TEXTPAGE text = impl_->TextPage(index);
    if (!text) return {};
    return TextRange(index, 0, std::max(0, FPDFText_CountChars(text)));
}

// ---------------------------------------------------------------------------

PdfRenderer::PdfRenderer(PdfDocument& document) : doc_(document), thread_([this] { Run(); }) {}

PdfRenderer::~PdfRenderer()
{
    {
    std::lock_guard<std::mutex> lock(mutex_);
    quit_ = true;
    queue_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool PdfRenderer::Submit(const PdfRenderJob& job)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ && inFlight_.SameImage(job)) return false;
    for (const PdfRenderResult& r : done_)
        if (r.job.SameImage(job)) return false;   // finished, not yet collected
    for (PdfRenderJob& queued : queue_)
        if (queued.SameImage(job))
            {
            queued.priority = std::min(queued.priority, job.priority);
            queued.requested = job.requested;
            return false;
            }
    queue_.push_back(job);
    wake_.notify_one();
    return true;
}

void PdfRenderer::Prune(uint64_t frame, unsigned generation, int rotation)
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [&](const PdfRenderJob& j) {
                     return j.requested + 2 < frame || j.generation != generation || j.rotation != rotation;
                 }),
                 queue_.end());
}

void PdfRenderer::CancelAll()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    done_.clear();
}

std::vector<PdfRenderResult> PdfRenderer::Collect()
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PdfRenderResult> out;
    out.swap(done_);
    return out;
}

bool PdfRenderer::IsQueued(const PdfRenderJob& job) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (busy_ && inFlight_.SameImage(job)) return true;
    for (const PdfRenderJob& queued : queue_)
        if (queued.SameImage(job)) return true;
    return false;
}

size_t PdfRenderer::Pending() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + (busy_ ? 1 : 0);
}

void PdfRenderer::Run()
{
    for (;;)
        {
        PdfRenderJob job;
        {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return quit_ || !queue_.empty(); });
        if (quit_) return;
        // Lowest priority number first; ties go to the latest request.
        const auto best = std::min_element(queue_.begin(), queue_.end(), [](const PdfRenderJob& a, const PdfRenderJob& b) {
            return a.priority != b.priority ? a.priority < b.priority : a.requested > b.requested;
        });
        job = *best;
        queue_.erase(best);
        inFlight_ = job;
        busy_ = true;
        }
        PdfRenderResult result;
        result.job = job;
        {
        std::string error;
        std::lock_guard<std::mutex> pdfLock(PdfLock());
        result.ok = doc_.IsOpen() && doc_.Render(job.page, job.width, job.height, job.rotation, result.rgba, error);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (result.ok) done_.push_back(std::move(result));
        busy_ = false;
        }
}

} // namespace platform
