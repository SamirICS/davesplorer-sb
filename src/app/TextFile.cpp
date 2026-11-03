#include "app/TextFile.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>

namespace app::text
{

bool IsTextExtension(std::string_view extension)
{
    static constexpr std::string_view kExts[] = {
        // Plain text and logs.
        "txt", "text", "log", "rst", "adoc", "org", "tex", "nfo",
        // Data and configuration.
        "json", "jsonc", "json5", "ndjson", "jsonl", "geojson", "xml", "xsd", "xsl", "xslt", "yaml", "yml",
        "toml", "ini", "cfg", "conf", "config", "properties", "env", "csv", "tsv", "sql", "graphql", "proto",
        "reg", "lock",
        // Shell and build.
        "sh", "bash", "zsh", "fish", "ksh", "csh", "bat", "cmd", "ps1", "psm1",
        "cmake", "mk", "mak", "make", "gradle", "bzl", "bazel", "ninja", "meson", "service", "rules",
        // Source.
        "py", "pyi", "pyw", "c", "h", "cpp", "hpp", "cc", "hh", "cxx", "hxx", "inl", "ipp", "m", "mm",
        "cs", "java", "kt", "kts", "scala", "groovy", "go", "rs", "swift", "dart", "zig", "nim", "d",
        "js", "mjs", "cjs", "jsx", "ts", "tsx", "vue", "svelte", "css", "scss", "sass", "less",
        "rb", "pl", "pm", "php", "lua", "r", "jl", "hs", "ml", "mli", "fs", "fsx", "ex", "exs", "erl",
        "clj", "lisp", "el", "scm", "vim", "awk", "sed", "tcl", "asm", "s", "v", "sv", "vhd", "vhdl",
        "glsl", "hlsl", "frag", "vert", "wgsl", "cu", "cl", "qml", "ui", "qrc", "pro", "pri",
        // Version control.
        "diff", "patch", "gitignore", "gitattributes", "gitmodules", "editorconfig", "clang-format",
        // Automotive: bus logs, databases and flash images that are text.
        "asc", "dbc", "arxml", "a2l", "odx", "ldf", "srec", "s19", "s28", "s37", "hex", "cdd",
    };
    for (std::string_view e : kExts)
        if (extension == e) return true;
    return false;
}

namespace
{

constexpr size_t kSniffBytes = 8 * 1024;
constexpr char32_t kReplacement = 0xFFFD;

std::filesystem::path U8(const std::string& s)
{
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

// The length of the UTF-8 sequence starting at bytes[i], or 0 when it is
// not a valid one. `truncated` is set when it is cut off by the end.
size_t Utf8SequenceAt(std::string_view bytes, size_t i, char32_t& cp, bool& truncated)
{
    truncated = false;
    const unsigned char b0 = (unsigned char)bytes[i];
    if (b0 < 0x80)
        {
        cp = b0;
        return 1;
        }
    size_t len;
    char32_t min;
    if ((b0 & 0xE0) == 0xC0) { len = 2; cp = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { len = 3; cp = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { len = 4; cp = b0 & 0x07; min = 0x10000; }
    else return 0;
    for (size_t k = 1; k < len; ++k)
        {
        if (i + k >= bytes.size())
            {
            truncated = true;
            return 0;
            }
        const unsigned char b = (unsigned char)bytes[i + k];
        if ((b & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b & 0x3F);
        }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

// Whether the bytes are UTF-8 throughout. A sequence cut off by the end
// is allowed when the bytes are only the start of the file.
bool IsUtf8(std::string_view bytes, bool partial)
{
    for (size_t i = 0; i < bytes.size();)
        {
        char32_t cp;
        bool truncated;
        const size_t len = Utf8SequenceAt(bytes, i, cp, truncated);
        if (len == 0) return partial && truncated;
        i += len;
        }
    return true;
}

void AppendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800)
        {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
        }
    else if (cp < 0x10000)
        {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
        }
    else
        {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
        }
}

// Builds the document from a stream of code points: line breaks of every
// kind become '\n', invisible control characters become U+FFFD.
struct Builder
{
    Document& doc;
    bool      sawLF = false, sawCRLF = false, sawCR = false;
    bool      pendingCR = false;
    size_t    lineLength = 0;

    explicit Builder(Document& d) : doc(d) { doc.lineStarts.push_back(0); }

    void NewLine()
    {
        doc.longestLine = std::max(doc.longestLine, lineLength);
        lineLength = 0;
        doc.text += '\n';
        doc.lineStarts.push_back(doc.text.size());
    }

    void Put(char32_t cp)
    {
        if (pendingCR)
            {
            pendingCR = false;
            if (cp == '\n')
                {
                sawCRLF = true;
                NewLine();
                return;
                }
            sawCR = true;
            NewLine();
            }
        if (cp == '\r')
            {
            pendingCR = true;
            return;
            }
        if (cp == '\n')
            {
            sawLF = true;
            NewLine();
            return;
            }
        if ((cp < 0x20 && cp != '\t') || cp == 0x7F) cp = kReplacement;
        AppendUtf8(doc.text, cp);
        lineLength = cp == '\t' ? (lineLength / kTabWidth + 1) * kTabWidth : lineLength + 1;
    }

    void Finish()
    {
        if (pendingCR)
            {
            sawCR = true;
            NewLine();
            }
        doc.longestLine = std::max(doc.longestLine, lineLength);
        // A final line break ends the last line rather than starting another.
        if (doc.lineStarts.size() > 1 && doc.lineStarts.back() == doc.text.size())
            {
            doc.lineStarts.pop_back();
            doc.text.pop_back();
            }
        const int kinds = (int)sawLF + (int)sawCRLF + (int)sawCR;
        doc.lineEnding = kinds > 1 ? LineEnding::Mixed
                       : sawCRLF   ? LineEnding::CRLF
                       : sawCR     ? LineEnding::CR
                       : sawLF     ? LineEnding::LF
                                   : LineEnding::None;
    }
};

void DecodeUtf16(std::string_view bytes, bool littleEndian, Builder& b)
{
    auto unit = [&](size_t i) {
        const unsigned lo = (unsigned char)bytes[i + (littleEndian ? 0 : 1)];
        const unsigned hi = (unsigned char)bytes[i + (littleEndian ? 1 : 0)];
        return (char32_t)(lo | (hi << 8));
    };
    for (size_t i = 0; i + 1 < bytes.size(); i += 2)
        {
        char32_t cp = unit(i);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < bytes.size())
            {
            const char32_t low = unit(i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF)
                {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
                }
            else cp = kReplacement;
            }
        else if (cp >= 0xD800 && cp <= 0xDFFF) cp = kReplacement;
        b.Put(cp);
        }
}

} // namespace

const char* EncodingName(Encoding encoding)
{
    switch (encoding)
        {
        case Encoding::Utf8: return "UTF-8";
        case Encoding::Utf8Bom: return "UTF-8 with BOM";
        case Encoding::Utf16LE: return "UTF-16 LE";
        case Encoding::Utf16BE: return "UTF-16 BE";
        case Encoding::Latin1: return "Latin-1";
        }
    return "";
}

const char* LineEndingName(LineEnding ending)
{
    switch (ending)
        {
        case LineEnding::None: return "";
        case LineEnding::LF: return "LF";
        case LineEnding::CRLF: return "CRLF";
        case LineEnding::CR: return "CR";
        case LineEnding::Mixed: return "Mixed line endings";
        }
    return "";
}

bool LooksLikeText(std::string_view head)
{
    if (head.size() >= 2 && ((unsigned char)head[0] == 0xFF && (unsigned char)head[1] == 0xFE)) return true;
    if (head.size() >= 2 && ((unsigned char)head[0] == 0xFE && (unsigned char)head[1] == 0xFF)) return true;
    size_t control = 0, high = 0;
    for (char ch : head)
        {
        const unsigned char c = (unsigned char)ch;
        if (c == 0) return false;
        // Tab, line breaks, form feed and escape (colour codes in logs) are
        // at home in text; the rest of C0 is not.
        if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != '\v' && c != 0x1B) || c == 0x7F) ++control;
        if (c >= 0x80) ++high;
        }
    if (control * 100 > head.size()) return false;
    // Not UTF-8 may still be Latin-1 text, which is mostly ASCII.
    return IsUtf8(head, true) || high * 10 < head.size() * 3;
}

bool FileLooksLikeText(const std::string& path)
{
    std::error_code ec;
    // Never read a pipe or a device: it might never answer.
    if (!std::filesystem::is_regular_file(U8(path), ec)) return false;
    std::ifstream stream(U8(path), std::ios::binary);
    if (!stream) return false;
    std::string head(kSniffBytes, '\0');
    stream.read(head.data(), (std::streamsize)head.size());
    head.resize((size_t)stream.gcount());
    return LooksLikeText(head);
}

size_t Document::LineOf(size_t offset) const
{
    const auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
    return it == lineStarts.begin() ? 0 : (size_t)(it - lineStarts.begin()) - 1;
}

Document Decode(std::string_view bytes)
{
    Document doc;
    doc.text.reserve(bytes.size());
    Builder b(doc);
    const auto starts = [&](std::initializer_list<unsigned char> bom) {
        if (bytes.size() < bom.size()) return false;
        size_t i = 0;
        for (unsigned char c : bom)
            if ((unsigned char)bytes[i++] != c) return false;
        return true;
    };
    if (starts({ 0xFF, 0xFE }) || starts({ 0xFE, 0xFF }))
        {
        const bool le = (unsigned char)bytes[0] == 0xFF;
        doc.encoding = le ? Encoding::Utf16LE : Encoding::Utf16BE;
        DecodeUtf16(bytes.substr(2), le, b);
        }
    else
        {
        if (starts({ 0xEF, 0xBB, 0xBF }))
            {
            doc.encoding = Encoding::Utf8Bom;
            bytes.remove_prefix(3);
            }
        if (IsUtf8(bytes, false))
            {
            for (size_t i = 0; i < bytes.size();)
                {
                char32_t cp;
                bool truncated;
                const size_t len = Utf8SequenceAt(bytes, i, cp, truncated);
                if (len == 0) break;   // never: the bytes were checked
                b.Put(cp);
                i += len;
                }
            }
        else
            {
            doc.encoding = Encoding::Latin1;
            for (char ch : bytes) b.Put((char32_t)(unsigned char)ch);
            }
        }
    b.Finish();
    return doc;
}

void TrimToCharacter(std::string& bytes)
{
    size_t keep = bytes.size();
    // Back over continuation bytes to the lead byte of the last character.
    size_t back = 0;
    while (back < 3 && keep > back && ((unsigned char)bytes[keep - 1 - back] & 0xC0) == 0x80) ++back;
    if (keep > back)
        {
        const unsigned char lead = (unsigned char)bytes[keep - 1 - back];
        const size_t need = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (need > back + 1) keep -= back + 1;   // the last character is incomplete
        }
    bytes.resize(keep);
}

std::vector<size_t> FindAll(const std::string& text, std::string_view needle, size_t limit)
{
    std::vector<size_t> hits;
    if (needle.empty()) return hits;
    const auto lower = [](char c) { return (char)std::tolower((unsigned char)c); };
    const auto hash = [&](char c) { return std::hash<char>{}(lower(c)); };
    const auto equal = [&](char a, char b) { return lower(a) == lower(b); };
    const std::boyer_moore_horspool_searcher searcher(needle.begin(), needle.end(), hash, equal);
    auto at = text.begin();
    while (hits.size() < limit)
        {
        const auto [first, last] = searcher(at, text.end());
        if (first == text.end()) break;
        hits.push_back((size_t)(first - text.begin()));
        at = last;
        }
    return hits;
}

} // namespace app::text
