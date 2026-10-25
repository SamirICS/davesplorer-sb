// The Markdown viewer: a file view showing a .md file rendered the way a
// code host shows a README -- headings, wrapped paragraphs with emphasis
// and inline code, lists and task lists, quotes, code blocks, tables, the
// file's own pictures, and links that go somewhere: to a heading, to
// another Markdown file (in this view, with Back), to a web page. Find
// lights up every match; Source shows the text itself, to select and copy.
//
// Layout is done here rather than with ImGui widgets, because text that
// changes font mid-line and wraps around pictures is not something
// TextWrapped can do. Each block is laid out once per width and the
// result kept; a frame only draws the blocks in view.
#include "ui/MainWindow.h"
#include "ui/Fonts.h"
#include "ui/IconsMaterialDesign.h"
#include "ui/Textures.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"

#include "app/Format.h"
#include "app/Image.h"
#include "app/Markdown.h"
#include "app/Navigation.h"
#include "app/PathUtil.h"
#include "platform/FileSystem.h"
#include "platform/Shell.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace ui
{

namespace md = app::md;

namespace
{

// A word, a space, a line break or a picture, pointing into the text of
// the run it belongs to.
struct Token
{
    enum class Kind : uint8_t { Word, Space, Break, Image };
    Kind     kind = Kind::Word;
    uint8_t  style = 0;
    int      link = -1;
    int      image = -1;
    uint32_t start = 0, length = 0;   // in the run's text
};

// The inline content of one paragraph, heading or table cell, split into
// tokens once per document. `text` is what find searches: the tokens'
// text end to end, a soft break as a space. A code block's run is its
// code, with no tokens.
struct TokenRun
{
    std::string        text;
    std::vector<Token> tokens;
};

// A piece of a token placed on a line, relative to its block's top left.
// A word too long for a line is cut, so a piece's text can be part of
// its token's.
struct Placed
{
    int      token = 0;
    uint32_t start = 0, length = 0;   // in the run's text
    float    x = 0, y = 0, w = 0, h = 0;
    ImFont*  font = nullptr;
    float    size = 0;
    bool     runStart = false, runEnd = false;   // the ends of an inline-code run, which get padding
};

struct Flow
{
    int                 run = -1;
    std::vector<Placed> items;
    float               width = 0, height = 0;
    float               firstLineCenter = 0;   // where a list marker sits
};

struct BlockLayout
{
    float y = 0, h = 0;          // content space; h includes the space after
    float x = 0, w = 0;          // the box the block's content sits in
    float top = 0;               // space before the content, inside h
    Flow  flow;                  // Paragraph, Heading

    // Table: column and row positions relative to the table, a flow per cell.
    std::vector<float>             colX, colW, rowY, rowH;
    std::vector<std::vector<Flow>> cells;
    float                          tableW = 0, tableH = 0;

    // Code: the box, and where each line starts in the code.
    float               codeW = 0, codeH = 0, lineH = 0;
    std::vector<size_t> lineStarts;
};

struct Picture
{
    std::string    path;         // empty for a web picture: never fetched
    bool           tried = false;
    ImTextureData* texture = nullptr;
    int            width = 0, height = 0;
};

struct Hit
{
    int      run = -1;
    uint32_t start = 0, length = 0;
};

} // namespace

struct MarkdownViewUi
{
    std::string   loadedPath;
    std::string   source;
    md::Document  doc;
    std::string   error;
    uint64_t      fileBytes = 0;
    std::filesystem::file_time_type modified{};
    double        lastCheck = 0;        // when the file was last looked at for changes

    std::vector<TokenRun>                      runs;
    std::vector<int>                           blockRun;   // per block, -1 for none
    std::vector<std::vector<std::vector<int>>> cellRun;    // per block, per row, per cell
    std::vector<Picture>                       pictures;   // per doc.images entry

    std::vector<BlockLayout> layout;
    float layoutWidth = -1, layoutFont = -1;
    float totalHeight = 0;

    bool showSource = false;
    int  hoveredLink = -1;              // last frame's, so every word of it underlines
    int  pressedLink = -1;
    std::string contextLink;            // the link a right-click was on

    // Going back after following a link to another Markdown file.
    struct Visit { std::string path; float scroll = 0; };
    std::vector<Visit> history;

    std::string scrollToAnchor;         // a heading to bring to the top next frame
    float       scrollTo = -1;          // or a position
    float       scrollY = 0;

    bool             findOpen = false;
    bool             findWantFocus = false;
    std::string      findText;
    std::string      findDoneFor;       // the text `hits` is for
    std::vector<Hit> hits;
    int              currentHit = -1;
    bool             scrollToHit = false;

    ~MarkdownViewUi()
    {
        for (Picture& pic : pictures) textures::Release(pic.texture);
    }
};

namespace
{

constexpr double kReloadCheckSeconds = 1.0;

std::filesystem::path U8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// ---- The document's runs -----------------------------------------------------

int AddRun(MarkdownViewUi& m, const std::vector<md::Span>& spans)
{
    TokenRun run;
    for (const md::Span& span : spans)
        {
        Token t;
        t.style = span.style;
        t.link = span.link;
        t.start = (uint32_t)run.text.size();
        if (span.kind == md::SpanKind::Image)
            {
            t.kind = Token::Kind::Image;
            t.image = span.image;
            run.text += span.text;
            t.length = (uint32_t)span.text.size();
            run.tokens.push_back(t);
            continue;
            }
        if (span.kind == md::SpanKind::HardBreak)
            {
            t.kind = Token::Kind::Break;
            run.text += ' ';
            t.length = 1;
            run.tokens.push_back(t);
            continue;
            }
        const std::string text = span.kind == md::SpanKind::SoftBreak ? std::string(" ") : span.text;
        const bool code = (span.style & md::kCode) != 0;
        size_t i = 0;
        while (i < text.size())
            {
            const bool space = IsSpace(text[i]);
            size_t j = i;
            while (j < text.size() && IsSpace(text[j]) == space) ++j;
            Token w = t;
            w.kind = space ? Token::Kind::Space : Token::Kind::Word;
            w.start = (uint32_t)run.text.size();
            if (space && !code)
                {
                // A run of spaces in text is one space, as HTML has it.
                if (!run.text.empty() && run.text.back() == ' ' && !run.tokens.empty() && run.tokens.back().kind == Token::Kind::Space)
                    {
                    i = j;
                    continue;
                    }
                run.text += ' ';
                w.length = 1;
                }
            else
                {
                std::string piece = text.substr(i, j - i);
                for (char& c : piece) if (IsSpace(c)) c = ' ';
                run.text += piece;
                w.length = (uint32_t)piece.size();
                }
            run.tokens.push_back(w);
            i = j;
            }
        }
    m.runs.push_back(std::move(run));
    return (int)m.runs.size() - 1;
}

int AddTextRun(MarkdownViewUi& m, const std::string& text)
{
    TokenRun run;
    run.text = text;
    m.runs.push_back(std::move(run));
    return (int)m.runs.size() - 1;
}

void ReleasePictures(MarkdownViewUi& m)
{
    for (Picture& pic : m.pictures) textures::Release(pic.texture);
    m.pictures.clear();
}

bool ReadFile(const std::string& path, std::string& out, std::string& error)
{
    std::ifstream stream(U8(path), std::ios::binary);
    if (!stream)
        {
        error = "The file could not be opened.";
        return false;
        }
    std::ostringstream text;
    text << stream.rdbuf();
    out = text.str();
    return true;
}

void Load(MarkdownViewUi& m, const std::string& path, bool keepScroll)
{
    m.loadedPath = path;
    m.error.clear();
    m.source.clear();
    m.doc = md::Document{};
    m.runs.clear();
    m.blockRun.clear();
    m.cellRun.clear();
    ReleasePictures(m);
    m.layout.clear();
    m.layoutWidth = -1;
    m.hits.clear();
    m.currentHit = -1;
    m.findDoneFor.clear();
    m.hoveredLink = m.pressedLink = -1;
    if (!keepScroll) m.scrollTo = 0;
    m.lastCheck = ImGui::GetTime();

    std::error_code ec;
    m.modified = std::filesystem::last_write_time(U8(path), ec);
    m.fileBytes = 0;
    const auto size = std::filesystem::file_size(U8(path), ec);
    if (!ec) m.fileBytes = (uint64_t)size;
    if (!ec && size > 32ull * 1024 * 1024)
        {
        m.error = "The file is too large to show (32 MB limit).";
        return;
        }
    if (!ReadFile(path, m.source, m.error)) return;
    m.doc = md::Parse(m.source);

    m.blockRun.assign(m.doc.blocks.size(), -1);
    m.cellRun.resize(m.doc.blocks.size());
    for (size_t i = 0; i < m.doc.blocks.size(); ++i)
        {
        const md::Block& b = m.doc.blocks[i];
        switch (b.kind)
            {
            case md::BlockKind::Paragraph:
            case md::BlockKind::Heading: m.blockRun[i] = AddRun(m, b.spans); break;
            case md::BlockKind::Code: m.blockRun[i] = AddTextRun(m, b.code); break;
            case md::BlockKind::Table:
                for (const auto& row : b.rows)
                    {
                    m.cellRun[i].emplace_back();
                    for (const md::Cell& cell : row) m.cellRun[i].back().push_back(AddRun(m, cell));
                    }
                break;
            case md::BlockKind::Rule: break;
            }
        }
    m.pictures.resize(m.doc.images.size());
    for (size_t i = 0; i < m.doc.images.size(); ++i) m.pictures[i].path = md::ResolveImagePath(path, m.doc.images[i]);
}

// Notices an edit made in another program and shows it, keeping the place.
void ReloadIfChanged(MarkdownViewUi& m)
{
    const double now = ImGui::GetTime();
    if (now - m.lastCheck < kReloadCheckSeconds) return;
    m.lastCheck = now;
    std::error_code ec;
    const auto modified = std::filesystem::last_write_time(U8(m.loadedPath), ec);
    if (!ec && modified != m.modified) Load(m, m.loadedPath, true);
}

// ---- Fonts and pictures -------------------------------------------------------

float HeadingScale(int level)
{
    static constexpr float kScale[] = { 1.0f, 1.9f, 1.5f, 1.25f, 1.08f, 1.0f, 0.92f };
    return kScale[std::clamp(level, 0, 6)];
}

ImFont* FontFor(uint8_t style, bool heading)
{
    if (style & md::kCode) return fonts::Mono();
    if (heading || (style & md::kBold)) return fonts::Bold();
    if (style & md::kItalic) return fonts::Italic();
    return fonts::Ui();
}

float TextWidth(ImFont* font, float size, const char* b, const char* e)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, b, e).x;
}

