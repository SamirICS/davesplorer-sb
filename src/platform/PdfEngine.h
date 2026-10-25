// The one place PDFium is spoken to, and the thread that renders its
// pages. Everything comes back as app::pdf structs, so nothing else
// includes an fpdf header.
//
// Ported from davepdf (github.com/evaderkrub/davepdf, MIT). The difference
// is the lock: davepdf has one document, a file manager may have several
// open, and PDFium is not thread safe across documents either, so every
// call from any thread holds the one PdfLock(). The methods do not take it
// themselves, so a caller can group several calls under one lock, or
// try_lock and come back next frame while a render holds it.
#pragma once

#include "app/Pdf.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platform
{

std::mutex& PdfLock();
const char* PdfiumVersion();

class PdfDocument
{
public:
    PdfDocument();
    ~PdfDocument();
    PdfDocument(const PdfDocument&) = delete;
    PdfDocument& operator=(const PdfDocument&) = delete;

    // The bytes stay in memory for the document's lifetime, which keeps
    // PDFium off the file (and off Windows' narrow-path file API).
    bool LoadFile(const std::string& path, const std::string& password, std::string& error);
    bool LoadMemory(std::string bytes, const std::string& password, std::string& error);
    void Close();
    bool IsOpen() const;
    // After a failed load that the right password would fix.
    bool NeedsPassword() const;

    int PageCount() const;
    std::vector<app::pdf::PageSize> PageSizes() const;
    // A page on white at exactly width x height, turned 0/90/180/270
    // degrees clockwise: width * height * 4 bytes of RGBA.
    bool Render(int page, int width, int height, int rotation, std::vector<unsigned char>& rgba, std::string& error);
    std::vector<app::pdf::OutlineItem> Outline() const;
    std::vector<app::pdf::LinkBox> Links(int page);
    // Appends every match on the page, in reading order.
    bool Search(int page, const std::string& utf8, bool matchCase, std::vector<app::pdf::SearchHit>& out);
    // Selection. Positions are normalised to the unrotated page; -1 when
    // no character is near the point.
    int CharIndexAt(int page, float nx, float ny);
    std::vector<app::pdf::RectF> TextRects(int page, int start, int count);
    std::string TextRange(int page, int start, int count);
    std::string PageText(int page);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PdfRenderJob
{
    int      page = 0, width = 0, height = 0, rotation = 0;
    unsigned generation = 0;
    int      priority = 0;        // lower runs first
    uint64_t requested = 0;       // frame of the latest request

    bool SameImage(const PdfRenderJob& o) const
    {
        return page == o.page && width == o.width && height == o.height && rotation == o.rotation && generation == o.generation;
    }
};

struct PdfRenderResult
{
    PdfRenderJob               job;
    std::vector<unsigned char> rgba;
    bool                       ok = false;
};

// One background thread per open PDF, turning render requests into pixels
// so the window keeps responding while a heavy page draws.
class PdfRenderer
{
public:
    explicit PdfRenderer(PdfDocument& document);
    ~PdfRenderer();
    PdfRenderer(const PdfRenderer&) = delete;
    PdfRenderer& operator=(const PdfRenderer&) = delete;

    // Queues a job, or refreshes the priority of the same one already
    // waiting. False when it is already queued, being rendered, or done
    // and waiting to be collected.
    bool Submit(const PdfRenderJob& job);
    // Drops waiting jobs nobody asked for since `frame`, and any of another
    // generation or rotation. The one being rendered always finishes.
    void Prune(uint64_t frame, unsigned generation, int rotation);
    void CancelAll();
    // Finished renders, oldest first.
    std::vector<PdfRenderResult> Collect();
    bool   IsQueued(const PdfRenderJob& job) const;
    size_t Pending() const;   // waiting plus in flight

private:
    void Run();

    PdfDocument&                 doc_;
    mutable std::mutex           mutex_;
    std::condition_variable      wake_;
    std::vector<PdfRenderJob>    queue_;
    std::vector<PdfRenderResult> done_;
    PdfRenderJob                 inFlight_;
    bool                         busy_ = false;
    bool                         quit_ = false;
    std::thread                  thread_;
};

} // namespace platform
