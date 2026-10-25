// Markdown files for the built-in viewer. md4c parses the text; what comes
// out is a flat list of blocks, each carrying the quote and list nesting it
// sits in, so the interface lays them out top to bottom without a tree walk.
// No ImGui here: the tests check the structure without a window.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace app::md
{

// .md, .markdown and the other extensions people give Markdown files. The
// extension is lower-case without the dot, as a FileEntry carries it.
bool IsMarkdown(std::string_view extension);

enum SpanStyle : uint8_t
{
    kBold      = 1,
    kItalic    = 2,
    kCode      = 4,   // `inline code`
    kStrike    = 8,
    kUnderline = 16,
};

enum class SpanKind : uint8_t
{
    Text,
    SoftBreak,     // a newline inside a paragraph: drawn as a space
    HardBreak,     // two trailing spaces, a backslash, or <br>
    Image,         // `text` is the alt text, `image` the picture
};

struct Span
{
    SpanKind    kind = SpanKind::Text;
    std::string text;
    uint8_t     style = 0;
    int         link = -1;    // index into Document::links, -1 when not a link
    int         image = -1;   // index into Document::images
};

enum class Align : uint8_t { Default, Left, Center, Right };

enum class BlockKind : uint8_t
{
    Paragraph,
    Heading,
    Code,          // fenced or indented
    Rule,
    Table,
};

using Cell = std::vector<Span>;

struct Block
{
    BlockKind kind = BlockKind::Paragraph;

    // Where it sits: inside how many quotes, how many lists deep. A block
    // that opens a list item carries the item's marker; the item's later
    // blocks line up under its text.
    int         quoteDepth = 0;
    int         listDepth = 0;
    bool        itemStart = false;
    std::string marker;      // "•", "◦", "▪", or "3." for an ordered list
    int         task = 0;    // 0 not a task item, 1 open "[ ]", 2 done "[x]"
    bool        tight = false;   // inside a tight list: no paragraph gap after
    bool        centered = false;   // HTML's align="center", as READMEs use for a logo

    std::vector<Span> spans;     // Paragraph, Heading
    int               level = 0; // Heading: 1..6
    std::string       anchor;    // Heading: the GitHub-style slug "#getting-started" links to

    std::string code;            // Code: the lines, newline-separated, without the last newline
    std::string language;        // Code: the fence's info word ("cpp"), may be empty

    // Table: rows of cells; the first headerRows are the header.
    std::vector<std::vector<Cell>> rows;
    std::vector<Align>             align;
    int                            headerRows = 0;
};

struct Document
{
    std::vector<Block>       blocks;
    std::vector<std::string> links;     // href as written (percent-encoding and all)
    std::vector<std::string> images;    // src as written
};

// Parses GitHub-flavoured Markdown: tables, task lists, strikethrough and
// bare URLs, as GitHub renders a README. A YAML front-matter block at the
// top is kept as a code block rather than read as a heading and rules.
// Raw HTML is reduced to what reads as text, keeping <img> and <br>.
Document Parse(std::string_view text);

// GitHub's heading anchors: lower-case, punctuation dropped, spaces to
// hyphens. Duplicates get -1, -2... from Parse.
std::string Slug(std::string_view headingText);

// The plain text of some spans, images as their alt text.
std::string PlainText(const std::vector<Span>& spans);

// Where a link in the document at docPath goes.
struct LinkTarget
{
    enum class Kind { Anchor, Web, File, Unsupported } kind = Kind::Unsupported;
    std::string url;      // Web: as written
    std::string path;     // File: absolute, percent-decoded
    std::string anchor;   // Anchor or File: the part after '#', without it
};
LinkTarget ResolveLink(const std::string& docPath, const std::string& href);

// A picture's file, if it is a local one: relative to the document, or an
// absolute path or file:// URL. Empty for web images and data: URIs.
std::string ResolveImagePath(const std::string& docPath, const std::string& src);

} // namespace app::md