// Local pictures are decoded the first time they are laid out; web ones
// are never fetched, and show as their alt text.
Picture* PictureFor(MarkdownViewUi& m, int index)
{
    if (index < 0 || index >= (int)m.pictures.size()) return nullptr;
    Picture& pic = m.pictures[(size_t)index];
    if (!pic.tried && !pic.path.empty())
        {
        pic.tried = true;
        app::DecodedImage image;
        std::string error;
        if (app::DecodeImageFile(pic.path, 2048, image, error))
            {
            pic.texture = textures::Create(image.width, image.height, image.rgba.data());
            pic.width = image.width;
            pic.height = image.height;
            }
        }
    return &pic;
}

// A picture that cannot be shown is a chip with its alt text.
float ChipWidth(const char* b, const char* e, float size)
{
    return TextWidth(fonts::Ui(), size, ICON_MD_IMAGE, nullptr) + size * 0.3f + TextWidth(fonts::Ui(), size, b, e) + size * 0.8f;
}

// ---- Layout -----------------------------------------------------------------

struct FlowStyle
{
    float base = 16;             // font size, pixels
    bool  heading = false;
    bool  centered = false;
    float lineGap = 1.5f;        // line height as a multiple of the font size
};

Flow LayoutRun(MarkdownViewUi& m, int runIndex, float wrap, const FlowStyle& fs)
{
    Flow flow;
    flow.run = runIndex;
    const TokenRun& run = m.runs[(size_t)runIndex];
    const std::vector<Token>& tokens = run.tokens;
    const float lineH = std::round(fs.base * fs.lineGap);
    const float codePad = std::round(fs.base * 0.25f);
    wrap = std::max(wrap, fs.base * 4);

    float x = 0, y = 0;
    float lineHeight = lineH;
    size_t lineFirst = 0;
    bool firstLine = true;

    auto finishLine = [&] {
        // A line drops a trailing space, so centring and wrapping ignore it.
        while (flow.items.size() > lineFirst && tokens[(size_t)flow.items.back().token].kind == Token::Kind::Space &&
               !(tokens[(size_t)flow.items.back().token].style & md::kCode))
            flow.items.pop_back();
        float lineW = 0;
        for (size_t i = lineFirst; i < flow.items.size(); ++i)
            {
            Placed& p = flow.items[i];
            // Text is centred in its own line height; a taller picture
            // stands on the line's bottom and makes the line taller.
            p.y = y + (p.h > lineH ? lineHeight - p.h : lineHeight - lineH + (lineH - p.h) * 0.5f);
            lineW = std::max(lineW, p.x + p.w + (p.runEnd ? codePad : 0.0f));
            }
        if (fs.centered && lineW < wrap)
            for (size_t i = lineFirst; i < flow.items.size(); ++i) flow.items[i].x += std::floor((wrap - lineW) * 0.5f);
        if (firstLine) flow.firstLineCenter = y + lineHeight - lineH * 0.5f;
        firstLine = false;
        flow.width = std::max(flow.width, lineW);
        y += lineHeight;
        x = 0;
        lineHeight = lineH;
        lineFirst = flow.items.size();
    };

    auto place = [&](int token, uint32_t start, uint32_t length, float px, float w, float h, ImFont* font, float size, bool runStart, bool runEnd) {
        Placed p;
        p.token = token;
        p.start = start;
        p.length = length;
        p.x = px;
        p.w = w;
        p.h = h;
        p.font = font;
        p.size = size;
        p.runStart = runStart;
        p.runEnd = runEnd;
        flow.items.push_back(p);
    };

    for (size_t ti = 0; ti < tokens.size(); ++ti)
        {
        const Token& t = tokens[ti];
        const bool code = (t.style & md::kCode) != 0;
        const float size = code ? std::round(fs.base * 0.9f) : fs.base;
        ImFont* font = FontFor(t.style, fs.heading);
        const bool prevCode = ti > 0 && (tokens[ti - 1].style & md::kCode) && tokens[ti - 1].kind != Token::Kind::Break;
        const bool nextCode = ti + 1 < tokens.size() && (tokens[ti + 1].style & md::kCode) && tokens[ti + 1].kind != Token::Kind::Break;
        const char* b = run.text.data() + t.start;
        const char* e = b + t.length;

        if (t.kind == Token::Kind::Break)
            {
            finishLine();
            continue;
            }
        if (t.kind == Token::Kind::Image)
            {
            const Picture* pic = PictureFor(m, t.image);
            float w = 0, h = 0;
            if (pic && pic->texture)
                {
                const float scale = ImGui::GetStyle().FontScaleDpi;
                w = (float)pic->width * scale;
                h = (float)pic->height * scale;
                if (w > wrap)
                    {
                    h *= wrap / w;
                    w = wrap;
                    }
                }
            else
                {
                w = std::min(wrap, ChipWidth(b, e, fs.base * 0.85f));
                h = std::round(fs.base * 1.35f);
                }
            if (x > 0 && x + w > wrap) finishLine();
            place((int)ti, t.start, t.length, x, w, h, fonts::Ui(), fs.base * 0.85f, false, false);
            lineHeight = std::max(lineHeight, h);
            x += w;
            continue;
            }

        const float leadPad = (code && !prevCode) ? codePad : 0.0f;
        const float tailPad = (code && !nextCode) ? codePad : 0.0f;
        if (t.kind == Token::Kind::Space)
            {
            if (x == 0 && !code) continue;   // no space at the start of a line
            const float w = code ? TextWidth(font, size, b, e) : TextWidth(font, size, " ", nullptr);
            place((int)ti, t.start, t.length, x + leadPad, w, size, font, size, leadPad > 0, tailPad > 0);
            x += leadPad + w + tailPad;
            continue;
            }

        // A word. One too long for a line of its own is cut, a character
        // at a time, where it has to be.
        float w = TextWidth(font, size, b, e);
        if (x > 0 && x + leadPad + w + tailPad > wrap) finishLine();
        bool firstPiece = true;
        while (leadPad + w + tailPad > wrap && e - b > 1)
            {
            const char* cut = b;
            float used = firstPiece ? leadPad : 0.0f;
            while (cut < e)
                {
                const char* next = cut + 1;
                while (next < e && ((unsigned char)*next & 0xC0) == 0x80) ++next;
                const float cw = TextWidth(font, size, cut, next);
                if (used + cw > wrap && cut > b) break;
                used += cw;
                cut = next;
                }
            if (cut >= e) break;
            const float pieceLead = firstPiece ? leadPad : 0.0f;
            place((int)ti, (uint32_t)(b - run.text.data()), (uint32_t)(cut - b), x + pieceLead, used - pieceLead, size, font, size,
                  firstPiece && leadPad > 0, false);
            finishLine();
            firstPiece = false;
            b = cut;
            w = TextWidth(font, size, b, e);
            }
        const float pieceLead = firstPiece ? leadPad : 0.0f;
        place((int)ti, (uint32_t)(b - run.text.data()), (uint32_t)(e - b), x + pieceLead, w, size, font, size, firstPiece && leadPad > 0, tailPad > 0);
        x += pieceLead + w + tailPad;
        }
    if (flow.items.size() > lineFirst || firstLine) finishLine();
    flow.height = y;
    return flow;
}

