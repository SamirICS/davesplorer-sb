// A small PDF made in memory for the tests: two pages of different sizes,
// a text layer to search, a link to the second page and one to the web,
// and a two-entry outline. After davepdf's sample generator.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace testpdf
{

inline std::string Make()
{
    const std::vector<std::string> content = {
        "BT /F1 18 Tf 72 700 Td (Hello from davesplorer) Tj ET\n"
        "BT /F1 12 Tf 72 660 Td (The quick brown fox jumps over the lazy dog.) Tj ET\n"
        "BT /F1 12 Tf 72 600 Td (Go to page two) Tj ET\n",
        "BT /F1 18 Tf 72 500 Td (A landscape page) Tj ET\n"
        "BT /F1 12 Tf 72 460 Td (The fox appears twice in this document.) Tj ET\n",
    };
    // 1 catalog, 2 pages, 3 outlines, 4 font, 5-6 page 1, 7-8 page 2,
    // 9-10 outline items, 11 link to page 2, 12 web link.
    std::vector<std::string> objects(12);
    objects[0] = "<< /Type /Catalog /Pages 2 0 R /Outlines 3 0 R >>";
    objects[1] = "<< /Type /Pages /Kids [ 5 0 R 7 0 R ] /Count 2 >>";
    objects[2] = "<< /Type /Outlines /First 9 0 R /Last 10 0 R /Count 2 >>";
    objects[3] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
    objects[4] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 6 0 R /Resources << /Font << /F1 4 0 R >> >> /Annots [ 11 0 R 12 0 R ] >>";
    objects[6] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 842 595] /Contents 8 0 R /Resources << /Font << /F1 4 0 R >> >> >>";
    for (int i = 0; i < 2; ++i)
        {
        char head[64];
        std::snprintf(head, sizeof(head), "<< /Length %zu >>\nstream\n", content[(size_t)i].size());
        objects[(size_t)(5 + i * 2)] = std::string(head) + content[(size_t)i] + "\nendstream";
        }
    objects[8] = "<< /Title (Start) /Parent 3 0 R /Next 10 0 R /Dest [ 5 0 R /Fit ] >>";
    objects[9] = "<< /Title (Landscape) /Parent 3 0 R /Prev 9 0 R /Dest [ 7 0 R /Fit ] >>";
    objects[10] = "<< /Type /Annot /Subtype /Link /Rect [ 72 595 200 615 ] /Border [ 0 0 0 ] /Dest [ 7 0 R /Fit ] >>";
    objects[11] = "<< /Type /Annot /Subtype /Link /Rect [ 72 560 200 580 ] /Border [ 0 0 0 ] /A << /S /URI /URI (https://example.com/) >> >>";

    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets;
    char buffer[128];
    for (size_t i = 0; i < objects.size(); ++i)
        {
        offsets.push_back(out.size());
        std::snprintf(buffer, sizeof(buffer), "%zu 0 obj\n", i + 1);
        out += buffer;
        out += objects[i];
        out += "\nendobj\n";
        }
    const size_t xref = out.size();
    std::snprintf(buffer, sizeof(buffer), "xref\n0 %zu\n0000000000 65535 f \n", objects.size() + 1);
    out += buffer;
    for (size_t offset : offsets)
        {
        std::snprintf(buffer, sizeof(buffer), "%010zu 00000 n \n", offset);
        out += buffer;
        }
    std::snprintf(buffer, sizeof(buffer), "trailer\n<< /Size %zu /Root 1 0 R >>\nstartxref\n%zu\n%%%%EOF\n", objects.size() + 1, xref);
    out += buffer;
    return out;
}

} // namespace testpdf
