#include "app/Markdown.h"
#include "app/PathUtil.h"

#include "md4c.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <unordered_map>

namespace app::md
{

bool IsMarkdown(std::string_view extension)
{
    static constexpr std::string_view kExts[] = { "md", "markdown", "mdown", "mkd", "mkdn", "mdwn" };
    for (std::string_view e : kExts)
        if (extension == e) return true;
    return false;
}

namespace
{

std::string ToLower(std::string_view s)
{
    std::string out(s);
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

void AppendUtf8(std::string& out, unsigned cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
    else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

// "&amp;" -> "&". The named ones a README is likely to hold; numeric ones
// in full. Anything else is left as written.
std::string DecodeEntity(std::string_view e)
{
    if (e.size() >= 4 && e[0] == '&' && e[1] == '#' && e.back() == ';')
        {
        const bool hex = e[2] == 'x' || e[2] == 'X';
        const std::string digits(e.substr(hex ? 3 : 2, e.size() - (hex ? 4 : 3)));
        if (digits.empty()) return std::string(e);
        char* end = nullptr;
        const unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
        if (end == nullptr || *end != '\0') return std::string(e);
        std::string out;
        AppendUtf8(out, (unsigned)cp);
        return out;
        }
    static const std::unordered_map<std::string_view, const char*> kNamed = {
        { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" },
        { "&nbsp;", "\xC2\xA0" }, { "&copy;", "\xC2\xA9" }, { "&reg;", "\xC2\xAE" }, { "&trade;", "\xE2\x84\xA2" },
        { "&hellip;", "\xE2\x80\xA6" }, { "&mdash;", "\xE2\x80\x94" }, { "&ndash;", "\xE2\x80\x93" },
        { "&larr;", "\xE2\x86\x90" }, { "&rarr;", "\xE2\x86\x92" }, { "&uarr;", "\xE2\x86\x91" }, { "&darr;", "\xE2\x86\x93" },
        { "&times;", "\xC3\x97" }, { "&middot;", "\xC2\xB7" }, { "&bull;", "\xE2\x80\xA2" }, { "&deg;", "\xC2\xB0" },
        { "&laquo;", "\xC2\xAB" }, { "&raquo;", "\xC2\xBB" }, { "&lsquo;", "\xE2\x80\x98" }, { "&rsquo;", "\xE2\x80\x99" },
        { "&ldquo;", "\xE2\x80\x9C" }, { "&rdquo;", "\xE2\x80\x9D" }, { "&check;", "\xE2\x9C\x93" },
    };
    const auto it = kNamed.find(e);
    return it == kNamed.end() ? std::string(e) : std::string(it->second);
}

// Entities inside a run of text, for attribute values and HTML text.
std::string DecodeEntities(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();)
        {
        if (s[i] == '&')
            {
            const size_t semi = s.find(';', i);
            if (semi != std::string_view::npos && semi - i <= 10)
                {
                out += DecodeEntity(s.substr(i, semi - i + 1));
                i = semi + 1;
                continue;
                }
            }
        out += s[i++];
        }
    return out;
}

std::string Attribute(const MD_ATTRIBUTE& a)
{
    return a.text ? DecodeEntities(std::string_view(a.text, a.size)) : std::string();
}

int PercentDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string PercentDecode(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        {
        if (s[i] == '%' && i + 2 < s.size() && PercentDigit(s[i + 1]) >= 0 && PercentDigit(s[i + 2]) >= 0)
            {
            out += (char)(PercentDigit(s[i + 1]) * 16 + PercentDigit(s[i + 2]));
            i += 2;
            }
        else
            {
            out += s[i];
            }
        }
    return out;
}

// "https://x" -> "https"; "docs/a.md" and "C:\a.md" -> "".
std::string Scheme(std::string_view href)
{
    size_t i = 0;
    while (i < href.size() && (std::isalnum((unsigned char)href[i]) || href[i] == '+' || href[i] == '-' || href[i] == '.')) ++i;
    if (i < 2 || i >= href.size() || href[i] != ':' || !std::isalpha((unsigned char)href[0])) return {};
    return ToLower(href.substr(0, i));
}

std::filesystem::path U8Path(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string FromU8Path(const std::filesystem::path& p)
{
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

// A link or image path relative to the document, or absolute, made into
// an absolute path on disk.
std::string LocalPath(const std::string& docPath, std::string_view rel)
{
    const std::string decoded = PercentDecode(rel);
    std::filesystem::path p = U8Path(decoded);
    if (!p.is_absolute())
        {
#ifdef _WIN32
        // "/docs/x.md" means the drive's root on Windows, as it does to a browser.
        if (!decoded.empty() && (decoded[0] == '/' || decoded[0] == '\\')) p = U8Path(docPath).root_path() / p.relative_path();
        else
#endif
        p = U8Path(ParentPath(docPath)) / p;
        }
    return FromU8Path(p.lexically_normal());
}

std::string FilePathFromUrl(std::string_view url)
{
    // file:///home/x or file://localhost/home/x; on Windows file:///C:/x.
    std::string_view rest = url.substr(std::strlen("file://"));
    if (rest.rfind("localhost", 0) == 0) rest.remove_prefix(std::strlen("localhost"));
    std::string path = PercentDecode(rest);
#ifdef _WIN32
    if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
    for (char& c : path) if (c == '/') c = '\\';
#endif
    return path;
}

struct ListInfo
{
    bool     ordered = false;
    unsigned next = 1;
    char     delimiter = '.';
    bool     tight = false;
};

// md4c reports the document as a stream of enter/leave/text events; this
// turns it into blocks.
class Builder
{
public:
    Document doc;

    // ---- md4c events ----

    void EnterBlock(MD_BLOCKTYPE type, void* detail)
    {
        switch (type)
            {
            case MD_BLOCK_DOC: break;
            case MD_BLOCK_QUOTE: leaf_ = -1; ++quoteDepth_; break;
            case MD_BLOCK_UL:
                {
                leaf_ = -1;
                ListInfo l;
                l.tight = static_cast<MD_BLOCK_UL_DETAIL*>(detail)->is_tight != 0;
                lists_.push_back(l);
                break;
                }
            case MD_BLOCK_OL:
                {
                leaf_ = -1;
                const auto* d = static_cast<MD_BLOCK_OL_DETAIL*>(detail);
                ListInfo l;
                l.ordered = true;
                l.next = d->start;
                l.delimiter = d->mark_delimiter;
                l.tight = d->is_tight != 0;
                lists_.push_back(l);
                break;
                }
            case MD_BLOCK_LI:
                {
                leaf_ = -1;
                const auto* d = static_cast<MD_BLOCK_LI_DETAIL*>(detail);
                itemPending_ = true;
                itemTask_ = d->is_task ? ((d->task_mark == 'x' || d->task_mark == 'X') ? 2 : 1) : 0;
                itemMarker_.clear();
                if (!lists_.empty() && lists_.back().ordered)
                    itemMarker_ = std::to_string(lists_.back().next++) + lists_.back().delimiter;
                break;
                }
            case MD_BLOCK_HR: leaf_ = -1; NewBlock(BlockKind::Rule); break;
            case MD_BLOCK_H:
                leaf_ = NewBlock(BlockKind::Heading);
                doc.blocks[(size_t)leaf_].level = (int)static_cast<MD_BLOCK_H_DETAIL*>(detail)->level;
                break;
            case MD_BLOCK_CODE:
                leaf_ = NewBlock(BlockKind::Code);
                doc.blocks[(size_t)leaf_].language = Attribute(static_cast<MD_BLOCK_CODE_DETAIL*>(detail)->lang);
                inCode_ = true;
                break;
            case MD_BLOCK_HTML: leaf_ = -1; inHtmlBlock_ = true; html_.clear(); break;
            case MD_BLOCK_P: leaf_ = NewBlock(BlockKind::Paragraph); break;
            case MD_BLOCK_TABLE: leaf_ = NewBlock(BlockKind::Table); table_ = leaf_; break;
            case MD_BLOCK_THEAD: inHead_ = true; break;
            case MD_BLOCK_TBODY: inHead_ = false; break;
            case MD_BLOCK_TR:
                if (table_ >= 0)
                    {
                    Block& t = doc.blocks[(size_t)table_];
                    t.rows.emplace_back();
                    if (inHead_) ++t.headerRows;
                    }
                break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD:
                if (table_ >= 0 && !doc.blocks[(size_t)table_].rows.empty())
                    {
                    Block& t = doc.blocks[(size_t)table_];
                    t.rows.back().emplace_back();
                    if (t.rows.size() == 1)
                        {
                        Align a = Align::Default;
                        switch (static_cast<MD_BLOCK_TD_DETAIL*>(detail)->align)
                            {
                            case MD_ALIGN_LEFT: a = Align::Left; break;
                            case MD_ALIGN_CENTER: a = Align::Center; break;
                            case MD_ALIGN_RIGHT: a = Align::Right; break;
                            default: break;
                            }
                        t.align.push_back(a);
                        }
                    inCell_ = true;
                    }
                break;
            }
    }

    void LeaveBlock(MD_BLOCKTYPE type)
    {
        switch (type)
            {
            case MD_BLOCK_QUOTE: leaf_ = -1; if (quoteDepth_ > 0) --quoteDepth_; break;
            case MD_BLOCK_UL:
            case MD_BLOCK_OL: leaf_ = -1; if (!lists_.empty()) lists_.pop_back(); break;
            case MD_BLOCK_LI:
                // An empty item still shows its marker.
                if (itemPending_) NewBlock(BlockKind::Paragraph);
                itemPending_ = false;
                leaf_ = -1;
                break;
            case MD_BLOCK_H:
                if (leaf_ >= 0) SetAnchor(doc.blocks[(size_t)leaf_]);
                leaf_ = -1;
                break;
            case MD_BLOCK_CODE:
                if (leaf_ >= 0)
                    {
                    std::string& code = doc.blocks[(size_t)leaf_].code;
                    while (!code.empty() && (code.back() == '\n' || code.back() == '\r')) code.pop_back();
                    }
                inCode_ = false;
                leaf_ = -1;
                break;
            case MD_BLOCK_HTML:
                inHtmlBlock_ = false;
                Html(html_, true);
                leaf_ = -1;
                centerNext_ = false;
                break;
            case MD_BLOCK_P: leaf_ = -1; break;
            case MD_BLOCK_TH:
            case MD_BLOCK_TD: inCell_ = false; break;
            case MD_BLOCK_TABLE: table_ = -1; leaf_ = -1; inHead_ = false; break;
            default: break;
            }
    }

    void EnterSpan(MD_SPANTYPE type, void* detail)
    {
        switch (type)
            {
            case MD_SPAN_EM: ++italic_; break;
            case MD_SPAN_STRONG: ++bold_; break;
            case MD_SPAN_CODE: ++code_; break;
            case MD_SPAN_DEL: ++strike_; break;
            case MD_SPAN_U: ++underline_; break;
            case MD_SPAN_A:
                doc.links.push_back(Attribute(static_cast<MD_SPAN_A_DETAIL*>(detail)->href));
                linkStack_.push_back((int)doc.links.size() - 1);
                break;
            case MD_SPAN_IMG:
                if (imgDepth_++ == 0)
                    {
                    alt_.clear();
                    imgSrc_ = Attribute(static_cast<MD_SPAN_IMG_DETAIL*>(detail)->src);
                    }
                break;
            default: break;
            }
    }

    void LeaveSpan(MD_SPANTYPE type)
    {
        switch (type)
            {
            case MD_SPAN_EM: --italic_; break;
            case MD_SPAN_STRONG: --bold_; break;
            case MD_SPAN_CODE: --code_; break;
            case MD_SPAN_DEL: --strike_; break;
            case MD_SPAN_U: --underline_; break;
            case MD_SPAN_A: if (!linkStack_.empty()) linkStack_.pop_back(); break;
            case MD_SPAN_IMG:
                if (--imgDepth_ == 0) AddImage(imgSrc_, alt_);
                break;
            default: break;
            }
    }

    void Text(MD_TEXTTYPE type, std::string_view text)
    {
        if (inHtmlBlock_)
            {
            html_.append(text);
            return;
            }
        if (inCode_ && leaf_ >= 0)
            {
            doc.blocks[(size_t)leaf_].code.append(text);
            return;
            }
        switch (type)
            {
            case MD_TEXT_NULLCHAR: AddText("\xEF\xBF\xBD"); break;
            case MD_TEXT_BR: AddBreak(true); break;
            case MD_TEXT_SOFTBR: AddBreak(false); break;
            case MD_TEXT_ENTITY: AddText(DecodeEntity(text)); break;
            case MD_TEXT_HTML: Html(text, false); break;
            default: AddText(text); break;
            }
    }

    // Front matter goes in as a code block before md4c sees the rest.
    void FrontMatter(std::string_view yaml)
    {
        const int i = NewBlock(BlockKind::Code);
        doc.blocks[(size_t)i].code = std::string(yaml);
        doc.blocks[(size_t)i].language = "yaml";
    }

private:
    int  leaf_ = -1;        // the block taking text, -1 for none
    int  table_ = -1;
    bool inHead_ = false;
    bool inCell_ = false;
    bool inCode_ = false;
    bool inHtmlBlock_ = false;
    std::string html_;
    bool centerNext_ = false;

    int quoteDepth_ = 0;
    std::vector<ListInfo> lists_;
    bool        itemPending_ = false;
    std::string itemMarker_;
    int         itemTask_ = 0;

    int bold_ = 0, italic_ = 0, code_ = 0, strike_ = 0, underline_ = 0;
    std::vector<int> linkStack_;
    int htmlLinks_ = 0;     // links opened by <a>, which only </a> may close

    int         imgDepth_ = 0;
    std::string imgSrc_;
    std::string alt_;

    std::unordered_map<std::string, int> anchors_;

    int NewBlock(BlockKind kind)
    {
        Block b;
        b.kind = kind;
        b.quoteDepth = quoteDepth_;
        b.listDepth = (int)lists_.size();
        b.tight = !lists_.empty() && lists_.back().tight;
        b.centered = centerNext_;
        if (itemPending_)
            {
            b.itemStart = true;
            b.marker = itemMarker_;
            b.task = itemTask_;
            itemPending_ = false;
            }
        doc.blocks.push_back(std::move(b));
        return (int)doc.blocks.size() - 1;
    }

    void SetAnchor(Block& b)
    {
        std::string slug = Slug(PlainText(b.spans));
        const int seen = anchors_[slug]++;
        if (seen > 0) slug += "-" + std::to_string(seen);
        b.anchor = slug;
    }

    uint8_t Style() const
    {
        return (uint8_t)((bold_ > 0 ? kBold : 0) | (italic_ > 0 ? kItalic : 0) | (code_ > 0 ? kCode : 0) |
                         (strike_ > 0 ? kStrike : 0) | (underline_ > 0 ? kUnderline : 0));
    }

    int Link() const { return linkStack_.empty() ? -1 : linkStack_.back(); }

    // Where inline content goes: the open table cell, the open block, or a
    // paragraph opened for it -- a tight list item's text arrives with no
    // paragraph around it, and so does text in an HTML block.
    std::vector<Span>* Target()
    {
        if (inCell_ && table_ >= 0)
            {
            Block& t = doc.blocks[(size_t)table_];
            if (!t.rows.empty() && !t.rows.back().empty()) return &t.rows.back().back();
            }
        if (leaf_ < 0 || doc.blocks[(size_t)leaf_].kind == BlockKind::Table || doc.blocks[(size_t)leaf_].kind == BlockKind::Rule)
            leaf_ = NewBlock(BlockKind::Paragraph);
        return &doc.blocks[(size_t)leaf_].spans;
    }

    void AddText(std::string_view text)
    {
        if (text.empty()) return;
        if (imgDepth_ > 0)
            {
            alt_.append(text);
            return;
            }
        // Whitespace alone does not open a paragraph (HTML blocks are full of it).
        if (leaf_ < 0 && !inCell_ && text.find_first_not_of(" \t\r\n") == std::string_view::npos) return;
        std::vector<Span>& spans = *Target();
        const uint8_t style = Style();
        const int link = Link();
        if (!spans.empty() && spans.back().kind == SpanKind::Text && spans.back().style == style && spans.back().link == link)
            {
            spans.back().text.append(text);
            return;
            }
        Span s;
        s.text = std::string(text);
        s.style = style;
        s.link = link;
        spans.push_back(std::move(s));
    }

    void AddBreak(bool hard)
    {
        if (imgDepth_ > 0)
            {
            alt_ += ' ';
            return;
            }
        if (leaf_ < 0 && !inCell_) return;
        Span s;
        s.kind = hard ? SpanKind::HardBreak : SpanKind::SoftBreak;
        s.link = Link();
        Target()->push_back(s);
    }

    void AddImage(const std::string& src, const std::string& alt)
    {
        doc.images.push_back(src);
        Span s;
        s.kind = SpanKind::Image;
        s.text = alt;
        s.style = Style();
        s.link = Link();
        s.image = (int)doc.images.size() - 1;
        Target()->push_back(std::move(s));
    }

    // ---- HTML ----
    //
    // Not a browser: tags that carry meaning a Markdown reader would see
    // (images, line breaks, links, emphasis, headings) are kept, the rest
    // are dropped and their text shown. In a block, block-level tags start
    // a new paragraph.

    struct Tag
    {
        std::string name;    // lower-case, no '/'
        bool        closing = false;
        std::unordered_map<std::string, std::string> attrs;
    };

    static bool ParseTag(std::string_view s, Tag& tag)
    {
        size_t i = 1;   // past '<'
        if (i < s.size() && s[i] == '/') { tag.closing = true; ++i; }
        const size_t nameStart = i;
        while (i < s.size() && (std::isalnum((unsigned char)s[i]) || s[i] == '-')) ++i;
        if (i == nameStart) return false;
        tag.name = ToLower(s.substr(nameStart, i - nameStart));
        while (i < s.size())
            {
            while (i < s.size() && (std::isspace((unsigned char)s[i]) || s[i] == '/')) ++i;
            if (i >= s.size() || s[i] == '>') break;
            const size_t an = i;
            while (i < s.size() && !std::isspace((unsigned char)s[i]) && s[i] != '=' && s[i] != '>' && s[i] != '/') ++i;
            std::string name = ToLower(s.substr(an, i - an));
            std::string value;
            if (i < s.size() && s[i] == '=')
                {
                ++i;
                if (i < s.size() && (s[i] == '"' || s[i] == '\''))
                    {
                    const char q = s[i++];
                    const size_t vs = i;
                    while (i < s.size() && s[i] != q) ++i;
                    value = DecodeEntities(s.substr(vs, i - vs));
                    if (i < s.size()) ++i;
                    }
                else
                    {
                    const size_t vs = i;
                    while (i < s.size() && !std::isspace((unsigned char)s[i]) && s[i] != '>') ++i;
                    value = DecodeEntities(s.substr(vs, i - vs));
                    }
                }
            if (!name.empty()) tag.attrs[name] = value;
            }
        return true;
    }

    static bool IsBlockTag(const std::string& n)
    {
        static const char* kBlock[] = { "p", "div", "ul", "ol", "li", "table", "thead", "tbody", "tr", "blockquote",
                                        "center", "pre", "details", "summary", "section", "article", "header",
                                        "footer", "nav", "figure", "figcaption", "dl", "dt", "dd" };
        for (const char* b : kBlock)
            if (n == b) return true;
        return false;
    }

    static void Bump(int& counter, bool closing)
    {
        counter = closing ? std::max(0, counter - 1) : counter + 1;
    }

    void HtmlTag(const Tag& tag, bool block)
    {
        const std::string& n = tag.name;
        if (n == "br") { AddBreak(true); return; }
        if (n == "img")
            {
            const auto src = tag.attrs.find("src");
            if (src == tag.attrs.end()) return;
            const auto alt = tag.attrs.find("alt");
            AddImage(src->second, alt == tag.attrs.end() ? std::string() : alt->second);
            return;
            }
        if (n == "a")
            {
            if (!tag.closing)
                {
                const auto href = tag.attrs.find("href");
                if (href == tag.attrs.end()) return;
                doc.links.push_back(href->second);
                linkStack_.push_back((int)doc.links.size() - 1);
                ++htmlLinks_;
                }
            else if (htmlLinks_ > 0 && !linkStack_.empty())
                {
                linkStack_.pop_back();
                --htmlLinks_;
                }
            return;
            }
        if (n == "b" || n == "strong") { Bump(bold_, tag.closing); return; }
        if (n == "i" || n == "em") { Bump(italic_, tag.closing); return; }
        if (n == "code" || n == "kbd" || n == "tt" || n == "samp") { Bump(code_, tag.closing); return; }
        if (n == "s" || n == "del" || n == "strike") { Bump(strike_, tag.closing); return; }
        if (n == "u" || n == "ins") { Bump(underline_, tag.closing); return; }
        if (!block) return;

        const auto align = tag.attrs.find("align");
        const bool center = n == "center" || (align != tag.attrs.end() && ToLower(align->second) == "center");
        if (n.size() == 2 && n[0] == 'h' && n[1] >= '1' && n[1] <= '6')
            {
            if (!tag.closing)
                {
                centerNext_ = center;
                leaf_ = NewBlock(BlockKind::Heading);
                doc.blocks[(size_t)leaf_].level = n[1] - '0';
                }
            else if (leaf_ >= 0 && doc.blocks[(size_t)leaf_].kind == BlockKind::Heading)
                {
                SetAnchor(doc.blocks[(size_t)leaf_]);
                leaf_ = -1;
                }
            return;
            }
        if (n == "hr") { leaf_ = -1; NewBlock(BlockKind::Rule); return; }
        if (IsBlockTag(n))
            {
            leaf_ = -1;
            if (!tag.closing) centerNext_ = center;
            }
    }

    void Html(std::string_view s, bool block)
    {
        std::string text;
        auto flush = [&] {
            if (text.empty()) return;
            AddText(DecodeEntities(text));
            text.clear();
        };
        for (size_t i = 0; i < s.size();)
            {
            if (s[i] == '<')
                {
                if (s.substr(i, 4) == "<!--")
                    {
                    const size_t end = s.find("-->", i + 4);
                    i = end == std::string_view::npos ? s.size() : end + 3;
                    continue;
                    }
                // The tag ends at the first '>' outside quotes.
                size_t j = i + 1;
                char quote = 0;
                while (j < s.size() && (quote || s[j] != '>'))
                    {
                    if (quote && s[j] == quote) quote = 0;
                    else if (!quote && (s[j] == '"' || s[j] == '\'')) quote = s[j];
                    ++j;
                    }
                Tag tag;
                if (j < s.size() && ParseTag(s.substr(i, j - i + 1), tag))
                    {
                    flush();
                    HtmlTag(tag, block);
                    i = j + 1;
                    continue;
                    }
                }
            // HTML collapses whitespace; so does this.
            const char c = s[i++];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                {
                if (!block || !text.empty() || leaf_ >= 0)
                    if (text.empty() || text.back() != ' ') text += ' ';
                }
            else
                {
                text += c;
                }
            }
        flush();
    }
};

int OnEnterBlock(MD_BLOCKTYPE type, void* detail, void* user) { static_cast<Builder*>(user)->EnterBlock(type, detail); return 0; }
int OnLeaveBlock(MD_BLOCKTYPE type, void*, void* user) { static_cast<Builder*>(user)->LeaveBlock(type); return 0; }
int OnEnterSpan(MD_SPANTYPE type, void* detail, void* user) { static_cast<Builder*>(user)->EnterSpan(type, detail); return 0; }
int OnLeaveSpan(MD_SPANTYPE type, void*, void* user) { static_cast<Builder*>(user)->LeaveSpan(type); return 0; }
int OnText(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* user)
{
    static_cast<Builder*>(user)->Text(type, std::string_view(text, size));
    return 0;
}

// The lines between a leading "---" and the next "---" or "...", or npos
// when the text does not start with front matter.
size_t FrontMatterEnd(std::string_view text, std::string_view& yaml)
{
    if (text.rfind("---\n", 0) != 0 && text.rfind("---\r\n", 0) != 0) return std::string_view::npos;
    size_t pos = text.find('\n') + 1;
    const size_t start = pos;
    while (pos < text.size())
        {
        size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) eol = text.size();
        std::string_view line = text.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "---" || line == "...")
            {
            yaml = text.substr(start, pos > start ? pos - start - 1 : 0);
            if (!yaml.empty() && yaml.back() == '\r') yaml.remove_suffix(1);
            return std::min(eol + 1, text.size());
            }
        pos = eol + 1;
        }
    return std::string_view::npos;
}

} // namespace

Document Parse(std::string_view text)
{
    Builder b;
    // A byte-order mark is not part of the first heading.
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.remove_prefix(3);
    std::string_view yaml;
    const size_t bodyStart = FrontMatterEnd(text, yaml);
    if (bodyStart != std::string_view::npos)
        {
        b.FrontMatter(yaml);
        text.remove_prefix(bodyStart);
        }

    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_DIALECT_GITHUB;
    parser.enter_block = OnEnterBlock;
    parser.leave_block = OnLeaveBlock;
    parser.enter_span = OnEnterSpan;
    parser.leave_span = OnLeaveSpan;
    parser.text = OnText;
    md_parse(text.data(), (MD_SIZE)text.size(), &parser, &b);
    return std::move(b.doc);
}

std::string Slug(std::string_view headingText)
{
    std::string out;
    out.reserve(headingText.size());
    for (char ch : headingText)
        {
        const unsigned char c = (unsigned char)ch;
        if (c >= 0x80) out += ch;   // letters in other scripts stay, as on GitHub
        else if (std::isalnum(c)) out += (char)std::tolower(c);
        else if (c == ' ') out += '-';
        else if (c == '-' || c == '_') out += ch;
        }
    return out;
}

std::string PlainText(const std::vector<Span>& spans)
{
    std::string out;
    for (const Span& s : spans)
        out += (s.kind == SpanKind::SoftBreak || s.kind == SpanKind::HardBreak) ? std::string(" ") : s.text;
    return out;
}

LinkTarget ResolveLink(const std::string& docPath, const std::string& href)
{
    LinkTarget t;
    if (href.empty()) return t;
    if (href[0] == '#')
        {
        t.kind = LinkTarget::Kind::Anchor;
        t.anchor = PercentDecode(href.substr(1));
        return t;
        }
    const std::string scheme = Scheme(href);
    if (scheme == "http" || scheme == "https")
        {
        t.kind = LinkTarget::Kind::Web;
        t.url = href;
        return t;
        }
    std::string rest = href;
    const size_t hash = rest.find('#');
    if (hash != std::string::npos)
        {
        t.anchor = PercentDecode(rest.substr(hash + 1));
        rest.erase(hash);
        }
    if (scheme == "file")
        {
        t.kind = LinkTarget::Kind::File;
        t.path = FilePathFromUrl(rest);
        return t;
        }
    if (!scheme.empty()) return t;   // mailto:, ftp:, javascript: and the rest
    const size_t query = rest.find('?');
    if (query != std::string::npos) rest.erase(query);
    if (rest.empty())
        {
        // "?x#y" or "#" alone: an anchor in this document.
        t.kind = LinkTarget::Kind::Anchor;
        return t;
        }
    t.kind = LinkTarget::Kind::File;
    t.path = LocalPath(docPath, rest);
    return t;
}

std::string ResolveImagePath(const std::string& docPath, const std::string& src)
{
    if (src.empty()) return {};
    const std::string scheme = Scheme(src);
    if (scheme == "file") return FilePathFromUrl(src);
    if (!scheme.empty()) return {};
    std::string rest = src;
    const size_t cut = rest.find_first_of("?#");
    if (cut != std::string::npos) rest.erase(cut);
    return rest.empty() ? std::string() : LocalPath(docPath, rest);
}

} // namespace app::md