// The widest a run is on one line, and its longest word: what a table
// column would like, and the least it can do with.
void MeasureRun(MarkdownViewUi& m, int runIndex, float base, bool heading, float& natural, float& minimum)
{
    const TokenRun& run = m.runs[(size_t)runIndex];
    natural = minimum = 0;
    float line = 0;
    for (const Token& t : run.tokens)
        {
        if (t.kind == Token::Kind::Break)
            {
            natural = std::max(natural, line);
            line = 0;
            continue;
            }
        const bool code = (t.style & md::kCode) != 0;
        const float size = code ? std::round(base * 0.9f) : base;
        float w = 0;
        if (t.kind == Token::Kind::Image)
            {
            const Picture* pic = PictureFor(m, t.image);
            w = (pic && pic->texture) ? std::min((float)pic->width * ImGui::GetStyle().FontScaleDpi, base * 20)
                                      : ChipWidth(run.text.data() + t.start, run.text.data() + t.start + t.length, base * 0.85f);
            }
        else
            {
            w = TextWidth(FontFor(t.style, heading), size, run.text.data() + t.start, run.text.data() + t.start + t.length);
            if (code) w += std::round(base * 0.5f);
            }
        line += w;
        if (t.kind != Token::Kind::Space) minimum = std::max(minimum, std::min(w, base * 14));
        }
    natural = std::max(natural, line);
}

// The left edge of a block's content: quotes and list levels push it in.
float BlockIndent(const md::Block& b, float base)
{
    return (float)b.quoteDepth * std::round(base * 1.1f) + (float)b.listDepth * std::round(base * 1.6f);
}

