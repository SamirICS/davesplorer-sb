// Plain text files for the built-in viewer: which files are text, and the
// file's text decoded to UTF-8 and split into lines. Source code, scripts,
// JSON, logs, configuration -- anything a person reads as characters.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace app::text
{

// Extensions that are text by convention: source files, scripts, data and
// configuration formats, logs. Lower-case without the dot, as a FileEntry
// carries it. Markdown is not among them; it has its own viewer.
bool IsTextExtension(std::string_view extension);

// Whether the start of a file reads as text: UTF-16 with a byte-order mark,
// or bytes with no NUL and almost no control characters. `head` is what was
// read of the file, so a UTF-8 sequence cut off at its end does not count
// against it. Empty is text.
bool LooksLikeText(std::string_view head);
// The same, reading the first 8 KB of the file. False when it can't be read.
bool FileLooksLikeText(const std::string& path);

// Columns between tab stops, in the view and in Document::longestLine.
constexpr size_t kTabWidth = 4;

enum class Encoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Latin1 };
enum class LineEnding { None, LF, CRLF, CR, Mixed };

const char* EncodingName(Encoding encoding);
const char* LineEndingName(LineEnding ending);

// A file's text as lines. `text` holds them joined by '\n' whatever the
// file used; line i is text[lineStarts[i] .. LineEnd(i)). A final line
// break does not make an empty last line. Control characters other than
// tab show as U+FFFD, so nothing invisible hides in the view.
struct Document
{
    std::string           text;
    std::vector<size_t>   lineStarts;   // at least one entry, for the first line
    Encoding              encoding = Encoding::Utf8;
    LineEnding            lineEnding = LineEnding::None;
    size_t                longestLine = 0;   // in columns: a code point is one, a tab runs to the next stop

    size_t LineCount() const { return lineStarts.size(); }
    size_t LineEnd(size_t line) const
    {
        return line + 1 < lineStarts.size() ? lineStarts[line + 1] - 1 : text.size();
    }
    std::string_view Line(size_t line) const
    {
        return std::string_view(text).substr(lineStarts[line], LineEnd(line) - lineStarts[line]);
    }
    // The line holding a byte offset into `text`.
    size_t LineOf(size_t offset) const;
};

// Decodes the bytes of a file: a BOM says UTF-8 or UTF-16; otherwise UTF-8
// when every sequence is valid, Latin-1 when not. The bytes are the whole
// file, or cut at a character boundary (see TrimToCharacter).
Document Decode(std::string_view bytes);
// Cuts bytes read from the start of a longer file back to the last whole
// UTF-8 character, so the cut does not turn the file into Latin-1.
void TrimToCharacter(std::string& bytes);

// Every match of `needle` in the text, ASCII case ignored, as byte offsets.
// Matches do not overlap; at most `limit` are returned.
std::vector<size_t> FindAll(const std::string& text, std::string_view needle, size_t limit);

} // namespace app::text