void LayoutDocument(MarkdownViewUi& m, float width, float base)
{
    m.layout.assign(m.doc.blocks.size(), BlockLayout{});
    m.layoutWidth = width;
    m.layoutFont = base;
    float y = 0;
    const float para = std::round(base * 0.8f);
    for (size_t i = 0; i < m.doc.blocks.size(); ++i)
        {
        const md::Block& b = m.doc.blocks[i];
        BlockLayout& L = m.layout[i];
        L.x = BlockIndent(b, base);
        L.w = std::max(base * 4, width - L.x);
        L.y = y;
        // Blocks inside the same tight list item sit close together.
        const bool nextTight = i + 1 < m.doc.blocks.size() && m.doc.blocks[i + 1].tight && m.doc.blocks[i + 1].listDepth > 0;
        float after = (b.tight && nextTight) ? std::round(base * 0.2f) : para;
        switch (b.kind)
            {
            case md::BlockKind::Paragraph:
                {
                FlowStyle fs;
                fs.base = base;
                fs.centered = b.centered;
                L.flow = LayoutRun(m, m.blockRun[i], L.w, fs);
                L.h = L.flow.height;
                break;
                }
            case md::BlockKind::Heading:
                {
                FlowStyle fs;
                fs.base = std::round(base * HeadingScale(b.level));
                fs.heading = true;
                fs.centered = b.centered;
                fs.lineGap = 1.3f;
                L.top = i == 0 ? 0.0f : std::round(base * (b.level <= 2 ? 1.0f : 0.6f));
                L.flow = LayoutRun(m, m.blockRun[i], L.w, fs);
                L.h = L.top + L.flow.height;
                // The two top levels are underlined, as on GitHub.
                if (b.level <= 2) L.h += std::round(base * 0.35f);
                after = std::round(base * 0.55f);
                break;
                }
            case md::BlockKind::Code:
                {
                const float size = std::round(base * 0.9f);
                const float pad = std::round(base * 0.75f);
                L.lineH = std::round(size * 1.45f);
                L.lineStarts.clear();
                L.lineStarts.push_back(0);
                const std::string& code = b.code;
                float widest = 0;
                for (size_t k = 0; k <= code.size(); ++k)
                    if (k == code.size() || code[k] == '\n')
                        {
                        const size_t s = L.lineStarts.back();
                        widest = std::max(widest, TextWidth(fonts::Mono(), size, code.data() + s, code.data() + k));
                        if (k < code.size()) L.lineStarts.push_back(k + 1);
                        }
                L.codeW = widest + pad * 2;
                L.codeH = (float)L.lineStarts.size() * L.lineH + pad * 2;
                // A block wider than the column scrolls sideways in its own
                // strip, which costs a scrollbar's height.
                if (L.codeW > L.w) L.codeH += ImGui::GetStyle().ScrollbarSize;
                L.h = L.codeH;
                break;
                }
            case md::BlockKind::Rule:
                L.h = std::round(base * 1.2f);
                after = std::round(base * 0.3f);
                break;
            case md::BlockKind::Table:
                {
                const size_t cols = b.align.size();
                if (cols == 0) break;
                const float cellPad = std::round(base * 0.6f);
                std::vector<float> natural(cols, 0), minimum(cols, 0);
                for (size_t r = 0; r < b.rows.size(); ++r)
                    for (size_t c = 0; c < b.rows[r].size() && c < cols; ++c)
                        {
                        float n = 0, mn = 0;
                        MeasureRun(m, m.cellRun[i][r][c], base, (int)r < b.headerRows, n, mn);
                        natural[c] = std::max(natural[c], n + cellPad * 2);
                        minimum[c] = std::max(minimum[c], mn + cellPad * 2);
                        }
                float wantW = 0, minW = 0;
                for (size_t c = 0; c < cols; ++c)
                    {
                    wantW += natural[c];
                    minW += minimum[c];
                    }
                // Every column gets what it wants if that fits; otherwise its
                // longest word, plus a share of what is left in proportion to
                // how much more it wanted.
                L.colW.assign(cols, 0);
                if (wantW <= L.w)
                    {
                    L.colW = natural;
                    }
                else if (minW >= L.w)
                    {
                    L.colW = minimum;
                    }
                else
                    {
                    const float spare = L.w - minW;
                    for (size_t c = 0; c < cols; ++c) L.colW[c] = minimum[c] + spare * (natural[c] - minimum[c]) / std::max(1.0f, wantW - minW);
                    }
                L.colX.assign(cols, 0);
                float x = 0;
                for (size_t c = 0; c < cols; ++c)
                    {
                    L.colW[c] = std::floor(L.colW[c]);
                    L.colX[c] = x;
                    x += L.colW[c];
                    }
                L.tableW = x;
                L.cells.assign(b.rows.size(), {});
                L.rowY.assign(b.rows.size(), 0);
                L.rowH.assign(b.rows.size(), 0);
                float ry = 0;
                for (size_t r = 0; r < b.rows.size(); ++r)
                    {
                    float rowH = base * 1.5f;
                    L.cells[r].resize(cols);
                    for (size_t c = 0; c < cols; ++c)
                        {
                        if (c >= b.rows[r].size()) continue;
                        FlowStyle fs;
                        fs.base = base;
                        fs.heading = (int)r < b.headerRows;
                        fs.lineGap = 1.4f;
                        Flow f = LayoutRun(m, m.cellRun[i][r][c], L.colW[c] - cellPad * 2, fs);
                        // Alignment from the separator row: shift each line's items.
                        const md::Align align = b.align[c];
                        if (align == md::Align::Center || align == md::Align::Right)
                            {
                            const float free = L.colW[c] - cellPad * 2 - f.width;
                            for (Placed& p : f.items) p.x += align == md::Align::Right ? free : std::floor(free * 0.5f);
                            }
                        rowH = std::max(rowH, f.height);
                        L.cells[r][c] = std::move(f);
                        }
                    L.rowY[r] = ry;
                    L.rowH[r] = rowH + std::round(base * 0.5f);
                    ry += L.rowH[r];
                    }
                L.tableH = ry;
                L.h = ry + (L.tableW > L.w ? ImGui::GetStyle().ScrollbarSize : 0.0f);
                break;
                }
            }
        L.h += after;
        y += L.h;
        }
    m.totalHeight = y;
}

// ---- Find ------------------------------------------------------------------

std::string Lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

// Every match, in document order: blocks top to bottom, a table's cells
// row by row. ASCII case is ignored.
void FindAll(MarkdownViewUi& m)
{
    m.hits.clear();
    m.currentHit = -1;
    m.findDoneFor = m.findText;
    if (m.findText.empty()) return;
    const std::string needle = Lower(m.findText);
    auto scan = [&](int run) {
        if (run < 0) return;
        const std::string hay = Lower(m.runs[(size_t)run].text);
        for (size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size()))
            m.hits.push_back({ run, (uint32_t)pos, (uint32_t)needle.size() });
    };
    for (size_t i = 0; i < m.doc.blocks.size(); ++i)
        {
        scan(m.blockRun[i]);
        for (const auto& row : m.cellRun[i])
            for (int run : row) scan(run);
        }
    if (!m.hits.empty())
        {
        m.currentHit = 0;
        m.scrollToHit = true;
        }
}

void StepHit(MarkdownViewUi& m, int dir)
{
    if (m.hits.empty()) return;
    const int n = (int)m.hits.size();
    m.currentHit = ((m.currentHit + dir) % n + n) % n;
    m.scrollToHit = true;
}

// Where a match is, in content space: the top of the line it is on.
bool HitPosition(const MarkdownViewUi& m, const Hit& hit, float& y)
{
    auto inFlow = [&](const Flow& f, float top) {
        if (f.run != hit.run) return false;
        for (const Placed& p : f.items)
            if (hit.start < p.start + p.length && hit.start + hit.length > p.start)
                {
                y = top + p.y;
                return true;
                }
        return false;
    };
    for (size_t i = 0; i < m.layout.size(); ++i)
        {
        const BlockLayout& L = m.layout[i];
        if (m.blockRun[i] == hit.run)
            {
            if (m.doc.blocks[i].kind == md::BlockKind::Code)
                {
                const auto it = std::upper_bound(L.lineStarts.begin(), L.lineStarts.end(), (size_t)hit.start);
                const size_t line = (size_t)(it - L.lineStarts.begin()) - 1;
                y = L.y + std::round(m.layoutFont * 0.75f) + (float)line * L.lineH;
                return true;
                }
            if (inFlow(L.flow, L.y + L.top)) return true;
            }
        for (size_t r = 0; r < L.cells.size(); ++r)
            for (const Flow& f : L.cells[r])
                if (inFlow(f, L.y + L.rowY[r] + std::round(m.layoutFont * 0.25f))) return true;
        }
    return false;
}

// ---- Links -----------------------------------------------------------------

bool ScrollToAnchor(MarkdownViewUi& m, const std::string& anchor)
{
    const std::string want = Lower(anchor);
    for (size_t i = 0; i < m.doc.blocks.size() && i < m.layout.size(); ++i)
        if (m.doc.blocks[i].kind == md::BlockKind::Heading && Lower(m.doc.blocks[i].anchor) == want)
            {
            m.scrollTo = m.layout[i].y;
            return true;
            }
    return false;
}

void FollowLink(app::AppState& state, UiState& ui, app::FileView& view, MarkdownViewUi& m, const std::string& href)
{
    const md::LinkTarget t = md::ResolveLink(view.path, href);
    std::string error;
    switch (t.kind)
        {
        case md::LinkTarget::Kind::Anchor:
            if (!t.anchor.empty()) ScrollToAnchor(m, t.anchor);
            else m.scrollTo = 0;
            return;
        case md::LinkTarget::Kind::Web:
            if (!platform::OpenUrl(t.url, error)) ShowError(ui, error);
            return;
        case md::LinkTarget::Kind::Unsupported:
            ShowError(ui, "Davesplorer does not open links like this one:\n" + href);
            return;
        case md::LinkTarget::Kind::File:
            break;
        }
    std::error_code ec;
    const std::filesystem::path p = U8(t.path);
    if (!std::filesystem::exists(p, ec))
        {
        ShowError(ui, "The link's target does not exist:\n" + t.path);
        return;
        }
    if (std::filesystem::is_directory(p, ec))
        {
        // A folder opens in the folder view, the way the shell would.
        if (!state.tabs.empty() && app::NavigateTo(state.active(), t.path, error)) ui.view(state.active().id).wantFocus = true;
        else if (!error.empty()) ShowError(ui, error);
        return;
        }
    const std::string name = app::PathName(t.path);
    const size_t dot = name.find_last_of('.');
    const std::string ext = dot == std::string::npos ? std::string() : Lower(name.substr(dot + 1));
    if (md::IsMarkdown(ext))
        {
        // Another Markdown file replaces this one in the view; Back returns.
        m.history.push_back({ view.path, m.scrollY });
        if (m.history.size() > 50) m.history.erase(m.history.begin());
        view.path = t.path;
        m.scrollToAnchor = t.anchor;
        return;
        }
    app::FileViewKind kind{};
    if (app::ViewableInApp(ext, kind))
        {
        OpenFileView(state, ui, t.path);
        return;
        }
    if (!platform::OpenWithShell(t.path, error)) ShowError(ui, error);
}

void GoBack(app::FileView& view, MarkdownViewUi& m)
{
    if (m.history.empty()) return;
    const MarkdownViewUi::Visit visit = m.history.back();
    m.history.pop_back();
    view.path = visit.path;
    m.loadedPath.clear();   // reloads on the next draw
    m.scrollTo = visit.scroll;
}

// ---- Drawing ---------------------------------------------------------------

struct DrawContext
{
    app::AppState*  state = nullptr;
    UiState*        ui = nullptr;
    app::FileView*  view = nullptr;
    MarkdownViewUi* m = nullptr;
    ImDrawList*     dl = nullptr;
    ImVec2          origin;          // screen position of content (0,0)
    float           base = 16;
    bool            hovered = false; // the content window is under the pointer
    int             hoverLink = -1;  // found this frame
    int             hoverPicture = -1;
};

ImU32 TextColor(const md::Block& b, bool heading)
{
    const Palette& p = CurrentPalette();
    if (heading && b.level == 6) return p.textMuted;
    if (heading) return p.heading;
    if (b.quoteDepth > 0) return p.textMuted;
    return p.text;
}

void DrawHighlights(DrawContext& c, const TokenRun& run, int runIndex, const Placed& p, ImVec2 at)
{
    for (size_t h = 0; h < c.m->hits.size(); ++h)
        {
        const Hit& hit = c.m->hits[h];
        if (hit.run != runIndex) continue;
        const uint32_t s = std::max(hit.start, p.start);
        const uint32_t e = std::min(hit.start + hit.length, p.start + p.length);
        if (s >= e) continue;
        const char* base = run.text.data();
        const float x0 = TextWidth(p.font, p.size, base + p.start, base + s);
        const float x1 = x0 + TextWidth(p.font, p.size, base + s, base + e);
        const bool current = (int)h == c.m->currentHit;
        const Palette& pal = CurrentPalette();
        c.dl->AddRectFilled(ImVec2(at.x + x0 - 1, at.y), ImVec2(at.x + x1 + 1, at.y + p.h), WithAlpha(current ? pal.accent : pal.warning, current ? 150 : 90), 2.0f);
        }
}

void DrawFlow(DrawContext& c, const Flow& f, ImVec2 at, ImU32 color)
{
    const TokenRun& run = c.m->runs[(size_t)f.run];
    const Palette& pal = CurrentPalette();
    const ThemeTokens& theme = ThemeAt(CurrentThemeIndex());
    const ImGuiIO& io = ImGui::GetIO();
    for (const Placed& p : f.items)
        {
        const Token& t = run.tokens[(size_t)p.token];
        const ImVec2 a(at.x + p.x, at.y + p.y);
        const ImVec2 b(a.x + p.w, a.y + p.h);
        const bool inside = c.hovered && io.MousePos.x >= a.x && io.MousePos.x < b.x && io.MousePos.y >= a.y && io.MousePos.y < b.y;
        if (inside && t.link >= 0) c.hoverLink = t.link;
        const bool link = t.link >= 0;
        const bool linkHot = link && t.link == c.m->hoveredLink;

        if (t.kind == Token::Kind::Image)
            {
            const Picture& pic = c.m->pictures[(size_t)t.image];
            if (pic.texture)
                {
                c.dl->AddImage(pic.texture->GetTexRef(), a, b);
                if (inside && !link) c.hoverPicture = t.image;
                if (linkHot) c.dl->AddRect(a, b, pal.link, 2.0f);
                }
            else
                {
                // The alt text in a chip, the link colour when it is one.
                const ImU32 fg = link ? pal.link : pal.textMuted;
                c.dl->AddRectFilled(a, b, theme.inlineCodeBackground, p.h * 0.25f);
                c.dl->AddRect(a, b, WithAlpha(fg, 90), p.h * 0.25f);
                const float iconW = TextWidth(fonts::Ui(), p.size, ICON_MD_IMAGE, nullptr);
                const float ty = a.y + (p.h - p.size) * 0.5f;
                const ImVec4 clip(a.x, a.y, b.x - p.size * 0.3f, b.y);
                c.dl->AddText(fonts::Ui(), p.size, ImVec2(a.x + p.size * 0.4f, ty - p.size * 0.12f), fg, ICON_MD_IMAGE, nullptr, 0.0f, &clip);
                c.dl->AddText(fonts::Ui(), p.size, ImVec2(a.x + p.size * 0.4f + iconW + p.size * 0.2f, ty), fg,
                              run.text.data() + t.start, run.text.data() + t.start + t.length, 0.0f, &clip);
                }
            continue;
            }

        const bool code = (t.style & md::kCode) != 0;
        if (code)
            {
            const float pad = std::round(c.base * 0.25f);
            const ImVec2 ba(a.x - (p.runStart ? pad : 0.0f), a.y - 1);
            const ImVec2 bb(b.x + (p.runEnd ? pad : 0.0f), b.y + 1);
            ImDrawFlags corners = ImDrawFlags_RoundCornersNone;
            if (p.runStart) corners |= ImDrawFlags_RoundCornersLeft;
            if (p.runEnd) corners |= ImDrawFlags_RoundCornersRight;
            c.dl->AddRectFilled(ba, bb, theme.inlineCodeBackground, 3.0f, corners);
            }
        if (!c.m->hits.empty()) DrawHighlights(c, run, f.run, p, a);
        if (t.kind == Token::Kind::Space && !code)
            {
            if (link && linkHot) c.dl->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), pal.link);
            continue;
            }
        ImU32 fg = color;
        if (code) fg = theme.inlineCodeText;
        if (link) fg = pal.link;
        const char* s = run.text.data() + p.start;
        c.dl->AddText(p.font, p.size, a, fg, s, s + p.length);
        // Lines through and under, drawn by hand: ImGui has no such styles.
        if (t.style & md::kStrike) c.dl->AddLine(ImVec2(a.x, a.y + p.h * 0.55f), ImVec2(b.x, a.y + p.h * 0.55f), fg, std::max(1.0f, p.size / 14));
        if ((t.style & md::kUnderline) || linkHot) c.dl->AddLine(ImVec2(a.x, b.y - 1), ImVec2(b.x, b.y - 1), fg, std::max(1.0f, p.size / 16));
        }
}

void DrawMarker(DrawContext& c, const md::Block& b, const BlockLayout& L, ImVec2 at)
{
    const Palette& pal = CurrentPalette();
    const float cy = at.y + L.top + L.flow.firstLineCenter;
    const float slot = std::round(c.base * 1.6f);
    const float right = at.x - std::round(c.base * 0.45f);
    if (b.task != 0)
        {
        const char* icon = b.task == 2 ? ICON_MD_CHECK_BOX : ICON_MD_CHECK_BOX_OUTLINE_BLANK;
        const float size = c.base * 1.05f;
        const float w = TextWidth(fonts::Ui(), size, icon, nullptr);
        c.dl->AddText(fonts::Ui(), size, ImVec2(right - w, cy - size * 0.5f - size * 0.18f), b.task == 2 ? pal.accent : pal.textMuted, icon);
        return;
        }
    if (!b.marker.empty())
        {
        const float w = TextWidth(fonts::Ui(), c.base, b.marker.c_str(), nullptr);
        c.dl->AddText(fonts::Ui(), c.base, ImVec2(right - w, cy - c.base * 0.5f - c.base * 0.08f), pal.textMuted, b.marker.c_str());
        return;
        }
    // Bullets by depth: filled disc, ring, square, as browsers draw them.
    const float r = std::max(2.0f, c.base * 0.16f);
    const ImVec2 center(right - slot * 0.25f, cy);
    switch ((b.listDepth - 1) % 3)
        {
        case 0: c.dl->AddCircleFilled(center, r, pal.textMuted); break;
        case 1: c.dl->AddCircle(center, r, pal.textMuted, 0, 1.2f); break;
        default: c.dl->AddRectFilled(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r), pal.textMuted); break;
        }
}

void DrawQuoteBars(DrawContext& c, const md::Block& b, const BlockLayout& L, ImVec2 blockAt, bool joinNext)
{
    const Palette& pal = CurrentPalette();
    const float step = std::round(c.base * 1.1f);
    const float w = std::max(2.0f, std::round(c.base * 0.2f));
    const float bottom = blockAt.y + (joinNext ? L.h : L.h - std::round(c.base * 0.8f));
    for (int q = 0; q < b.quoteDepth; ++q)
        {
        const float x = blockAt.x + (float)q * step;
        c.dl->AddRectFilled(ImVec2(x, blockAt.y), ImVec2(x + w, bottom), WithAlpha(pal.border, 220));
        }
}

void DrawCodeBlock(DrawContext& c, size_t index, const md::Block& b, const BlockLayout& L, ImVec2 at)
{
    const ThemeTokens& theme = ThemeAt(CurrentThemeIndex());
    const float size = std::round(c.base * 0.9f);
    const float pad = std::round(c.base * 0.75f);
    const ImVec2 boxMax(at.x + L.w, at.y + L.codeH);
    c.dl->AddRectFilled(at, boxMax, theme.codeBackground, 6.0f);
    const int run = c.m->blockRun[index];

    // A strip of its own, so a long line scrolls sideways under its box
    // rather than past the column.
    ImGui::SetCursorScreenPos(at);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const std::string id = "##code" + std::to_string(index);
    ImGui::BeginChild(id.c_str(), ImVec2(L.w, L.codeH), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 inner = ImGui::GetCursorScreenPos();
    const std::string& code = b.code;
    const TokenRun& tr = c.m->runs[(size_t)run];
    const ImU32 fg = ImGui::GetColorU32(ImGuiCol_Text);
    for (size_t li = 0; li < L.lineStarts.size(); ++li)
        {
        const size_t s = L.lineStarts[li];
        const size_t e = li + 1 < L.lineStarts.size() ? L.lineStarts[li + 1] - 1 : code.size();
        const ImVec2 pos(inner.x + pad, inner.y + pad + (float)li * L.lineH + (L.lineH - size) * 0.5f);
        for (size_t h = 0; h < c.m->hits.size(); ++h)
            {
            const Hit& hit = c.m->hits[h];
            if (hit.run != run || hit.start >= e || hit.start + hit.length <= s) continue;
            const size_t hs = std::max<size_t>(hit.start, s), he = std::min<size_t>(hit.start + hit.length, e);
            const float x0 = TextWidth(fonts::Mono(), size, tr.text.data() + s, tr.text.data() + hs);
            const float x1 = x0 + TextWidth(fonts::Mono(), size, tr.text.data() + hs, tr.text.data() + he);
            const bool current = (int)h == c.m->currentHit;
            dl->AddRectFilled(ImVec2(pos.x + x0 - 1, pos.y - 1), ImVec2(pos.x + x1 + 1, pos.y + size + 2),
                              WithAlpha(current ? CurrentPalette().accent : CurrentPalette().warning, current ? 150 : 90), 2.0f);
            }
        dl->AddText(fonts::Mono(), size, pos, fg, code.data() + s, code.data() + e);
        }
    ImGui::Dummy(ImVec2(L.codeW, L.codeH - (L.codeW > L.w ? ImGui::GetStyle().ScrollbarSize : 0.0f)));
    // Copy, in the corner, while the pointer is over the block.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
        {
        const float bw = ImGui::GetFrameHeight();
        ImGui::SetCursorScreenPos(ImVec2(at.x + L.w - bw - 4, at.y + 4));
        if (IconButton("CopyCode", ICON_MD_CONTENT_COPY, "Copy")) ImGui::SetClipboardText(code.c_str());
        }
    if (!b.language.empty())
        {
        const float ls = std::round(c.base * 0.75f);
        const float lw = TextWidth(fonts::Ui(), ls, b.language.c_str(), nullptr);
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))
            dl->AddText(fonts::Ui(), ls, ImVec2(at.x + L.w - lw - pad * 0.6f, at.y + pad * 0.3f), CurrentPalette().textFaint, b.language.c_str());
        }
    ImGui::EndChild();
}

void DrawTable(DrawContext& c, size_t index, const md::Block& b, const BlockLayout& L, ImVec2 at)
{
    const Palette& pal = CurrentPalette();
    const ThemeTokens& theme = ThemeAt(CurrentThemeIndex());
    const float cellPad = std::round(c.base * 0.6f);
    auto drawGrid = [&](ImDrawList* dl, ImVec2 o) {
        for (size_t r = 0; r < L.rowY.size(); ++r)
            {
            const ImVec2 ra(o.x, o.y + L.rowY[r]);
            const ImVec2 rb(o.x + L.tableW, ra.y + L.rowH[r]);
            if ((int)r < b.headerRows) dl->AddRectFilled(ra, rb, Lift(theme.surface, 0.05f, theme.isDark));
            else if ((r - (size_t)b.headerRows) % 2 == 1) dl->AddRectFilled(ra, rb, WithAlpha(theme.codeBackground, 160));
            }
        for (size_t r = 0; r < L.rowY.size(); ++r)
            for (size_t col = 0; col < L.colX.size(); ++col)
                {
                const ImVec2 ca(o.x + L.colX[col], o.y + L.rowY[r]);
                dl->AddRect(ca, ImVec2(ca.x + L.colW[col] + 1, ca.y + L.rowH[r] + 1), pal.border);
                }
    };
    auto drawCells = [&](ImDrawList* dl, ImVec2 o) {
        ImDrawList* saved = c.dl;
        c.dl = dl;
        for (size_t r = 0; r < L.cells.size(); ++r)
            for (size_t col = 0; col < L.cells[r].size(); ++col)
                {
                const Flow& f = L.cells[r][col];
                if (f.run < 0) continue;
                const float vy = std::round((L.rowH[r] - f.height) * 0.5f);
                DrawFlow(c, f, ImVec2(o.x + L.colX[col] + cellPad, o.y + L.rowY[r] + vy), (int)r < b.headerRows ? pal.heading : pal.text);
                }
        c.dl = saved;
    };
    if (L.tableW <= L.w)
        {
        drawGrid(c.dl, at);
        drawCells(c.dl, at);
        return;
        }
    // Wider than the column: a strip that scrolls sideways.
    ImGui::SetCursorScreenPos(at);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const std::string id = "##table" + std::to_string(index);
    ImGui::BeginChild(id.c_str(), ImVec2(L.w, L.tableH + ImGui::GetStyle().ScrollbarSize), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const bool savedHover = c.hovered;
    c.hovered = c.hovered && ImGui::IsWindowHovered();
    drawGrid(ImGui::GetWindowDrawList(), o);
    drawCells(ImGui::GetWindowDrawList(), o);
    c.hovered = savedHover;
    ImGui::Dummy(ImVec2(L.tableW + 1, L.tableH));
    ImGui::EndChild();
}

void DrawBlock(DrawContext& c, size_t i)
{
    MarkdownViewUi& m = *c.m;
    const md::Block& b = m.doc.blocks[i];
    const BlockLayout& L = m.layout[i];
    const ImVec2 blockAt(c.origin.x, c.origin.y + L.y);
    const ImVec2 at(c.origin.x + L.x, c.origin.y + L.y);

    if (b.quoteDepth > 0)
        {
        const bool joinNext = i + 1 < m.doc.blocks.size() && m.doc.blocks[i + 1].quoteDepth >= b.quoteDepth;
        DrawQuoteBars(c, b, L, blockAt, joinNext);
        }
    switch (b.kind)
        {
        case md::BlockKind::Paragraph:
            DrawFlow(c, L.flow, at, TextColor(b, false));
            break;
        case md::BlockKind::Heading:
            DrawFlow(c, L.flow, ImVec2(at.x, at.y + L.top), TextColor(b, true));
            if (b.level <= 2)
                {
                const float y = at.y + L.top + L.flow.height + std::round(c.base * 0.2f);
                c.dl->AddLine(ImVec2(at.x, y), ImVec2(at.x + L.w, y), CurrentPalette().border, 1.0f);
                }
            break;
        case md::BlockKind::Code:
            DrawCodeBlock(c, i, b, L, at);
            break;
        case md::BlockKind::Rule:
            {
            const float y = std::round(at.y + L.h * 0.5f);
            c.dl->AddRectFilled(ImVec2(at.x, y - 1), ImVec2(at.x + L.w, y + 1), CurrentPalette().border);
            break;
            }
        case md::BlockKind::Table:
            DrawTable(c, i, b, L, at);
            break;
        }
    if (b.itemStart && (b.kind == md::BlockKind::Paragraph || b.kind == md::BlockKind::Heading)) DrawMarker(c, b, L, at);
}

// ---- The window's parts ----------------------------------------------------

void DrawContentsPopup(MarkdownViewUi& m)
{
    if (!ImGui::BeginPopup("##contents")) return;
    const float indent = ImGui::GetFontSize();
    int count = 0;
    for (size_t i = 0; i < m.doc.blocks.size() && i < m.layout.size(); ++i)
        {
        const md::Block& b = m.doc.blocks[i];
        if (b.kind != md::BlockKind::Heading) continue;
        ++count;
        ImGui::PushID((int)i);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + indent * (float)std::max(0, b.level - 1));
        std::string title = md::PlainText(b.spans);
        if (title.empty()) title = "(untitled)";
        if (ImGui::Selectable(title.c_str()))
            {
            m.scrollTo = m.layout[i].y;
            ImGui::CloseCurrentPopup();
            }
        ImGui::PopID();
        }
    if (count == 0) FaintText("No headings");
    ImGui::EndPopup();
}

bool HasHeadings(const MarkdownViewUi& m)
{
    for (const md::Block& b : m.doc.blocks)
        if (b.kind == md::BlockKind::Heading) return true;
    return false;
}

void OpenFind(MarkdownViewUi& m)
{
    m.findOpen = true;
    m.findWantFocus = true;
}

void CloseFind(MarkdownViewUi& m)
{
    m.findOpen = false;
    m.findText.clear();
    m.hits.clear();
    m.currentHit = -1;
    m.findDoneFor.clear();
}

void DrawToolbar(app::AppState& state, UiState& ui, app::FileView& view, MarkdownViewUi& m)
{
    if (IconButton("Back", ICON_MD_ARROW_BACK, "Back", !m.history.empty())) GoBack(view, m);
    ImGui::SameLine();
    if (IconButton("Contents", ICON_MD_TOC, "Contents", HasHeadings(m))) ImGui::OpenPopup("##contents");
    DrawContentsPopup(m);
    ImGui::SameLine();
    if (IconButton("Find", ICON_MD_SEARCH, "Find (Ctrl+F)", !m.showSource))
        {
        if (m.findOpen) CloseFind(m);
        else OpenFind(m);
        }
    ImGui::SameLine();
    // Source: the file's text, to select and copy from.
    if (m.showSource) ImGui::PushStyleColor(ImGuiCol_Text, CurrentPalette().accent);
    const bool flip = IconButton("Source", ICON_MD_CODE, m.showSource ? "Show rendered" : "Show source");
    if (m.showSource) ImGui::PopStyleColor();
    if (flip)
        {
        m.showSource = !m.showSource;
        if (m.showSource) CloseFind(m);
        }

    const ImGuiStyle& style = ImGui::GetStyle();
    auto buttonW = [&](const char* icon) { return ImGui::CalcTextSize(icon).x + style.FramePadding.x * 2.0f; };
    const float cluster = buttonW(ICON_MD_FOLDER_OPEN) + buttonW(ICON_MD_OPEN_IN_NEW) + style.ItemSpacing.x;
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

void DrawFindBar(MarkdownViewUi& m)
{
    if (m.findWantFocus)
        {
        ImGui::SetKeyboardFocusHere();
        m.findWantFocus = false;
        }
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.5f, ImGui::GetFontSize() * 18));
    if (ImGui::InputTextWithHint("##find", "Find in document", &m.findText, ImGuiInputTextFlags_EnterReturnsTrue))
        {
        StepHit(m, ImGui::GetIO().KeyShift ? -1 : 1);
        m.findWantFocus = true;   // Enter leaves the field; keep typing in it
        }
    if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape)) CloseFind(m);
    if (m.findText != m.findDoneFor) FindAll(m);
    ImGui::SameLine();
    if (IconButton("FindPrev", ICON_MD_KEYBOARD_ARROW_UP, "Previous match (Shift+Enter)", !m.hits.empty())) StepHit(m, -1);
    ImGui::SameLine();
    if (IconButton("FindNext", ICON_MD_KEYBOARD_ARROW_DOWN, "Next match (Enter)", !m.hits.empty())) StepHit(m, 1);
    if (!m.findText.empty())
        {
        const std::string count = m.hits.empty() ? std::string("No matches")
                                                  : std::to_string(m.currentHit + 1) + " of " + std::to_string(m.hits.size());
        ToolbarGroup(ImGui::CalcTextSize(count.c_str()).x);
        ImGui::AlignTextToFramePadding();
        MutedText(count.c_str());
        }
    ToolbarGroup(ImGui::GetFrameHeight());
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - ImGui::GetFrameHeight()));
    if (IconButton("FindClose", ICON_MD_CLOSE, "Close (Escape)")) CloseFind(m);
}

void DrawSource(MarkdownViewUi& m, ImVec2 size)
{
    ImGui::PushFont(fonts::Mono(), 0.0f);
    ImGui::InputTextMultiline("##source", &m.source, size, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();
}

void DrawDocument(app::AppState& state, UiState& ui, app::FileView& view, MarkdownViewUi& m, ImVec2 size)
{
    const ThemeTokens& theme = ThemeAt(CurrentThemeIndex());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.surface);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    const bool open = ImGui::BeginChild("##doc", size, ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoNavInputs);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (!open)
        {
        ImGui::EndChild();
        return;
        }
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float base = ImGui::GetFontSize();
    const float viewW = window->InnerRect.GetWidth();
    const float viewH = window->InnerRect.GetHeight();
    // A readable column: as wide as the view, up to about ninety characters.
    const float margin = std::round(base * 1.4f);
    const float column = std::max(base * 8, std::min(viewW - margin * 2, base * 48));
    if (m.layoutWidth != column || m.layoutFont != base || m.layout.size() != m.doc.blocks.size()) LayoutDocument(m, column, base);

    // Requested jumps: a heading, a place, a match.
    if (!m.scrollToAnchor.empty())
        {
        ScrollToAnchor(m, m.scrollToAnchor);
        m.scrollToAnchor.clear();
        }
    if (m.scrollToHit && m.currentHit >= 0 && m.currentHit < (int)m.hits.size())
        {
        float y = 0;
        if (HitPosition(m, m.hits[(size_t)m.currentHit], y))
            {
            const float top = margin + y;
            const float now = ImGui::GetScrollY();
            if (top < now + base * 2 || top > now + viewH - base * 3) m.scrollTo = std::max(0.0f, top - viewH * 0.4f - margin);
            }
        m.scrollToHit = false;
        }
    if (m.scrollTo >= 0)
        {
        ImGui::SetScrollY(m.scrollTo);
        m.scrollTo = -1;
        }

    // Keys, while this view has the focus and no field does.
    ImGuiIO& io = ImGui::GetIO();
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && !io.WantTextInput && !io.KeyCtrl && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        {
        float y = ImGui::GetScrollY();
        const float line = base * 3, pageStep = viewH * 0.9f;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) y += line;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) y -= line;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown) || (ImGui::IsKeyPressed(ImGuiKey_Space) && !io.KeyShift)) y += pageStep;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp) || (ImGui::IsKeyPressed(ImGuiKey_Space) && io.KeyShift)) y -= pageStep;
        if (ImGui::IsKeyPressed(ImGuiKey_Home)) y = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_End)) y = ImGui::GetScrollMaxY();
        if (ImGui::IsKeyPressed(ImGuiKey_Backspace) || (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && io.KeyAlt)) GoBack(view, m);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && m.findOpen) CloseFind(m);
        if (y != ImGui::GetScrollY()) ImGui::SetScrollY(std::clamp(y, 0.0f, ImGui::GetScrollMaxY()));
        }
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && ImGui::IsMouseClicked(ImGuiMouseButton(3))) GoBack(view, m);

    const float scrollY = ImGui::GetScrollY();
    m.scrollY = scrollY;
    DrawContext c;
    c.state = &state;
    c.ui = &ui;
    c.view = &view;
    c.m = &m;
    c.dl = ImGui::GetWindowDrawList();
    c.base = base;
    c.origin = ImVec2(window->InnerRect.Min.x + std::floor((viewW - column) * 0.5f), window->InnerRect.Min.y + margin - scrollY);
    c.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Only the blocks in view are drawn; the rest are kept laid out.
    const float top = scrollY - margin - base * 4, bottom = scrollY + viewH + base * 4;
    for (size_t i = 0; i < m.layout.size(); ++i)
        {
        const BlockLayout& L = m.layout[i];
        if (L.y + L.h < top || L.y > bottom) continue;
        DrawBlock(c, i);
        }
    if (m.doc.blocks.empty()) c.dl->AddText(ImVec2(c.origin.x, c.origin.y), CurrentPalette().textFaint, "This file is empty.");
    ImGui::SetCursorPos(ImVec2(0, m.totalHeight + margin * 2));
    ImGui::Dummy(ImVec2(1, 1));

    // Links: pointer, tooltip, click, and a menu to copy the address.
    m.hoveredLink = c.hoverLink;
    if (c.hoverLink >= 0)
        {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const std::string& href = m.doc.links[(size_t)c.hoverLink];
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) ImGui::SetTooltip("%s", href.c_str());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) m.pressedLink = c.hoverLink;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
            m.contextLink = href;
            ImGui::OpenPopup("##link");
            }
        }
    else if (c.hoverPicture >= 0)
        {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) OpenFileView(state, ui, m.pictures[(size_t)c.hoverPicture].path);
        }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
        const int pressed = m.pressedLink;
        m.pressedLink = -1;
        if (pressed >= 0 && pressed == c.hoverLink && !ImGui::IsMouseDragPastThreshold(ImGuiMouseButton_Left))
            {
            const std::string href = m.doc.links[(size_t)pressed];   // a copy: following it may reload the document
            FollowLink(state, ui, view, m, href);
            }
        }
    if (ImGui::BeginPopup("##link"))
        {
        if (ImGui::MenuItem("Open link")) FollowLink(state, ui, view, m, m.contextLink);
        if (ImGui::MenuItem("Copy link address")) ImGui::SetClipboardText(m.contextLink.c_str());
        ImGui::EndPopup();
        }
    ImGui::EndChild();
}

void DrawStatusBar(const MarkdownViewUi& m)
{
    // Words, as a reader's measure of length.
    size_t words = 0;
    bool inWord = false;
    for (char ch : m.source)
        {
        const bool letter = !IsSpace(ch);
        if (letter && !inWord) ++words;
        inWord = letter;
        }
    std::string text = "Markdown document";
    if (m.fileBytes > 0) text += "    " + app::HumanSize(m.fileBytes);
    text += "    " + std::to_string(words) + (words == 1 ? " word" : " words");
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    MutedText(text.c_str());
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

} // namespace

MarkdownViewInfo DescribeMarkdownView(const FileViewUi& v)
{
    MarkdownViewInfo info;
    if (!v.markdown) return info;
    info.blocks = v.markdown->doc.blocks.size();
    info.hits = v.markdown->hits.size();
    info.history = v.markdown->history.size();
    info.error = v.markdown->error;
    return info;
}

void DrawMarkdownFile(app::AppState& state, UiState& ui, app::FileView& view, FileViewUi& v)
{
    if (!v.markdown) v.markdown = std::make_shared<MarkdownViewUi>();
    MarkdownViewUi& m = *v.markdown;
    if (m.loadedPath != view.path) Load(m, view.path, m.scrollTo >= 0);
    else ReloadIfChanged(m);

    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (focused && !m.showSource && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) OpenFind(m);
    if (focused && m.findOpen && (ImGui::IsKeyPressed(ImGuiKey_F3) || ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_F3)))
        StepHit(m, ImGui::GetIO().KeyShift ? -1 : 1);

    DrawToolbar(state, ui, view, m);
    if (m.findOpen && !m.showSource) DrawFindBar(m);

    const float statusHeight = state.settings.showStatusBar ? ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2 : 0.0f;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(avail.x, std::max(avail.y - statusHeight, 1.0f));
    if (!m.error.empty()) DrawMessage(m.error, size);
    else if (m.showSource) DrawSource(m, size);
    else DrawDocument(state, ui, view, m, size);
    if (state.settings.showStatusBar) DrawStatusBar(m);
}

} // namespace ui
