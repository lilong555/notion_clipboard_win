#include "converter.h"

#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace ncw
{
using LinkReferenceMap = std::map<std::string, std::string>;
const std::string kHtmlMarkRichTextMarker = "\x1fmark\x1f";
const std::string kHtmlColorRichTextMarkerPrefix = "\x1f" "color:";
const std::string kHtmlColorRichTextMarkerSuffix = "\x1f";

struct MarkdownAlertStyle
{
    std::string emoji;
    std::string color;
};

struct MarkdownAdmonitionStart
{
    MarkdownAlertStyle style;
    std::string title;
    std::size_t fence_len = 3;
};

std::string NormalizeLineEndings(std::string text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\r')
        {
            if (i + 1 < text.size() && text[i + 1] == '\n')
            {
                ++i;
            }
            output.push_back('\n');
        }
        else
        {
            output.push_back(text[i]);
        }
    }
    return output;
}

std::size_t Utf8CharLength(unsigned char first)
{
    if ((first & 0x80) == 0)
    {
        return 1;
    }
    if ((first & 0xE0) == 0xC0)
    {
        return 2;
    }
    if ((first & 0xF0) == 0xE0)
    {
        return 3;
    }
    if ((first & 0xF8) == 0xF0)
    {
        return 4;
    }
    return 1;
}

std::vector<std::string> SplitUtf8ByCharLimit(const std::string &text, std::size_t max_chars)
{
    std::vector<std::string> chunks;
    std::size_t begin = 0;
    std::size_t pos = 0;
    std::size_t chars = 0;
    while (pos < text.size())
    {
        if (chars >= max_chars)
        {
            chunks.push_back(text.substr(begin, pos - begin));
            begin = pos;
            chars = 0;
        }
        const std::size_t len = std::min(Utf8CharLength(static_cast<unsigned char>(text[pos])), text.size() - pos);
        pos += len;
        ++chars;
    }
    if (begin < text.size())
    {
        chunks.push_back(text.substr(begin));
    }
    return chunks;
}

std::string TruncateUtf8(const std::string &text, std::size_t max_chars)
{
    std::size_t pos = 0;
    std::size_t chars = 0;
    while (pos < text.size() && chars < max_chars)
    {
        const std::size_t len = std::min(Utf8CharLength(static_cast<unsigned char>(text[pos])), text.size() - pos);
        pos += len;
        ++chars;
    }
    if (pos >= text.size())
    {
        return text;
    }
    return text.substr(0, pos) + "...";
}

std::string CollapseWhitespace(const std::string &text)
{
    std::string output;
    bool in_space = false;
    for (unsigned char ch : text)
    {
        if (std::isspace(ch))
        {
            if (!in_space)
            {
                output.push_back(' ');
                in_space = true;
            }
            continue;
        }
        output.push_back(static_cast<char>(ch));
        in_space = false;
    }
    return Trim(output);
}

void ReplaceAllInPlace(std::string *text, const std::string &from, const std::string &to);
std::size_t CountRepeatedChar(const std::string &text, std::size_t pos, char ch);
std::size_t FindRepeatedCharRun(const std::string &text, std::size_t pos, char ch, std::size_t run_len);
bool IsEscaped(const std::string &text, std::size_t pos);
std::string UnescapeMarkdownText(const std::string &text, bool preserve_inline_math_parentheses = false);
std::string StripNonMathDollarMarkersForPlainText(const std::string &text);
bool LooksLikeAsciiFunctionFormula(const std::string &expression);
bool ExtractMarkdownImage(const std::string &text, std::size_t pos, std::string *alt_text, std::string *url,
                          std::size_t *end_pos);
bool ExtractMarkdownFootnoteReference(const std::string &text, std::size_t pos, std::string *label,
                                      std::size_t *end_pos);
bool IsMarkdownReferenceDefinitionLine(const std::string &line, std::string *normalized_label, std::string *url);
bool IsMarkdownHighlightDelimiterAt(const std::string &text, std::size_t pos, bool closing);
std::size_t FindMarkdownHighlightClose(const std::string &text, std::size_t begin);
std::string BuildSupSubFallback(const std::string &text, bool superscript);

std::optional<MarkdownAlertStyle> MarkdownAlertStyleForType(const std::string &type)
{
    const std::string normalized = ToLowerAscii(Trim(type));
    if (normalized == "note" || normalized == "info" || normalized == "abstract" || normalized == "summary" ||
        normalized == "tldr")
    {
        return MarkdownAlertStyle{"ℹ️", "blue_background"};
    }
    if (normalized == "tip" || normalized == "success" || normalized == "check" || normalized == "done" ||
        normalized == "example")
    {
        return MarkdownAlertStyle{"💡", "green_background"};
    }
    if (normalized == "important" || normalized == "question" || normalized == "help" || normalized == "faq")
    {
        return MarkdownAlertStyle{"❗", "purple_background"};
    }
    if (normalized == "warning" || normalized == "attention" || normalized == "failure" || normalized == "fail" ||
        normalized == "missing")
    {
        return MarkdownAlertStyle{"⚠️", "yellow_background"};
    }
    if (normalized == "caution" || normalized == "danger" || normalized == "error" || normalized == "bug")
    {
        return MarkdownAlertStyle{"⛔", "red_background"};
    }
    return std::nullopt;
}

std::optional<MarkdownAlertStyle> ParseMarkdownAlertMarkerLine(const std::string &line, std::size_t *marker_end = nullptr)
{
    const std::string trimmed = Trim(line);
    if (trimmed.size() < 4 || trimmed[0] != '[' || trimmed[1] != '!')
    {
        return std::nullopt;
    }
    const std::size_t end = trimmed.find(']', 2);
    if (end == std::string::npos)
    {
        return std::nullopt;
    }
    const std::optional<MarkdownAlertStyle> style = MarkdownAlertStyleForType(trimmed.substr(2, end - 2));
    if (!style.has_value())
    {
        return std::nullopt;
    }
    if (marker_end != nullptr)
    {
        *marker_end = end + 1;
    }
    return style;
}

std::optional<MarkdownAdmonitionStart> ParseMarkdownColonAdmonitionStart(const std::string &line)
{
    const std::string trimmed = Trim(line);
    std::size_t fence_len = 0;
    while (fence_len < trimmed.size() && trimmed[fence_len] == ':')
    {
        ++fence_len;
    }
    if (fence_len < 3)
    {
        return std::nullopt;
    }

    const std::string rest = Trim(trimmed.substr(fence_len));
    if (rest.empty())
    {
        return std::nullopt;
    }

    std::size_t type_end = 0;
    while (type_end < rest.size() && !std::isspace(static_cast<unsigned char>(rest[type_end])))
    {
        ++type_end;
    }
    const std::optional<MarkdownAlertStyle> style = MarkdownAlertStyleForType(rest.substr(0, type_end));
    if (!style.has_value())
    {
        return std::nullopt;
    }

    std::string title = Trim(rest.substr(type_end));
    if (title.size() >= 2 && ((title.front() == '"' && title.back() == '"') ||
                              (title.front() == '\'' && title.back() == '\'')))
    {
        title = title.substr(1, title.size() - 2);
    }
    return MarkdownAdmonitionStart{*style, title, fence_len};
}

bool IsMarkdownColonAdmonitionEnd(const std::string &line, std::size_t fence_len)
{
    const std::string trimmed = Trim(line);
    return trimmed.size() >= fence_len && std::all_of(trimmed.begin(), trimmed.end(), [](char ch)
                                                      { return ch == ':'; });
}

std::optional<MarkdownAdmonitionStart> ParseMarkdownBangAdmonitionStart(const std::string &line)
{
    const std::string trimmed = Trim(line);
    if (trimmed.size() < 4 ||
        !(trimmed.rfind("!!!", 0) == 0 || trimmed.rfind("???", 0) == 0))
    {
        return std::nullopt;
    }

    std::size_t marker_len = 3;
    if (marker_len < trimmed.size() && (trimmed[marker_len] == '+' || trimmed[marker_len] == '-'))
    {
        ++marker_len;
    }
    if (marker_len >= trimmed.size() || !std::isspace(static_cast<unsigned char>(trimmed[marker_len])))
    {
        return std::nullopt;
    }

    const std::string rest = Trim(trimmed.substr(marker_len));
    if (rest.empty())
    {
        return std::nullopt;
    }
    std::size_t type_end = 0;
    while (type_end < rest.size() && !std::isspace(static_cast<unsigned char>(rest[type_end])))
    {
        ++type_end;
    }
    const std::optional<MarkdownAlertStyle> style = MarkdownAlertStyleForType(rest.substr(0, type_end));
    if (!style.has_value())
    {
        return std::nullopt;
    }

    std::string title = Trim(rest.substr(type_end));
    if (title.size() >= 2 && ((title.front() == '"' && title.back() == '"') ||
                              (title.front() == '\'' && title.back() == '\'')))
    {
        title = title.substr(1, title.size() - 2);
    }
    return MarkdownAdmonitionStart{*style, title, marker_len};
}

std::string StripInlineCodeDelimitersForTitle(const std::string &line)
{
    std::string output;
    output.reserve(line.size());
    for (std::size_t i = 0; i < line.size();)
    {
        if (line[i] != '`')
        {
            output.push_back(line[i++]);
            continue;
        }

        const std::size_t run_len = CountRepeatedChar(line, i, '`');
        const std::size_t close = FindRepeatedCharRun(line, i + run_len, '`', run_len);
        if (close == std::string::npos)
        {
            i += run_len;
            continue;
        }
        output += line.substr(i + run_len, close - i - run_len);
        i = close + run_len;
    }
    return output;
}

bool LooksLikeMarkdownHeadingAttributeList(const std::string &text)
{
    const std::string trimmed = Trim(text);
    if (trimmed.empty())
    {
        return false;
    }

    bool saw_attribute = false;
    std::size_t pos = 0;
    while (pos < trimmed.size())
    {
        while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos])))
        {
            ++pos;
        }
        if (pos >= trimmed.size())
        {
            break;
        }

        if (trimmed[pos] == '#' || trimmed[pos] == '.')
        {
            ++pos;
            while (pos < trimmed.size() && !std::isspace(static_cast<unsigned char>(trimmed[pos])))
            {
                ++pos;
            }
            saw_attribute = true;
            continue;
        }

        const std::size_t equals = trimmed.find('=', pos);
        const std::size_t next_space = trimmed.find_first_of(" \t\r\n", pos);
        if (equals == std::string::npos || (next_space != std::string::npos && next_space < equals))
        {
            return false;
        }

        pos = equals + 1;
        if (pos < trimmed.size() && (trimmed[pos] == '"' || trimmed[pos] == '\''))
        {
            const char quote = trimmed[pos++];
            bool closed = false;
            while (pos < trimmed.size())
            {
                if (trimmed[pos] == '\\' && pos + 1 < trimmed.size())
                {
                    pos += 2;
                    continue;
                }
                if (trimmed[pos] == quote)
                {
                    ++pos;
                    closed = true;
                    break;
                }
                ++pos;
            }
            if (!closed)
            {
                return false;
            }
        }
        else
        {
            while (pos < trimmed.size() && !std::isspace(static_cast<unsigned char>(trimmed[pos])))
            {
                ++pos;
            }
        }
        saw_attribute = true;
    }
    return saw_attribute;
}

std::string StripMarkdownHeadingAttributeList(std::string text)
{
    text = Trim(std::move(text));
    if (text.size() < 4 || text.back() != '}')
    {
        return text;
    }

    const std::size_t open = text.rfind('{');
    if (open == std::string::npos || open == 0 || !std::isspace(static_cast<unsigned char>(text[open - 1])))
    {
        return text;
    }
    if (!LooksLikeMarkdownHeadingAttributeList(text.substr(open + 1, text.size() - open - 2)))
    {
        return text;
    }
    return Trim(text.substr(0, open));
}

std::string StripTitleMarkdownMarkers(std::string line)
{
    line = StripMarkdownHeadingAttributeList(std::move(line));
    ReplaceAllInPlace(&line, "**", "");
    ReplaceAllInPlace(&line, "__", "");
    ReplaceAllInPlace(&line, kHtmlMarkRichTextMarker, "");
    std::size_t color_marker = 0;
    while ((color_marker = line.find(kHtmlColorRichTextMarkerPrefix, color_marker)) != std::string::npos)
    {
        const std::size_t marker_end = line.find(kHtmlColorRichTextMarkerSuffix,
                                                 color_marker + kHtmlColorRichTextMarkerPrefix.size());
        if (marker_end == std::string::npos)
        {
            break;
        }
        line.erase(color_marker, marker_end + kHtmlColorRichTextMarkerSuffix.size() - color_marker);
    }
    std::string stripped;
    stripped.reserve(line.size());
    bool italic = false;
    bool underline = false;
    bool strikethrough = false;
    bool highlight = false;
    for (std::size_t i = 0; i < line.size();)
    {
        std::string footnote_label;
        std::size_t footnote_end = std::string::npos;
        if (ExtractMarkdownFootnoteReference(line, i, &footnote_label, &footnote_end))
        {
            stripped += BuildSupSubFallback(footnote_label, true);
            i = footnote_end;
            continue;
        }
        if (line.compare(i, 2, "==") == 0 && !IsEscaped(line, i))
        {
            if (highlight && IsMarkdownHighlightDelimiterAt(line, i, true))
            {
                highlight = false;
                i += 2;
                continue;
            }
            if (!highlight && IsMarkdownHighlightDelimiterAt(line, i, false) &&
                FindMarkdownHighlightClose(line, i + 2) != std::string::npos)
            {
                highlight = true;
                i += 2;
                continue;
            }
        }
        if (line.compare(i, 2, "++") == 0 && !IsEscaped(line, i) &&
            (underline || FindRepeatedCharRun(line, i + 2, '+', 2) != std::string::npos))
        {
            underline = !underline;
            i += 2;
            continue;
        }
        if (line.compare(i, 2, "~~") == 0 && !IsEscaped(line, i) &&
            (strikethrough || FindRepeatedCharRun(line, i + 2, '~', 2) != std::string::npos))
        {
            strikethrough = !strikethrough;
            i += 2;
            continue;
        }
        if (line[i] == '*' && !IsEscaped(line, i) && (italic || line.find('*', i + 1) != std::string::npos))
        {
            italic = !italic;
            ++i;
            continue;
        }
        stripped.push_back(line[i++]);
    }
    line = std::move(stripped);
    return UnescapeMarkdownText(StripNonMathDollarMarkersForPlainText(StripInlineCodeDelimitersForTitle(line)), true);
}

std::string StripTitleMarkdownPrefix(std::string line)
{
    line = Trim(line);
    if (line.empty())
    {
        return line;
    }

    std::string image_alt;
    std::string image_url;
    std::size_t image_end = std::string::npos;
    if (ExtractMarkdownImage(line, 0, &image_alt, &image_url, &image_end) && image_end == line.size())
    {
        return StripTitleMarkdownMarkers(image_alt);
    }

    std::size_t hashes = 0;
    while (hashes < line.size() && line[hashes] == '#')
    {
        ++hashes;
    }
    if (hashes > 0 && hashes < line.size() && std::isspace(static_cast<unsigned char>(line[hashes])))
    {
        line = Trim(line.substr(hashes));
        return StripTitleMarkdownMarkers(line);
    }

    if (const std::optional<MarkdownAdmonitionStart> admonition = ParseMarkdownColonAdmonitionStart(line))
    {
        if (Trim(admonition->title).empty())
        {
            return "";
        }
        return StripTitleMarkdownMarkers(admonition->title);
    }
    if (const std::optional<MarkdownAdmonitionStart> admonition = ParseMarkdownBangAdmonitionStart(line))
    {
        if (Trim(admonition->title).empty())
        {
            return "";
        }
        return StripTitleMarkdownMarkers(admonition->title);
    }

    if (line.size() >= 6 && (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
        std::isspace(static_cast<unsigned char>(line[1])) && line[2] == '[' && line[4] == ']' &&
        std::isspace(static_cast<unsigned char>(line[5])))
    {
        line = Trim(line.substr(6));
        return StripTitleMarkdownMarkers(line);
    }

    if (line.size() >= 2 && (line[0] == '-' || line[0] == '*' || line[0] == '+') &&
        std::isspace(static_cast<unsigned char>(line[1])))
    {
        line = Trim(line.substr(2));
        return StripTitleMarkdownMarkers(line);
    }

    if (line.size() >= 2 && line[0] == '>' && std::isspace(static_cast<unsigned char>(line[1])))
    {
        line = Trim(line.substr(2));
        std::size_t marker_end = 0;
        if (ParseMarkdownAlertMarkerLine(line, &marker_end).has_value())
        {
            line = Trim(line.substr(marker_end));
            if (line.empty())
            {
                return "";
            }
        }
        return StripTitleMarkdownMarkers(line);
    }

    std::size_t digits = 0;
    while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
    {
        ++digits;
    }
    if (digits > 0 && digits + 1 < line.size() && (line[digits] == '.' || line[digits] == ')') &&
        std::isspace(static_cast<unsigned char>(line[digits + 1])))
    {
        line = Trim(line.substr(digits + 2));
        return StripTitleMarkdownMarkers(line);
    }

    return StripTitleMarkdownMarkers(line);
}

std::string BuildTitleFromContent(const std::string &content)
{
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line))
    {
        const std::string trimmed_line = Trim(line);
        if (trimmed_line.rfind("```", 0) == 0 || trimmed_line.rfind("~~~", 0) == 0)
        {
            continue;
        }
        line = CollapseWhitespace(StripTitleMarkdownPrefix(line));
        if (!line.empty())
        {
            return TruncateUtf8(line, 80);
        }
    }
    return "Clipboard " + LocalTimestamp();
}

std::string TrimLeft(const std::string &input)
{
    std::size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])))
    {
        ++begin;
    }
    return input.substr(begin);
}

std::string StripUtf8Bom(std::string text)
{
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
    {
        return text.substr(3);
    }
    return text;
}

void ReplaceAllInPlace(std::string *text, const std::string &from, const std::string &to)
{
    if (from.empty())
    {
        return;
    }
    std::size_t pos = 0;
    while ((pos = text->find(from, pos)) != std::string::npos)
    {
        text->replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool IsEscaped(const std::string &text, std::size_t pos)
{
    if (pos == 0)
    {
        return false;
    }
    std::size_t slash_count = 0;
    std::size_t cursor = pos;
    while (cursor > 0 && text[cursor - 1] == '\\')
    {
        ++slash_count;
        --cursor;
    }
    return (slash_count % 2) == 1;
}

bool IsAsciiPunctuation(char ch)
{
    const unsigned char uch = static_cast<unsigned char>(ch);
    return uch < 128 && std::ispunct(uch) != 0;
}

std::string UnescapeMarkdownText(const std::string &text, bool preserve_inline_math_parentheses)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\\' && i + 1 < text.size() && IsAsciiPunctuation(text[i + 1]))
        {
            if (preserve_inline_math_parentheses && (text[i + 1] == '(' || text[i + 1] == ')'))
            {
                output.push_back(text[i]);
                continue;
            }
            output.push_back(text[++i]);
            continue;
        }
        output.push_back(text[i]);
    }
    return output;
}

std::vector<std::string> SplitLinesPreserveEmpty(const std::string &text)
{
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const std::size_t pos = text.find('\n', start);
        if (pos == std::string::npos)
        {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, pos - start));
        start = pos + 1;
        if (start == text.size())
        {
            lines.emplace_back();
            break;
        }
    }
    return lines;
}

bool StartsWithFence(const std::string &line, char *fence_char, std::size_t *fence_len)
{
    const std::string trimmed_left = TrimLeft(line);
    if (trimmed_left.empty() || (trimmed_left[0] != '`' && trimmed_left[0] != '~'))
    {
        return false;
    }
    const char ch = trimmed_left[0];
    std::size_t count = 0;
    while (count < trimmed_left.size() && trimmed_left[count] == ch)
    {
        ++count;
    }
    if (count < 3)
    {
        return false;
    }
    if (fence_char != nullptr)
    {
        *fence_char = ch;
    }
    if (fence_len != nullptr)
    {
        *fence_len = count;
    }
    return true;
}

bool IsClosingFenceLine(const std::string &line, char fence_char, std::size_t fence_len)
{
    const std::string trimmed_left = TrimLeft(line);
    if (trimmed_left.empty() || trimmed_left[0] != fence_char)
    {
        return false;
    }
    std::size_t close_len = 0;
    while (close_len < trimmed_left.size() && trimmed_left[close_len] == fence_char)
    {
        ++close_len;
    }
    return close_len >= fence_len && Trim(trimmed_left.substr(close_len)).empty();
}

bool IsAsciiAlpha(char ch)
{
    return std::isalpha(static_cast<unsigned char>(ch)) != 0;
}

bool IsAsciiAlnum(char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) != 0;
}

bool IsAsciiSpace(char ch)
{
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

bool ShouldInsertLatexBackslash(const std::string &token)
{
    static const std::vector<std::string> known_commands = {
        "approx", "cdot", "times", "div", "leq", "geq", "neq", "infty", "partial", "nabla", "sum", "prod",
        "int", "sqrt", "frac", "dfrac", "tfrac", "log", "ln", "sin", "cos", "tan", "cot", "exp", "lim",
        "min", "max", "forall", "exists", "in", "notin", "subset", "subseteq", "supset", "supseteq", "cup",
        "cap", "land", "lor", "to", "rightarrow", "Rightarrow", "leftarrow", "Leftarrow", "cdots", "ldots",
        "dots", "theta", "Theta", "lambda", "mu", "phi", "Phi", "pi", "alpha", "beta", "gamma", "delta",
        "Delta", "epsilon", "eta", "rho", "sigma", "Sigma", "omega", "Omega",
    };
    return std::find(known_commands.begin(), known_commands.end(), token) != known_commands.end();
}

std::size_t CountRepeatedChar(const std::string &text, std::size_t pos, char ch)
{
    std::size_t count = 0;
    while (pos + count < text.size() && text[pos + count] == ch)
    {
        ++count;
    }
    return count;
}

std::size_t FindRepeatedCharRun(const std::string &text, std::size_t pos, char ch, std::size_t run_len)
{
    while (pos < text.size())
    {
        const std::size_t found = text.find(ch, pos);
        if (found == std::string::npos)
        {
            return std::string::npos;
        }
        if (CountRepeatedChar(text, found, ch) == run_len)
        {
            return found;
        }
        pos = found + 1;
    }
    return std::string::npos;
}

std::string InsertMissingLatexBackslashes(const std::string &expression)
{
    std::string repaired;
    repaired.reserve(expression.size() + 16);
    for (std::size_t i = 0; i < expression.size();)
    {
        if (expression.compare(i, 6, "\\text{") == 0)
        {
            std::size_t cursor = i + 6;
            int depth = 1;
            while (cursor < expression.size() && depth > 0)
            {
                if (!IsEscaped(expression, cursor) && expression[cursor] == '{')
                {
                    ++depth;
                }
                else if (!IsEscaped(expression, cursor) && expression[cursor] == '}')
                {
                    --depth;
                }
                ++cursor;
            }
            repaired += expression.substr(i, cursor - i);
            i = cursor;
            continue;
        }

        if (!IsAsciiAlpha(expression[i]))
        {
            repaired.push_back(expression[i]);
            ++i;
            continue;
        }

        const std::size_t begin = i;
        while (i < expression.size() && IsAsciiAlpha(expression[i]))
        {
            ++i;
        }
        const std::string token = expression.substr(begin, i - begin);
        const char prev = (begin == 0) ? '\0' : expression[begin - 1];
        const char next = (i < expression.size()) ? expression[i] : '\0';
        const bool already_escaped = prev == '\\';
        const bool starts_new_token = begin == 0 || !IsAsciiAlnum(prev);
        const bool ends_token = next == '\0' || !IsAsciiAlpha(next);
        if (!already_escaped && starts_new_token && ends_token && ShouldInsertLatexBackslash(token))
        {
            repaired.push_back('\\');
        }
        repaired += token;
    }
    return repaired;
}

std::size_t CountUnescapedToken(const std::string &text, const std::string &token)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while (!token.empty() && (pos = text.find(token, pos)) != std::string::npos)
    {
        if (!IsEscaped(text, pos))
        {
            ++count;
        }
        pos += token.size();
    }
    return count;
}

std::string StripLatexOuterDelimiters(std::string expression)
{
    expression = Trim(expression);
    bool changed = true;
    while (changed)
    {
        changed = false;
        if (expression.size() >= 4 && expression.rfind("$$", 0) == 0 && expression.substr(expression.size() - 2) == "$$")
        {
            expression = Trim(expression.substr(2, expression.size() - 4));
            changed = true;
        }
        else if (expression.size() >= 2 && expression.front() == '$' && expression.back() == '$')
        {
            expression = Trim(expression.substr(1, expression.size() - 2));
            changed = true;
        }
        else if (expression.size() >= 4 && expression.rfind("\\(", 0) == 0 &&
                 expression.substr(expression.size() - 2) == "\\)")
        {
            expression = Trim(expression.substr(2, expression.size() - 4));
            changed = true;
        }
        else if (expression.size() >= 4 && expression.rfind("\\[", 0) == 0 &&
                 expression.substr(expression.size() - 2) == "\\]")
        {
            expression = Trim(expression.substr(2, expression.size() - 4));
            changed = true;
        }
    }
    return expression;
}

bool StripLatexEnvironment(std::string *expression, const std::string &env, bool wrap_aligned)
{
    const std::string begin = "\\begin{" + env + "}";
    const std::string end = "\\end{" + env + "}";
    std::string trimmed = Trim(*expression);
    if (trimmed.rfind(begin, 0) != 0 || trimmed.size() < begin.size() + end.size() ||
        trimmed.substr(trimmed.size() - end.size()) != end)
    {
        return false;
    }
    trimmed = Trim(trimmed.substr(begin.size(), trimmed.size() - begin.size() - end.size()));
    if (wrap_aligned)
    {
        *expression = "\\begin{aligned}\n" + trimmed + "\n\\end{aligned}";
    }
    else
    {
        *expression = trimmed;
    }
    return true;
}

bool IsRepeatedCharLine(const std::string &line, char ch, std::size_t min_count)
{
    const std::string trimmed = Trim(line);
    return trimmed.size() >= min_count &&
           std::all_of(trimmed.begin(), trimmed.end(), [&](char current)
                       { return current == ch; });
}

std::string RemoveLatexUnderlineArtifactLines(const std::string &text)
{
    const std::vector<std::string> lines = SplitLinesPreserveEmpty(NormalizeLineEndings(text));
    std::vector<std::string> kept;
    kept.reserve(lines.size());
    for (const std::string &line : lines)
    {
        if (IsRepeatedCharLine(line, '=', 3))
        {
            continue;
        }
        std::string cleaned_line = line;
        const std::string trimmed_left = TrimLeft(cleaned_line);
        if (trimmed_left.size() > 2 && trimmed_left[0] == '#' &&
            std::isspace(static_cast<unsigned char>(trimmed_left[1])))
        {
            cleaned_line = TrimLeft(trimmed_left.substr(2));
        }
        kept.push_back(cleaned_line);
    }

    std::ostringstream oss;
    for (std::size_t i = 0; i < kept.size(); ++i)
    {
        if (i != 0)
        {
            oss << "\n";
        }
        oss << kept[i];
    }
    return oss.str();
}

std::string RepairLatexExpression(std::string expression)
{
    expression = StripUtf8Bom(std::move(expression));
    expression = NormalizeLineEndings(std::move(expression));
    expression = RemoveLatexUnderlineArtifactLines(expression);
    ReplaceAllInPlace(&expression, "\t", " ");
    ReplaceAllInPlace(&expression, "\xc2\xa0", " ");
    ReplaceAllInPlace(&expression, "\xe3\x80\x80", " ");
    expression = StripLatexOuterDelimiters(std::move(expression));

    StripLatexEnvironment(&expression, "equation", false);
    StripLatexEnvironment(&expression, "equation*", false);
    StripLatexEnvironment(&expression, "align", true);
    StripLatexEnvironment(&expression, "align*", true);
    StripLatexEnvironment(&expression, "gather", true);
    StripLatexEnvironment(&expression, "gather*", true);

    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"−", "-"}, {"–", "-"}, {"—", "-"}, {"∗", "*"}, {"×", "\\times "}, {"÷", "\\div "},
        {"·", "\\cdot "}, {"∈", "\\in "}, {"∉", "\\notin "}, {"≤", "\\leq "}, {"≥", "\\geq "},
        {"≠", "\\neq "}, {"≈", "\\approx "}, {"∞", "\\infty "}, {"∂", "\\partial "},
        {"∇", "\\nabla "}, {"∑", "\\sum "}, {"∏", "\\prod "}, {"∫", "\\int "}, {"√", "\\sqrt "},
        {"α", "\\alpha "}, {"β", "\\beta "}, {"γ", "\\gamma "}, {"λ", "\\lambda "}, {"μ", "\\mu "},
        {"π", "\\pi "}, {"φ", "\\phi "}, {"Φ", "\\Phi "}, {"θ", "\\theta "}, {"ω", "\\omega "},
        {"Ω", "\\Omega "}, {"Δ", "\\Delta "},
    };
    for (const auto &replacement : replacements)
    {
        ReplaceAllInPlace(&expression, replacement.first, replacement.second);
    }

    expression = InsertMissingLatexBackslashes(expression);
    ReplaceAllInPlace(&expression, "\\\n", "\\\\\n");
    while (expression.find("  ") != std::string::npos)
    {
        ReplaceAllInPlace(&expression, "  ", " ");
    }
    if (CountUnescapedToken(expression, "\\left") != CountUnescapedToken(expression, "\\right"))
    {
        ReplaceAllInPlace(&expression, "\\left", "");
        ReplaceAllInPlace(&expression, "\\right", "");
    }
    return Trim(expression);
}

struct InlineSegment
{
    enum class Type
    {
        Text,
        Equation,
    };
    Type type = Type::Text;
    std::string content;
    bool bold = false;
    bool code = false;
    bool strikethrough = false;
    bool underline = false;
    std::string link_url;
    bool italic = false;
    std::string color = "default";
};

struct MarkdownBlock
{
    enum class Type
    {
        Paragraph,
        Heading1,
        Heading2,
        Heading3,
        BulletedListItem,
        NumberedListItem,
        Quote,
        Callout,
        ToDo,
        Divider,
        Equation,
        Table,
        Image,
        Code,
    };
    Type type = Type::Paragraph;
    std::vector<InlineSegment> rich_text;
    std::string text;
    std::string language;
    bool checked = false;
};

bool IsDividerLine(const std::string &trimmed)
{
    return trimmed == "---" || trimmed == "***" || trimmed == "___";
}

std::optional<std::string> ExtractLatexBeginEnvironmentName(const std::string &trimmed, bool allow_trailing_content = false)
{
    const std::string begin_prefix = "\\begin{";
    if (trimmed.rfind(begin_prefix, 0) != 0)
    {
        return std::nullopt;
    }
    const std::size_t name_begin = begin_prefix.size();
    const std::size_t name_end = trimmed.find('}', name_begin);
    if (name_end == std::string::npos || name_end == name_begin)
    {
        return std::nullopt;
    }
    const std::string name = trimmed.substr(name_begin, name_end - name_begin);
    const bool valid_name = std::all_of(name.begin(), name.end(), [](unsigned char ch)
                                        { return std::isalpha(ch) != 0 || ch == '*'; });
    if (!valid_name)
    {
        return std::nullopt;
    }
    const std::string rest = Trim(trimmed.substr(name_end + 1));
    if (!allow_trailing_content && !rest.empty() && rest.front() != '[' && rest.front() != '{')
    {
        return std::nullopt;
    }
    return name;
}

bool IsSupportedBlockLatexEnvironment(const std::string &name)
{
    static const std::vector<std::string> environments = {
        "equation", "equation*", "align", "align*", "aligned", "alignedat", "gather", "gather*",
        "gathered", "multline", "multline*", "split", "cases", "dcases", "rcases", "matrix",
        "pmatrix", "bmatrix", "Bmatrix", "vmatrix", "Vmatrix", "smallmatrix", "array", "subarray",
    };
    return std::find(environments.begin(), environments.end(), name) != environments.end();
}

bool IsBlockEquationFenceStart(const std::string &trimmed)
{
    if (trimmed == "$$" || trimmed == "\\[")
    {
        return true;
    }
    const std::optional<std::string> env = ExtractLatexBeginEnvironmentName(trimmed);
    return env.has_value() && IsSupportedBlockLatexEnvironment(*env);
}

bool IsLooseBracketEquationFenceStart(const std::string &trimmed)
{
    if (trimmed == "[")
    {
        return true;
    }

    std::size_t hashes = 0;
    while (hashes < trimmed.size() && trimmed[hashes] == '#')
    {
        ++hashes;
    }
    if (hashes == 0 || hashes > 6)
    {
        return false;
    }
    std::size_t pos = hashes;
    while (pos < trimmed.size() && std::isspace(static_cast<unsigned char>(trimmed[pos])))
    {
        ++pos;
    }
    return pos + 1 == trimmed.size() && trimmed[pos] == '[';
}

bool IsLooseBracketEquationFenceEnd(const std::string &trimmed)
{
    return trimmed == "]";
}

bool IsBlockEquationFenceEnd(const std::string &trimmed, const std::string &opening)
{
    if (opening == "$$")
    {
        return trimmed == "$$";
    }
    if (opening == "\\[")
    {
        return trimmed == "\\]";
    }
    if (const std::optional<std::string> env = ExtractLatexBeginEnvironmentName(opening))
    {
        return trimmed == "\\end{" + *env + "}";
    }
    return false;
}

std::optional<std::string> ExtractSingleLineBlockEquation(const std::string &trimmed)
{
    if (trimmed.size() > 4 && trimmed.rfind("$$", 0) == 0 && trimmed.substr(trimmed.size() - 2) == "$$")
    {
        return Trim(trimmed.substr(2, trimmed.size() - 4));
    }
    if (trimmed.size() > 4 && trimmed.rfind("\\[", 0) == 0 && trimmed.substr(trimmed.size() - 2) == "\\]")
    {
        return Trim(trimmed.substr(2, trimmed.size() - 4));
    }
    if (const std::optional<std::string> env = ExtractLatexBeginEnvironmentName(trimmed, true);
        env.has_value() && IsSupportedBlockLatexEnvironment(*env))
    {
        const std::string end = "\\end{" + *env + "}";
        if (trimmed.size() > end.size() && trimmed.substr(trimmed.size() - end.size()) == end)
        {
            return trimmed;
        }
    }
    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> ExtractStandaloneMarkdownImage(const std::string &text)
{
    std::string alt_text;
    std::string url;
    std::size_t end_pos = std::string::npos;
    if (!ExtractMarkdownImage(Trim(text), 0, &alt_text, &url, &end_pos))
    {
        return std::nullopt;
    }
    if (end_pos != Trim(text).size())
    {
        return std::nullopt;
    }
    return std::make_pair(url, CollapseWhitespace(StripTitleMarkdownMarkers(alt_text)));
}

bool IsHeadingLine(const std::string &line)
{
    const std::string trimmed_left = TrimLeft(line);
    std::size_t level = 0;
    while (level < trimmed_left.size() && trimmed_left[level] == '#')
    {
        ++level;
    }
    return level >= 1 && level <= 6 && level < trimmed_left.size() &&
           std::isspace(static_cast<unsigned char>(trimmed_left[level]));
}

bool IsSetextHeadingUnderline(const std::string &trimmed, MarkdownBlock::Type *type)
{
    if (trimmed.empty())
    {
        return false;
    }
    if (std::all_of(trimmed.begin(), trimmed.end(), [](char ch)
                    { return ch == '='; }))
    {
        if (type != nullptr)
        {
            *type = MarkdownBlock::Type::Heading1;
        }
        return true;
    }
    if (std::all_of(trimmed.begin(), trimmed.end(), [](char ch)
                    { return ch == '-'; }))
    {
        if (type != nullptr)
        {
            *type = MarkdownBlock::Type::Heading2;
        }
        return true;
    }
    return false;
}

bool IsBulletListLine(const std::string &line)
{
    const std::string trimmed_left = TrimLeft(line);
    return trimmed_left.size() >= 2 && (trimmed_left[0] == '-' || trimmed_left[0] == '*' || trimmed_left[0] == '+') &&
           std::isspace(static_cast<unsigned char>(trimmed_left[1]));
}

bool IsTaskListLine(const std::string &line, bool *checked)
{
    const std::string trimmed_left = TrimLeft(line);
    if (trimmed_left.size() < 6 || (trimmed_left[0] != '-' && trimmed_left[0] != '*' && trimmed_left[0] != '+') ||
        !std::isspace(static_cast<unsigned char>(trimmed_left[1])) || trimmed_left[2] != '[' ||
        trimmed_left[4] != ']' || !std::isspace(static_cast<unsigned char>(trimmed_left[5])))
    {
        return false;
    }
    const char mark = static_cast<char>(std::tolower(static_cast<unsigned char>(trimmed_left[3])));
    if (mark != ' ' && mark != 'x')
    {
        return false;
    }
    if (checked != nullptr)
    {
        *checked = mark == 'x';
    }
    return true;
}

std::optional<std::size_t> NumberedListContentStart(const std::string &line)
{
    const std::string trimmed_left = TrimLeft(line);
    std::size_t i = 0;
    while (i < trimmed_left.size() && std::isdigit(static_cast<unsigned char>(trimmed_left[i])))
    {
        ++i;
    }
    if (i == 0 || i + 1 >= trimmed_left.size() || (trimmed_left[i] != '.' && trimmed_left[i] != ')') ||
        !std::isspace(static_cast<unsigned char>(trimmed_left[i + 1])))
    {
        return std::nullopt;
    }
    return i + 1;
}

bool IsNumberedListLine(const std::string &line)
{
    return NumberedListContentStart(line).has_value();
}

bool IsQuoteLine(const std::string &line)
{
    const std::string trimmed_left = TrimLeft(line);
    return trimmed_left.size() >= 2 && trimmed_left[0] == '>' &&
           std::isspace(static_cast<unsigned char>(trimmed_left[1]));
}

std::optional<std::string> ExtractMarkdownDefinitionListText(const std::string &line)
{
    std::size_t pos = 0;
    while (pos < line.size() && pos < 4 && line[pos] == ' ')
    {
        ++pos;
    }
    if (pos >= line.size() || line[pos] != ':')
    {
        return std::nullopt;
    }
    if (pos + 1 < line.size() && !std::isspace(static_cast<unsigned char>(line[pos + 1])))
    {
        return std::nullopt;
    }
    return Trim(line.substr(pos + 1));
}

bool IsMarkdownDefinitionListLine(const std::string &line)
{
    return ExtractMarkdownDefinitionListText(line).has_value();
}

std::vector<std::string> SplitMarkdownTableCellsRaw(const std::string &line)
{
    std::vector<std::string> cells;
    const std::string trimmed = Trim(line);
    if (trimmed.empty())
    {
        return cells;
    }

    std::string cell;
    bool saw_delimiter = false;
    for (std::size_t i = 0; i < trimmed.size();)
    {
        if (trimmed[i] == '`')
        {
            const std::size_t run_len = CountRepeatedChar(trimmed, i, '`');
            const std::size_t close = FindRepeatedCharRun(trimmed, i + run_len, '`', run_len);
            if (close != std::string::npos)
            {
                cell += trimmed.substr(i, close + run_len - i);
                i = close + run_len;
                continue;
            }
        }

        if (trimmed[i] == '|' && !IsEscaped(trimmed, i))
        {
            cells.push_back(Trim(cell));
            cell.clear();
            saw_delimiter = true;
            ++i;
            continue;
        }

        cell.push_back(trimmed[i++]);
    }
    cells.push_back(Trim(cell));

    if (!saw_delimiter)
    {
        return {};
    }
    if (!cells.empty() && cells.front().empty() && !trimmed.empty() && trimmed.front() == '|')
    {
        cells.erase(cells.begin());
    }
    if (!cells.empty() && cells.back().empty() && !trimmed.empty() && trimmed.back() == '|' &&
        !IsEscaped(trimmed, trimmed.size() - 1))
    {
        cells.pop_back();
    }
    return cells;
}

bool IsMarkdownTableLine(const std::string &line)
{
    const std::string trimmed = Trim(line);
    if (trimmed.size() < 3 || trimmed.find('|') == std::string::npos)
    {
        return false;
    }

    const std::vector<std::string> cells = SplitMarkdownTableCellsRaw(line);
    const bool has_content = std::any_of(cells.begin(), cells.end(), [](const std::string &cell)
                                         { return !cell.empty(); });
    return cells.size() >= 2 && has_content;
}

std::vector<std::string> SplitMarkdownTableCells(const std::string &line)
{
    const std::vector<std::string> cells = SplitMarkdownTableCellsRaw(line);
    if (cells.size() < 2)
    {
        return {};
    }
    return cells;
}

std::optional<std::size_t> MarkdownTableColumnCount(const std::string &line)
{
    const std::vector<std::string> cells = SplitMarkdownTableCells(line);
    if (cells.empty())
    {
        return std::nullopt;
    }
    return cells.size();
}

bool IsMarkdownTableSeparatorLine(const std::string &line)
{
    const std::vector<std::string> cells = SplitMarkdownTableCells(line);
    if (cells.size() < 2)
    {
        return false;
    }

    for (const std::string &cell : cells)
    {
        if (std::count(cell.begin(), cell.end(), '-') < 3)
        {
            return false;
        }
        for (const char ch : cell)
        {
            if (ch != '-' && ch != ':' && !IsAsciiSpace(ch))
            {
                return false;
            }
        }
    }
    return true;
}

bool IsMarkdownTableStart(const std::vector<std::string> &lines, std::size_t index)
{
    if (index >= lines.size() || IsMarkdownTableSeparatorLine(lines[index]))
    {
        return false;
    }
    const std::optional<std::size_t> columns = MarkdownTableColumnCount(lines[index]);
    if (!columns.has_value())
    {
        return false;
    }
    // 跳过表头与下一行之间的空行，兼容某些来源在每行之间插入空行的表格。
    std::size_t next = index + 1;
    while (next < lines.size() && Trim(lines[next]).empty())
    {
        ++next;
    }
    if (next >= lines.size())
    {
        return false;
    }
    const std::optional<std::size_t> next_columns = MarkdownTableColumnCount(lines[next]);
    return next_columns.has_value() && *columns == *next_columns;
}

bool IsPlainTableFenceLanguage(const std::string &language)
{
    const std::string normalized = ToLowerAscii(Trim(language));
    return normalized.empty() || normalized == "text" || normalized == "txt" || normalized == "plain" ||
           normalized == "plaintext" || normalized == "plain text";
}

bool IsMarkdownTableText(const std::string &text)
{
    const std::vector<std::string> lines = SplitLinesPreserveEmpty(NormalizeLineEndings(text));
    std::vector<std::string> table_lines;
    table_lines.reserve(lines.size());
    for (const std::string &line : lines)
    {
        if (!Trim(line).empty())
        {
            table_lines.push_back(line);
        }
    }

    if (table_lines.size() < 2 || !IsMarkdownTableStart(table_lines, 0))
    {
        return false;
    }

    const std::optional<std::size_t> expected_columns = MarkdownTableColumnCount(table_lines.front());
    if (!expected_columns.has_value())
    {
        return false;
    }
    for (const std::string &line : table_lines)
    {
        const std::optional<std::size_t> columns = MarkdownTableColumnCount(line);
        if (!columns.has_value() || *columns != *expected_columns)
        {
            return false;
        }
    }
    return true;
}

bool CanUseSetextHeadingText(const std::string &line)
{
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || trimmed.find('\\') != std::string::npos || IsHeadingLine(line) ||
        IsTaskListLine(line, nullptr) || IsBulletListLine(line) || IsNumberedListLine(line) || IsQuoteLine(line) ||
        IsMarkdownTableLine(line) || IsBlockEquationFenceStart(trimmed) || IsLooseBracketEquationFenceStart(trimmed))
    {
        return false;
    }

    char fence_char = '\0';
    std::size_t fence_len = 0;
    if (StartsWithFence(line, &fence_char, &fence_len))
    {
        return false;
    }
    return true;
}

bool CanUseMarkdownDefinitionListTerm(const std::string &line)
{
    const std::string trimmed = Trim(line);
    if (trimmed.empty() || IsHeadingLine(line) || IsTaskListLine(line, nullptr) || IsBulletListLine(line) ||
        IsNumberedListLine(line) || IsQuoteLine(line) || IsMarkdownDefinitionListLine(line) ||
        IsMarkdownReferenceDefinitionLine(line, nullptr, nullptr) || IsMarkdownTableLine(line) ||
        IsDividerLine(trimmed) || IsBlockEquationFenceStart(trimmed) || IsLooseBracketEquationFenceStart(trimmed))
    {
        return false;
    }

    char fence_char = '\0';
    std::size_t fence_len = 0;
    return !StartsWithFence(line, &fence_char, &fence_len);
}

bool IsParagraphBoundary(const std::string &line)
{
    const std::string trimmed = Trim(line);
    char fence_char = '\0';
    std::size_t fence_len = 0;
    return trimmed.empty() || IsDividerLine(trimmed) || IsHeadingLine(line) || IsTaskListLine(line, nullptr) ||
           IsBulletListLine(line) || IsNumberedListLine(line) || IsQuoteLine(line) ||
           ParseMarkdownColonAdmonitionStart(line).has_value() ||
           ParseMarkdownBangAdmonitionStart(line).has_value() ||
           IsBlockEquationFenceStart(trimmed) || IsLooseBracketEquationFenceStart(trimmed) ||
           StartsWithFence(line, &fence_char, &fence_len);
}

bool HasKnownMathUtf8Symbol(const std::string &text)
{
    static const std::vector<std::string> symbols = {
        "−", "×", "÷", "·", "∈", "∉", "≤", "≥", "≠", "≈", "∞", "∂", "∇", "∑", "∏",
        "∫", "√", "α", "β", "γ", "λ", "μ", "π", "φ", "Φ", "θ", "ω", "Ω", "Δ",
    };
    return std::any_of(symbols.begin(), symbols.end(), [&](const std::string &symbol)
                       { return text.find(symbol) != std::string::npos; });
}

bool LooksLikeBlockLatexExpression(const std::string &expression)
{
    const std::string cleaned = Trim(RemoveLatexUnderlineArtifactLines(expression));
    if (cleaned.empty())
    {
        return false;
    }
    if (HasKnownMathUtf8Symbol(cleaned) || cleaned.find('\\') != std::string::npos)
    {
        return true;
    }
    bool has_ascii_alnum = false;
    bool has_ascii_math_operator = false;
    bool only_ascii_math_chars = true;
    for (unsigned char ch : cleaned)
    {
        has_ascii_alnum = has_ascii_alnum || std::isalnum(ch) != 0;
        has_ascii_math_operator =
            has_ascii_math_operator || ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '<' || ch == '>';
        if (ch >= 128 || !(std::isalnum(ch) || std::isspace(ch) || ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
                           ch == '=' || ch == '^' || ch == '_' || ch == '{' || ch == '}' || ch == '[' || ch == ']' ||
                           ch == '(' || ch == ')' || ch == ',' || ch == '.' || ch == '<' || ch == '>' || ch == '|'))
        {
            only_ascii_math_chars = false;
        }
    }
    if (only_ascii_math_chars && has_ascii_alnum && has_ascii_math_operator)
    {
        return true;
    }
    if (LooksLikeAsciiFunctionFormula(cleaned))
    {
        return true;
    }
    return cleaned.find_first_of("=^_{}") != std::string::npos;
}

bool IsKnownNonMathDollarToken(const std::string &token)
{
    const std::string lower = ToLowerAscii(Trim(token));
    static const std::vector<std::string> known_tokens = {
        "home", "path", "user", "username", "temp", "tmp", "appdata", "localappdata", "programfiles", "systemroot",
    };
    return std::find(known_tokens.begin(), known_tokens.end(), lower) != known_tokens.end();
}

bool IsPlainUppercaseWord(const std::string &token)
{
    const std::string trimmed = Trim(token);
    if (trimmed.size() <= 1)
    {
        return false;
    }
    return std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char ch)
                       { return std::isupper(ch) != 0; });
}

bool LooksLikeInlineLatexExpression(const std::string &expression)
{
    const std::string trimmed = Trim(expression);
    if (trimmed.empty())
    {
        return false;
    }
    if (IsKnownNonMathDollarToken(trimmed))
    {
        return false;
    }
    if (IsPlainUppercaseWord(trimmed))
    {
        return false;
    }
    if (HasKnownMathUtf8Symbol(trimmed))
    {
        return true;
    }

    bool has_ascii_letter = false;
    bool has_lower = false;
    bool has_non_alpha = false;
    std::string alpha_token;
    for (unsigned char ch : trimmed)
    {
        if (std::isdigit(ch) || ch == '\\' || ch == '^' || ch == '_' || ch == '=' || ch == '+' || ch == '-' ||
            ch == '*' || ch == '/' || ch == '<' || ch == '>' || ch == '{' || ch == '}' || ch == '[' ||
            ch == ']' || ch == '(' || ch == ')' || ch == ',' || ch == '.')
        {
            return true;
        }
        if (std::isalpha(ch))
        {
            has_ascii_letter = true;
            has_lower = has_lower || std::islower(ch) != 0;
            alpha_token.push_back(static_cast<char>(ch));
            continue;
        }
        if (!std::isspace(ch))
        {
            has_non_alpha = true;
        }
    }

    if (!has_non_alpha && has_ascii_letter)
    {
        return true;
    }
    return false;
}

bool LooksLikeAsciiFunctionFormula(const std::string &expression)
{
    const std::string trimmed = Trim(expression);
    if (trimmed.size() < 4 || !IsAsciiAlpha(trimmed.front()) || trimmed.back() != ')')
    {
        return false;
    }

    std::size_t open = 0;
    while (open < trimmed.size() && (IsAsciiAlnum(trimmed[open]) || trimmed[open] == '_'))
    {
        ++open;
    }
    if (open == 0 || open + 2 >= trimmed.size() || trimmed[open] != '(')
    {
        return false;
    }
    const std::string inside = Trim(trimmed.substr(open + 1, trimmed.size() - open - 2));
    if (inside.empty())
    {
        return false;
    }
    return std::all_of(inside.begin(), inside.end(), [](unsigned char ch)
                       {
                           return ch < 128 && (std::isalnum(ch) || std::isspace(ch) || ch == '+' || ch == '-' ||
                                               ch == '*' || ch == '/' || ch == '^' || ch == '_' || ch == ',' ||
                                               ch == '.' || ch == '(' || ch == ')');
                       });
}

bool LooksLikeImplicitParenthesizedLatexExpression(const std::string &expression)
{
    const std::string trimmed = Trim(expression);
    return !trimmed.empty() && trimmed.find('\\') != std::string::npos && LooksLikeInlineLatexExpression(trimmed);
}

std::optional<std::size_t> FindMatchingInlineParenthesis(const std::string &text, std::size_t open_pos)
{
    if (open_pos >= text.size() || text[open_pos] != '(')
    {
        return std::nullopt;
    }

    int depth = 0;
    for (std::size_t pos = open_pos; pos < text.size(); ++pos)
    {
        if (text[pos] == '\n' || text[pos] == '\r')
        {
            return std::nullopt;
        }
        if (IsEscaped(text, pos))
        {
            continue;
        }
        if (text[pos] == '(')
        {
            ++depth;
            continue;
        }
        if (text[pos] == ')')
        {
            --depth;
            if (depth == 0)
            {
                return pos;
            }
        }
    }
    return std::nullopt;
}

bool IsInlineDollarOpenAllowed(const std::string &text, std::size_t pos)
{
    if (pos + 1 >= text.size() || IsAsciiSpace(text[pos + 1]))
    {
        return false;
    }
    if (pos > 0)
    {
        const char prev = text[pos - 1];
        if (IsAsciiAlnum(prev) || prev == '/' || prev == '\\')
        {
            return false;
        }
    }
    return true;
}

bool IsInlineDollarCloseAllowed(const std::string &text, std::size_t pos)
{
    if (pos == 0 || IsAsciiSpace(text[pos - 1]))
    {
        return false;
    }
    if (pos + 1 < text.size())
    {
        const char next = text[pos + 1];
        if (IsAsciiAlnum(next) || next == '/' || next == '\\')
        {
            return false;
        }
    }
    return true;
}

std::string StripNonMathDollarMarkersForPlainText(const std::string &text)
{
    std::string output;
    output.reserve(text.size());

    for (std::size_t i = 0; i < text.size();)
    {
        if (text.compare(i, 2, "$$") == 0)
        {
            output += "$$";
            i += 2;
            continue;
        }

        if (text[i] == '$' && !IsEscaped(text, i) && IsInlineDollarOpenAllowed(text, i))
        {
            std::size_t close_pos = std::string::npos;
            std::size_t search = i + 1;
            while (search < text.size())
            {
                const std::size_t candidate = text.find('$', search);
                if (candidate == std::string::npos)
                {
                    break;
                }
                if (!IsEscaped(text, candidate))
                {
                    const std::string expr = text.substr(i + 1, candidate - i - 1);
                    const std::string trimmed_expr = Trim(expr);
                    if (IsInlineDollarCloseAllowed(text, candidate) ||
                        (!trimmed_expr.empty() && !LooksLikeInlineLatexExpression(expr)))
                    {
                        close_pos = candidate;
                        break;
                    }
                }
                search = candidate + 1;
            }

            if (close_pos != std::string::npos)
            {
                const std::string expr = text.substr(i + 1, close_pos - i - 1);
                const std::string trimmed_expr = Trim(expr);
                if (!trimmed_expr.empty() && !LooksLikeInlineLatexExpression(expr))
                {
                    output += trimmed_expr;
                    i = close_pos + 1;
                    continue;
                }
            }
        }

        output.push_back(text[i]);
        ++i;
    }

    return output;
}

std::size_t FindUnescapedChar(const std::string &text, char ch, std::size_t begin)
{
    std::size_t pos = begin;
    while ((pos = text.find(ch, pos)) != std::string::npos)
    {
        if (!IsEscaped(text, pos))
        {
            return pos;
        }
        ++pos;
    }
    return std::string::npos;
}

bool IsSupportedMarkdownLinkUrl(const std::string &url)
{
    const std::string lower = ToLowerAscii(Trim(url));
    return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0 ||
           lower.rfind("mailto:", 0) == 0 || lower.rfind("notion://", 0) == 0;
}

bool IsSupportedExternalImageUrl(const std::string &url)
{
    const std::string lower = ToLowerAscii(Trim(url));
    return (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) &&
           lower.find_first_of("<>\r\n") == std::string::npos;
}

std::optional<std::string> NormalizeMarkdownLinkDestination(std::string destination)
{
    destination = Trim(std::move(destination));
    if (destination.empty())
    {
        return std::nullopt;
    }
    if (destination.front() == '<' && destination.back() == '>' && destination.size() > 2)
    {
        destination = Trim(destination.substr(1, destination.size() - 2));
    }
    if (!IsSupportedMarkdownLinkUrl(destination))
    {
        return std::nullopt;
    }
    return destination;
}

std::optional<std::string> NormalizeMarkdownImageDestination(std::string destination)
{
    destination = Trim(std::move(destination));
    if (destination.empty())
    {
        return std::nullopt;
    }
    if (destination.front() == '<' && destination.back() == '>' && destination.size() > 2)
    {
        destination = Trim(destination.substr(1, destination.size() - 2));
    }
    if (!IsSupportedExternalImageUrl(destination))
    {
        return std::nullopt;
    }
    return destination;
}

bool ExtractMarkdownInlineLink(const std::string &text, std::size_t pos, std::string *label, std::string *url,
                               std::size_t *end_pos)
{
    if (pos >= text.size() || text[pos] != '[' || IsEscaped(text, pos))
    {
        return false;
    }
    const std::size_t label_end = FindUnescapedChar(text, ']', pos + 1);
    if (label_end == std::string::npos || label_end + 1 >= text.size() || text[label_end + 1] != '(')
    {
        return false;
    }

    const std::string raw_label = text.substr(pos + 1, label_end - pos - 1);
    if (Trim(raw_label).empty())
    {
        return false;
    }

    const std::size_t dest_begin = label_end + 2;
    if (dest_begin >= text.size())
    {
        return false;
    }

    std::size_t dest_end = std::string::npos;
    if (text[dest_begin] == '<')
    {
        const std::size_t angle_end = FindUnescapedChar(text, '>', dest_begin + 1);
        if (angle_end == std::string::npos || angle_end + 1 >= text.size() || text[angle_end + 1] != ')')
        {
            return false;
        }
        dest_end = angle_end + 1;
        *end_pos = angle_end + 2;
    }
    else
    {
        int paren_depth = 0;
        for (std::size_t cursor = dest_begin; cursor < text.size(); ++cursor)
        {
            if (IsEscaped(text, cursor))
            {
                continue;
            }
            if (text[cursor] == '(')
            {
                ++paren_depth;
                continue;
            }
            if (text[cursor] == ')')
            {
                if (paren_depth == 0)
                {
                    dest_end = cursor;
                    *end_pos = cursor + 1;
                    break;
                }
                --paren_depth;
            }
        }
    }

    if (dest_end == std::string::npos || dest_end <= dest_begin)
    {
        return false;
    }

    const std::optional<std::string> normalized_url =
        NormalizeMarkdownLinkDestination(text.substr(dest_begin, dest_end - dest_begin));
    if (!normalized_url.has_value())
    {
        return false;
    }

    *label = raw_label;
    *url = *normalized_url;
    return true;
}

bool ExtractMarkdownImage(const std::string &text, std::size_t pos, std::string *alt_text, std::string *url,
                          std::size_t *end_pos)
{
    if (pos + 1 >= text.size() || text[pos] != '!' || text[pos + 1] != '[' || IsEscaped(text, pos))
    {
        return false;
    }
    const std::size_t label_begin = pos + 1;
    const std::size_t label_end = FindUnescapedChar(text, ']', label_begin + 1);
    if (label_end == std::string::npos || label_end + 1 >= text.size() || text[label_end + 1] != '(')
    {
        return false;
    }

    const std::size_t dest_begin = label_end + 2;
    if (dest_begin >= text.size())
    {
        return false;
    }

    std::size_t dest_end = std::string::npos;
    if (text[dest_begin] == '<')
    {
        const std::size_t angle_end = FindUnescapedChar(text, '>', dest_begin + 1);
        if (angle_end == std::string::npos || angle_end + 1 >= text.size() || text[angle_end + 1] != ')')
        {
            return false;
        }
        dest_end = angle_end + 1;
        *end_pos = angle_end + 2;
    }
    else
    {
        int paren_depth = 0;
        for (std::size_t cursor = dest_begin; cursor < text.size(); ++cursor)
        {
            if (IsEscaped(text, cursor))
            {
                continue;
            }
            if (text[cursor] == '(')
            {
                ++paren_depth;
                continue;
            }
            if (text[cursor] == ')')
            {
                if (paren_depth == 0)
                {
                    dest_end = cursor;
                    *end_pos = cursor + 1;
                    break;
                }
                --paren_depth;
            }
        }
    }

    if (dest_end == std::string::npos || dest_end <= dest_begin)
    {
        return false;
    }

    const std::optional<std::string> normalized_url =
        NormalizeMarkdownImageDestination(text.substr(dest_begin, dest_end - dest_begin));
    if (!normalized_url.has_value())
    {
        return false;
    }

    *alt_text = text.substr(label_begin + 1, label_end - label_begin - 1);
    *url = *normalized_url;
    return true;
}

std::string NormalizeLinkReferenceLabel(std::string label)
{
    label = CollapseWhitespace(Trim(std::move(label)));
    return ToLowerAscii(label);
}

bool ExtractMarkdownFootnoteReference(const std::string &text, std::size_t pos, std::string *label,
                                      std::size_t *end_pos)
{
    if (pos + 3 >= text.size() || text[pos] != '[' || text[pos + 1] != '^' || IsEscaped(text, pos))
    {
        return false;
    }
    const std::size_t label_end = FindUnescapedChar(text, ']', pos + 2);
    if (label_end == std::string::npos || label_end == pos + 2)
    {
        return false;
    }
    const std::string raw_label = Trim(text.substr(pos + 2, label_end - pos - 2));
    if (raw_label.empty() || raw_label.find_first_of("\r\n[]") != std::string::npos)
    {
        return false;
    }
    if (label != nullptr)
    {
        *label = raw_label;
    }
    if (end_pos != nullptr)
    {
        *end_pos = label_end + 1;
    }
    return true;
}

bool IsMarkdownFootnoteDefinitionLine(const std::string &line, std::string *label = nullptr,
                                      std::string *definition = nullptr)
{
    std::size_t pos = 0;
    while (pos < line.size() && pos < 4 && line[pos] == ' ')
    {
        ++pos;
    }
    if (pos > 3 || pos + 3 >= line.size() || line[pos] != '[' || line[pos + 1] != '^' || IsEscaped(line, pos))
    {
        return false;
    }
    const std::size_t label_end = FindUnescapedChar(line, ']', pos + 2);
    if (label_end == std::string::npos || label_end == pos + 2 || label_end + 1 >= line.size() ||
        line[label_end + 1] != ':')
    {
        return false;
    }
    const std::string raw_label = Trim(line.substr(pos + 2, label_end - pos - 2));
    if (raw_label.empty() || raw_label.find_first_of("\r\n[]") != std::string::npos)
    {
        return false;
    }
    if (label != nullptr)
    {
        *label = raw_label;
    }
    if (definition != nullptr)
    {
        *definition = Trim(line.substr(label_end + 2));
    }
    return true;
}

bool IsMarkdownReferenceDefinitionLine(const std::string &line, std::string *normalized_label = nullptr,
                                       std::string *url = nullptr)
{
    std::size_t pos = 0;
    while (pos < line.size() && pos < 4 && line[pos] == ' ')
    {
        ++pos;
    }
    if (pos > 3 || pos >= line.size() || line[pos] != '[' || IsEscaped(line, pos))
    {
        return false;
    }

    const std::size_t label_end = FindUnescapedChar(line, ']', pos + 1);
    if (label_end == std::string::npos || label_end + 1 >= line.size() || line[label_end + 1] != ':')
    {
        return false;
    }

    const std::string label = NormalizeLinkReferenceLabel(line.substr(pos + 1, label_end - pos - 1));
    if (label.empty() || label[0] == '^')
    {
        return false;
    }

    std::size_t dest_begin = label_end + 2;
    while (dest_begin < line.size() && IsAsciiSpace(line[dest_begin]))
    {
        ++dest_begin;
    }
    if (dest_begin >= line.size())
    {
        return false;
    }

    std::size_t dest_end = std::string::npos;
    if (line[dest_begin] == '<')
    {
        const std::size_t angle_end = FindUnescapedChar(line, '>', dest_begin + 1);
        if (angle_end == std::string::npos)
        {
            return false;
        }
        dest_end = angle_end + 1;
    }
    else
    {
        dest_end = dest_begin;
        while (dest_end < line.size() && !IsAsciiSpace(line[dest_end]))
        {
            ++dest_end;
        }
    }

    const std::optional<std::string> normalized_url =
        NormalizeMarkdownLinkDestination(line.substr(dest_begin, dest_end - dest_begin));
    if (!normalized_url.has_value())
    {
        return false;
    }

    if (normalized_label != nullptr)
    {
        *normalized_label = label;
    }
    if (url != nullptr)
    {
        *url = *normalized_url;
    }
    return true;
}

LinkReferenceMap CollectMarkdownLinkReferences(const std::vector<std::string> &lines)
{
    LinkReferenceMap references;
    std::size_t i = 0;
    while (i < lines.size())
    {
        const std::string &line = lines[i];
        const std::string trimmed = Trim(line);
        if (trimmed.empty())
        {
            ++i;
            continue;
        }

        char fence_char = '\0';
        std::size_t fence_len = 0;
        if (StartsWithFence(line, &fence_char, &fence_len))
        {
            ++i;
            while (i < lines.size())
            {
                if (IsClosingFenceLine(lines[i], fence_char, fence_len))
                {
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }

        std::string label;
        std::string url;
        if (IsMarkdownReferenceDefinitionLine(line, &label, &url) && references.find(label) == references.end())
        {
            references.emplace(std::move(label), std::move(url));
            ++i;
            continue;
        }

        if (IsParagraphBoundary(line))
        {
            ++i;
            continue;
        }

        ++i;
        while (i < lines.size() && !IsParagraphBoundary(lines[i]))
        {
            ++i;
        }
    }
    return references;
}

bool ExtractMarkdownReferenceLink(const std::string &text, std::size_t pos, const LinkReferenceMap *references,
                                  std::string *label, std::string *url, std::size_t *end_pos)
{
    if (references == nullptr || references->empty() || pos >= text.size() || text[pos] != '[' || IsEscaped(text, pos))
    {
        return false;
    }

    const std::size_t label_end = FindUnescapedChar(text, ']', pos + 1);
    if (label_end == std::string::npos || label_end == pos + 1)
    {
        return false;
    }

    const std::string raw_label = text.substr(pos + 1, label_end - pos - 1);
    std::string reference_label;
    std::size_t candidate_end = label_end + 1;

    if (candidate_end < text.size() && text[candidate_end] == '[' && !IsEscaped(text, candidate_end))
    {
        const std::size_t reference_end = FindUnescapedChar(text, ']', candidate_end + 1);
        if (reference_end == std::string::npos)
        {
            return false;
        }
        reference_label = text.substr(candidate_end + 1, reference_end - candidate_end - 1);
        if (reference_label.empty())
        {
            reference_label = raw_label;
        }
        candidate_end = reference_end + 1;
    }
    else
    {
        reference_label = raw_label;
    }

    const auto found = references->find(NormalizeLinkReferenceLabel(reference_label));
    if (found == references->end())
    {
        return false;
    }

    *label = raw_label;
    *url = found->second;
    *end_pos = candidate_end;
    return true;
}

bool IsAutolinkEmailAddress(const std::string &text)
{
    if (text.empty() || text.find_first_of("<> \t\r\n") != std::string::npos)
    {
        return false;
    }
    const std::size_t at = text.find('@');
    if (at == std::string::npos || at == 0 || at + 1 >= text.size() || text.find('@', at + 1) != std::string::npos)
    {
        return false;
    }
    const std::size_t dot = text.find('.', at + 2);
    if (dot == std::string::npos || dot + 1 >= text.size())
    {
        return false;
    }
    for (unsigned char ch : text)
    {
        if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '%' || ch == '+' || ch == '-' || ch == '@'))
        {
            return false;
        }
    }
    return true;
}

bool ExtractMarkdownAutolink(const std::string &text, std::size_t pos, std::string *label, std::string *url,
                             std::size_t *end_pos)
{
    if (pos >= text.size() || text[pos] != '<' || IsEscaped(text, pos))
    {
        return false;
    }
    const std::size_t close = FindUnescapedChar(text, '>', pos + 1);
    if (close == std::string::npos || close == pos + 1)
    {
        return false;
    }

    const std::string candidate = text.substr(pos + 1, close - pos - 1);
    if (candidate.find_first_of("< \t\r\n") != std::string::npos)
    {
        return false;
    }
    if (const std::optional<std::string> normalized_url = NormalizeMarkdownLinkDestination(candidate))
    {
        *label = candidate;
        *url = *normalized_url;
        *end_pos = close + 1;
        return true;
    }
    if (IsAutolinkEmailAddress(candidate))
    {
        *label = candidate;
        *url = "mailto:" + candidate;
        *end_pos = close + 1;
        return true;
    }
    return false;
}

bool IsSingleEmphasisDelimiterAt(const std::string &text, std::size_t pos, char delimiter, bool closing)
{
    if (pos >= text.size() || text[pos] != delimiter || IsEscaped(text, pos))
    {
        return false;
    }
    if ((pos > 0 && text[pos - 1] == delimiter) || (pos + 1 < text.size() && text[pos + 1] == delimiter))
    {
        return false;
    }
    if (closing)
    {
        if (pos == 0 || IsAsciiSpace(text[pos - 1]))
        {
            return false;
        }
        if (delimiter == '_' && pos + 1 < text.size() && IsAsciiAlnum(text[pos + 1]))
        {
            return false;
        }
        return true;
    }

    if (pos + 1 >= text.size() || IsAsciiSpace(text[pos + 1]))
    {
        return false;
    }
    if (delimiter == '_' && pos > 0 && IsAsciiAlnum(text[pos - 1]))
    {
        return false;
    }
    return true;
}

std::size_t FindSingleEmphasisClose(const std::string &text, std::size_t begin, char delimiter)
{
    std::size_t pos = begin;
    while ((pos = text.find(delimiter, pos)) != std::string::npos)
    {
        if (IsSingleEmphasisDelimiterAt(text, pos, delimiter, true))
        {
            return pos;
        }
        ++pos;
    }
    return std::string::npos;
}

bool IsMarkdownHighlightDelimiterAt(const std::string &text, std::size_t pos, bool closing)
{
    if (pos + 1 >= text.size() || text.compare(pos, 2, "==") != 0 || IsEscaped(text, pos))
    {
        return false;
    }

    const char prev = pos == 0 ? '\0' : text[pos - 1];
    const char next = pos + 2 < text.size() ? text[pos + 2] : '\0';
    if (closing)
    {
        return prev != '\0' && !IsAsciiSpace(prev) && prev != '=' && (next == '\0' || (!IsAsciiAlnum(next) && next != '='));
    }
    return next != '\0' && !IsAsciiSpace(next) && next != '=' && (prev == '\0' || (!IsAsciiAlnum(prev) && prev != '='));
}

std::size_t FindMarkdownHighlightClose(const std::string &text, std::size_t begin)
{
    std::size_t pos = begin;
    while ((pos = text.find("==", pos)) != std::string::npos)
    {
        if (IsMarkdownHighlightDelimiterAt(text, pos, true))
        {
            return pos;
        }
        pos += 2;
    }
    return std::string::npos;
}

std::vector<InlineSegment> ParseInlineMarkdown(const std::string &text, const LinkReferenceMap *references = nullptr)
{
    std::vector<InlineSegment> segments;
    bool bold = false;
    bool strikethrough = false;
    bool underline = false;
    bool markdown_highlight = false;
    std::string current_color = "default";
    bool italic = false;
    char italic_delimiter = '\0';
    std::size_t i = 0;

    auto push_text = [&](const std::string &content, bool is_code = false, const std::string &link_url = "")
    {
        const std::string normalized_content =
            is_code ? content : UnescapeMarkdownText(StripNonMathDollarMarkersForPlainText(content));
        if (normalized_content.empty())
        {
            return;
        }
        if (!segments.empty() && segments.back().type == InlineSegment::Type::Text &&
            segments.back().bold == bold && segments.back().code == is_code &&
            segments.back().strikethrough == strikethrough && segments.back().link_url == link_url &&
            segments.back().italic == italic && segments.back().underline == underline &&
            segments.back().color == current_color)
        {
            segments.back().content += normalized_content;
            return;
        }
        segments.push_back(
            {InlineSegment::Type::Text, normalized_content, bold, is_code, strikethrough, underline, link_url, italic,
             current_color});
    };

    auto push_segments = [&](std::vector<InlineSegment> link_segments, const std::string &link_url)
    {
        for (InlineSegment &segment : link_segments)
        {
            if (segment.type == InlineSegment::Type::Text)
            {
                segment.bold = segment.bold || bold;
                segment.strikethrough = segment.strikethrough || strikethrough;
                segment.underline = segment.underline || underline;
                segment.link_url = link_url;
                segment.italic = segment.italic || italic;
                if (segment.color == "default" && current_color != "default")
                {
                    segment.color = current_color;
                }
            }
            segments.push_back(std::move(segment));
        }
    };

    while (i < text.size())
    {
        if (text.compare(i, kHtmlMarkRichTextMarker.size(), kHtmlMarkRichTextMarker) == 0)
        {
            current_color = current_color == "yellow_background" ? "default" : "yellow_background";
            i += kHtmlMarkRichTextMarker.size();
            continue;
        }
        if (text.compare(i, kHtmlColorRichTextMarkerPrefix.size(), kHtmlColorRichTextMarkerPrefix) == 0)
        {
            const std::size_t marker_end = text.find(kHtmlColorRichTextMarkerSuffix,
                                                     i + kHtmlColorRichTextMarkerPrefix.size());
            if (marker_end != std::string::npos)
            {
                current_color = text.substr(i + kHtmlColorRichTextMarkerPrefix.size(),
                                            marker_end - i - kHtmlColorRichTextMarkerPrefix.size());
                if (current_color.empty())
                {
                    current_color = "default";
                }
                i = marker_end + kHtmlColorRichTextMarkerSuffix.size();
                continue;
            }
        }
        if (text.compare(i, 2, "==") == 0 && !IsEscaped(text, i))
        {
            if (markdown_highlight && IsMarkdownHighlightDelimiterAt(text, i, true))
            {
                markdown_highlight = false;
                current_color = "default";
                i += 2;
                continue;
            }
            if (!markdown_highlight && IsMarkdownHighlightDelimiterAt(text, i, false) &&
                FindMarkdownHighlightClose(text, i + 2) != std::string::npos)
            {
                markdown_highlight = true;
                current_color = "yellow_background";
                i += 2;
                continue;
            }
        }
        if (text.compare(i, 2, "~~") == 0 && !IsEscaped(text, i) &&
            (strikethrough || FindRepeatedCharRun(text, i + 2, '~', 2) != std::string::npos))
        {
            strikethrough = !strikethrough;
            i += 2;
            continue;
        }
        if (text.compare(i, 2, "++") == 0 && !IsEscaped(text, i) &&
            (underline || FindRepeatedCharRun(text, i + 2, '+', 2) != std::string::npos))
        {
            underline = !underline;
            i += 2;
            continue;
        }
        if (text.compare(i, 2, "**") == 0)
        {
            bold = !bold;
            i += 2;
            continue;
        }
        if ((text[i] == '*' || text[i] == '_') &&
            ((italic && text[i] == italic_delimiter && IsSingleEmphasisDelimiterAt(text, i, text[i], true)) ||
             (!italic && IsSingleEmphasisDelimiterAt(text, i, text[i], false) &&
              FindSingleEmphasisClose(text, i + 1, text[i]) != std::string::npos)))
        {
            if (italic)
            {
                italic = false;
                italic_delimiter = '\0';
            }
            else
            {
                italic = true;
                italic_delimiter = text[i];
            }
            ++i;
            continue;
        }
        if (text[i] == '`')
        {
            const std::size_t run_len = CountRepeatedChar(text, i, '`');
            const std::size_t close = FindRepeatedCharRun(text, i + run_len, '`', run_len);
            if (close != std::string::npos)
            {
                push_text(text.substr(i + run_len, close - i - run_len), true);
                i = close + run_len;
                continue;
            }
        }

        std::string link_label;
        std::string link_url;
        std::size_t link_end = std::string::npos;
        std::string footnote_label;
        std::size_t footnote_end = std::string::npos;
        if (ExtractMarkdownFootnoteReference(text, i, &footnote_label, &footnote_end))
        {
            push_text(BuildSupSubFallback(footnote_label, true));
            i = footnote_end;
            continue;
        }
        if (ExtractMarkdownImage(text, i, &link_label, &link_url, &link_end))
        {
            const std::string fallback_label = Trim(link_label).empty() ? link_url : link_label;
            push_text(fallback_label, false, link_url);
            i = link_end;
            continue;
        }
        if (ExtractMarkdownInlineLink(text, i, &link_label, &link_url, &link_end))
        {
            push_segments(ParseInlineMarkdown(link_label, references), link_url);
            i = link_end;
            continue;
        }
        if (ExtractMarkdownReferenceLink(text, i, references, &link_label, &link_url, &link_end))
        {
            push_segments(ParseInlineMarkdown(link_label, references), link_url);
            i = link_end;
            continue;
        }
        if (ExtractMarkdownAutolink(text, i, &link_label, &link_url, &link_end))
        {
            push_text(link_label, false, link_url);
            i = link_end;
            continue;
        }

        auto try_equation = [&](const std::string &open, const std::string &close) -> bool
        {
            if (text.compare(i, open.size(), open) != 0 || IsEscaped(text, i))
            {
                return false;
            }
            if (open == "$" && !IsInlineDollarOpenAllowed(text, i))
            {
                return false;
            }

            std::size_t close_pos = std::string::npos;
            std::size_t search = i + open.size();
            while (search < text.size())
            {
                const std::size_t candidate = text.find(close, search);
                if (candidate == std::string::npos)
                {
                    break;
                }
                if (!IsEscaped(text, candidate))
                {
                    const std::string expr = text.substr(i + open.size(), candidate - i - open.size());
                    const std::string trimmed_expr = Trim(expr);
                    const bool is_non_math_dollar =
                        open == "$" && !trimmed_expr.empty() && !LooksLikeInlineLatexExpression(expr);
                    if (open != "$" || IsInlineDollarCloseAllowed(text, candidate) || is_non_math_dollar)
                    {
                        close_pos = candidate;
                        break;
                    }
                }
                search = candidate + close.size();
            }
            if (close_pos == std::string::npos)
            {
                return false;
            }

            const std::string expr = text.substr(i + open.size(), close_pos - i - open.size());
            const std::string trimmed_expr = Trim(expr);
            if (trimmed_expr.empty())
            {
                return false;
            }
            if (open == "$" && !LooksLikeInlineLatexExpression(expr))
            {
                push_text(trimmed_expr);
                i = close_pos + close.size();
                return true;
            }
            segments.push_back({InlineSegment::Type::Equation, expr, false, false});
            i = close_pos + close.size();
            return true;
        };

        if (try_equation("$$", "$$") || try_equation("\\(", "\\)") || try_equation("$", "$"))
        {
            continue;
        }

        if (text[i] == '(' && !IsEscaped(text, i) &&
            (i == 0 || (!IsAsciiAlnum(text[i - 1]) && text[i - 1] != '\\')))
        {
            const std::optional<std::size_t> close_pos = FindMatchingInlineParenthesis(text, i);
            if (close_pos.has_value())
            {
                const std::string expr = text.substr(i, *close_pos - i + 1);
                const std::string inner_expr = expr.substr(1, expr.size() - 2);
                if (LooksLikeImplicitParenthesizedLatexExpression(inner_expr))
                {
                    segments.push_back({InlineSegment::Type::Equation, expr, false, false});
                    i = *close_pos + 1;
                    continue;
                }
            }
        }

        std::size_t next = i + 1;
        while (next < text.size() && text.compare(next, 2, "**") != 0 &&
               text.compare(next, kHtmlMarkRichTextMarker.size(), kHtmlMarkRichTextMarker) != 0 &&
               text.compare(next, kHtmlColorRichTextMarkerPrefix.size(), kHtmlColorRichTextMarkerPrefix) != 0 &&
               !(text.compare(next, 2, "==") == 0 && !IsEscaped(text, next)) &&
               !(text.compare(next, 2, "++") == 0 && !IsEscaped(text, next)) &&
               !(text.compare(next, 2, "~~") == 0 && !IsEscaped(text, next)) && text[next] != '`' &&
               !((text[next] == '*' || text[next] == '_') && !IsEscaped(text, next)) &&
               !(text[next] == '[' && !IsEscaped(text, next)) &&
               !(text[next] == '!' && next + 1 < text.size() && text[next + 1] == '[' && !IsEscaped(text, next)) &&
               !(text[next] == '<' && !IsEscaped(text, next)) &&
               !(text[next] == '(' && !IsEscaped(text, next)) &&
               !(text[next] == '$' && !IsEscaped(text, next)) &&
               !(text[next] == '\\' && next + 1 < text.size() && text[next + 1] == '('))
        {
            ++next;
        }
        push_text(text.substr(i, next - i));
        i = next;
    }
    return segments;
}

std::string DedentBlockText(const std::string &text)
{
    const std::vector<std::string> lines = SplitLinesPreserveEmpty(text);
    std::size_t common_indent = std::string::npos;
    for (const std::string &line : lines)
    {
        if (Trim(line).empty())
        {
            continue;
        }
        std::size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ')
        {
            ++indent;
        }
        common_indent = (common_indent == std::string::npos) ? indent : std::min(common_indent, indent);
    }
    if (common_indent == std::string::npos || common_indent == 0)
    {
        return text;
    }

    std::ostringstream oss;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i != 0)
        {
            oss << "\n";
        }
        const std::string &line = lines[i];
        oss << (Trim(line).empty() ? "" : line.substr(std::min(common_indent, line.size())));
    }
    return oss.str();
}

std::optional<MarkdownAlertStyle> ExtractMarkdownAlertFromQuote(const std::string &quote, std::string *content)
{
    std::vector<std::string> lines = SplitLinesPreserveEmpty(NormalizeLineEndings(quote));
    if (lines.empty())
    {
        return std::nullopt;
    }

    std::size_t marker_end = 0;
    const std::optional<MarkdownAlertStyle> style = ParseMarkdownAlertMarkerLine(lines.front(), &marker_end);
    if (!style.has_value())
    {
        return std::nullopt;
    }

    lines.front() = Trim(Trim(lines.front()).substr(marker_end));
    std::ostringstream oss;
    bool wrote = false;
    for (const std::string &line : lines)
    {
        if (!wrote && Trim(line).empty())
        {
            continue;
        }
        if (wrote)
        {
            oss << "\n";
        }
        oss << line;
        wrote = true;
    }

    if (content != nullptr)
    {
        *content = DedentBlockText(Trim(oss.str()));
    }
    return style;
}

std::size_t CountLeadingSpaces(const std::string &line)
{
    std::size_t count = 0;
    while (count < line.size() && line[count] == ' ')
    {
        ++count;
    }
    return count;
}

std::string StripMarkdownIndentedAdmonitionLine(const std::string &line)
{
    if (Trim(line).empty())
    {
        return "";
    }
    const std::size_t indent = CountLeadingSpaces(line);
    return line.substr(std::min<std::size_t>(4, indent));
}

std::vector<MarkdownBlock> ParseMarkdownBlocks(const std::string &content)
{
    const std::vector<std::string> lines = SplitLinesPreserveEmpty(StripUtf8Bom(NormalizeLineEndings(content)));
    const LinkReferenceMap link_references = CollectMarkdownLinkReferences(lines);
    auto parse_inline = [&](const std::string &text)
    {
        return ParseInlineMarkdown(text, &link_references);
    };
    std::vector<MarkdownBlock> blocks;
    blocks.reserve(std::min<std::size_t>(lines.size(), 256));

    std::size_t i = 0;
    while (i < lines.size())
    {
        const std::string &line = lines[i];
        const std::string trimmed = Trim(line);
        if (trimmed.empty())
        {
            ++i;
            continue;
        }
        std::string footnote_label;
        std::string footnote_text;
        if (IsMarkdownFootnoteDefinitionLine(line, &footnote_label, &footnote_text))
        {
            std::size_t cursor = i + 1;
            while (cursor < lines.size() && !Trim(lines[cursor]).empty())
            {
                std::size_t indent = 0;
                while (indent < lines[cursor].size() && lines[cursor][indent] == ' ')
                {
                    ++indent;
                }
                if (indent < 4)
                {
                    break;
                }
                if (!footnote_text.empty())
                {
                    footnote_text += "\n";
                }
                footnote_text += Trim(lines[cursor]);
                ++cursor;
            }

            std::vector<InlineSegment> rich_text = {
                {InlineSegment::Type::Text, BuildSupSubFallback(footnote_label, true) + ": ", true, false}};
            const std::vector<InlineSegment> definition_segments = parse_inline(footnote_text);
            rich_text.insert(rich_text.end(), definition_segments.begin(), definition_segments.end());
            blocks.push_back({MarkdownBlock::Type::BulletedListItem, std::move(rich_text), "", ""});
            i = cursor;
            continue;
        }
        if (IsMarkdownReferenceDefinitionLine(line))
        {
            ++i;
            continue;
        }

        if (const std::optional<std::pair<std::string, std::string>> image = ExtractStandaloneMarkdownImage(trimmed))
        {
            blocks.push_back({MarkdownBlock::Type::Image, {}, image->first, image->second});
            ++i;
            continue;
        }

        if (const std::optional<MarkdownAdmonitionStart> admonition = ParseMarkdownColonAdmonitionStart(line))
        {
            ++i;
            std::string body;
            while (i < lines.size() && !IsMarkdownColonAdmonitionEnd(lines[i], admonition->fence_len))
            {
                if (!body.empty())
                {
                    body += "\n";
                }
                body += lines[i];
                ++i;
            }
            if (i < lines.size())
            {
                ++i;
            }

            std::string callout_text = Trim(admonition->title);
            const std::string body_text = Trim(DedentBlockText(body));
            if (!body_text.empty())
            {
                if (!callout_text.empty())
                {
                    callout_text += "\n";
                }
                callout_text += body_text;
            }
            if (!callout_text.empty())
            {
                blocks.push_back({MarkdownBlock::Type::Callout,
                                  parse_inline(callout_text),
                                  admonition->style.emoji,
                                  admonition->style.color});
            }
            continue;
        }

        if (const std::optional<MarkdownAdmonitionStart> admonition = ParseMarkdownBangAdmonitionStart(line))
        {
            ++i;
            std::string body;
            while (i < lines.size())
            {
                const std::string &candidate = lines[i];
                if (!Trim(candidate).empty() && CountLeadingSpaces(candidate) < 4)
                {
                    break;
                }
                if (!body.empty())
                {
                    body += "\n";
                }
                body += StripMarkdownIndentedAdmonitionLine(candidate);
                ++i;
            }

            std::string callout_text = Trim(admonition->title);
            const std::string body_text = Trim(DedentBlockText(body));
            if (!body_text.empty())
            {
                if (!callout_text.empty())
                {
                    callout_text += "\n";
                }
                callout_text += body_text;
            }
            if (!callout_text.empty())
            {
                blocks.push_back({MarkdownBlock::Type::Callout,
                                  parse_inline(callout_text),
                                  admonition->style.emoji,
                                  admonition->style.color});
            }
            continue;
        }

        char fence_char = '\0';
        std::size_t fence_len = 0;
        if (StartsWithFence(line, &fence_char, &fence_len))
        {
            const std::string trimmed_left = TrimLeft(line);
            const std::string language = Trim(trimmed_left.substr(fence_len));
            ++i;
            std::string code;
            while (i < lines.size())
            {
                if (IsClosingFenceLine(lines[i], fence_char, fence_len))
                {
                    ++i;
                    break;
                }
                if (!code.empty())
                {
                    code += "\n";
                }
                code += lines[i];
                ++i;
            }
            blocks.push_back({IsPlainTableFenceLanguage(language) && IsMarkdownTableText(code)
                                  ? MarkdownBlock::Type::Table
                                  : MarkdownBlock::Type::Code,
                              {},
                              code,
                              language});
            continue;
        }

        if (const std::optional<std::string> single_line_equation = ExtractSingleLineBlockEquation(trimmed))
        {
            if (!single_line_equation->empty())
            {
                blocks.push_back({MarkdownBlock::Type::Equation, {}, *single_line_equation, ""});
                ++i;
                continue;
            }
        }

        if (IsBlockEquationFenceStart(trimmed))
        {
            const std::string opening = trimmed;
            const bool preserve_latex_environment = ExtractLatexBeginEnvironmentName(opening).has_value();
            ++i;
            std::string expression;
            while (i < lines.size() && !IsBlockEquationFenceEnd(Trim(lines[i]), opening))
            {
                if (!expression.empty())
                {
                    expression += "\n";
                }
                expression += lines[i];
                ++i;
            }
            std::string closing;
            if (i < lines.size())
            {
                closing = Trim(lines[i]);
                ++i;
            }
            std::string block_expression = DedentBlockText(expression);
            if (preserve_latex_environment && !closing.empty())
            {
                block_expression = opening + (block_expression.empty() ? "\n" : "\n" + block_expression + "\n") + closing;
            }
            blocks.push_back({MarkdownBlock::Type::Equation, {}, block_expression, ""});
            continue;
        }

        if (IsLooseBracketEquationFenceStart(trimmed))
        {
            std::size_t cursor = i + 1;
            std::string expression;
            while (cursor < lines.size() && !IsLooseBracketEquationFenceEnd(Trim(lines[cursor])))
            {
                if (!expression.empty())
                {
                    expression += "\n";
                }
                expression += lines[cursor];
                ++cursor;
            }
            if (cursor < lines.size() && LooksLikeBlockLatexExpression(expression))
            {
                blocks.push_back({MarkdownBlock::Type::Equation, {}, DedentBlockText(expression), ""});
                i = cursor + 1;
                continue;
            }
        }

        if (i + 1 < lines.size() && CanUseMarkdownDefinitionListTerm(line) &&
            IsMarkdownDefinitionListLine(lines[i + 1]))
        {
            const std::string term = Trim(line);
            std::size_t cursor = i + 1;
            while (cursor < lines.size())
            {
                const std::optional<std::string> definition = ExtractMarkdownDefinitionListText(lines[cursor]);
                if (!definition.has_value())
                {
                    break;
                }

                std::vector<InlineSegment> rich_text = parse_inline(term);
                for (InlineSegment &segment : rich_text)
                {
                    segment.bold = true;
                }
                if (!Trim(*definition).empty())
                {
                    rich_text.push_back({InlineSegment::Type::Text, ": ", false, false});
                    const std::vector<InlineSegment> definition_segments = parse_inline(*definition);
                    rich_text.insert(rich_text.end(), definition_segments.begin(), definition_segments.end());
                }
                blocks.push_back({MarkdownBlock::Type::BulletedListItem, std::move(rich_text), "", ""});
                ++cursor;
            }
            i = cursor;
            continue;
        }

        MarkdownBlock::Type setext_type = MarkdownBlock::Type::Paragraph;
        if (i + 1 < lines.size() && CanUseSetextHeadingText(line) &&
            IsSetextHeadingUnderline(Trim(lines[i + 1]), &setext_type))
        {
            blocks.push_back(
                {setext_type, parse_inline(StripMarkdownHeadingAttributeList(DedentBlockText(line))), "", ""});
            i += 2;
            continue;
        }

        if (IsDividerLine(trimmed))
        {
            blocks.push_back({MarkdownBlock::Type::Divider, {}, "", ""});
            ++i;
            continue;
        }

        if (IsHeadingLine(line))
        {
            const std::string trimmed_left = TrimLeft(line);
            std::size_t level = 0;
            while (level < trimmed_left.size() && trimmed_left[level] == '#')
            {
                ++level;
            }
            const std::string heading_text = StripMarkdownHeadingAttributeList(Trim(trimmed_left.substr(level)));
            const MarkdownBlock::Type type =
                level == 1 ? MarkdownBlock::Type::Heading1
                           : (level == 2 ? MarkdownBlock::Type::Heading2 : MarkdownBlock::Type::Heading3);
            blocks.push_back({type, parse_inline(heading_text), "", ""});
            ++i;
            continue;
        }

        bool task_checked = false;
        if (IsTaskListLine(line, &task_checked))
        {
            const std::string trimmed_left = TrimLeft(line);
            blocks.push_back({MarkdownBlock::Type::ToDo,
                              parse_inline(Trim(trimmed_left.substr(6))),
                              "",
                              "",
                              task_checked});
            ++i;
            continue;
        }

        if (IsBulletListLine(line))
        {
            const std::string trimmed_left = TrimLeft(line);
            blocks.push_back(
                {MarkdownBlock::Type::BulletedListItem, parse_inline(Trim(trimmed_left.substr(1))), "", ""});
            ++i;
            continue;
        }

        if (IsNumberedListLine(line))
        {
            const std::string trimmed_left = TrimLeft(line);
            const std::size_t content_start = NumberedListContentStart(trimmed_left).value_or(0);
            blocks.push_back({MarkdownBlock::Type::NumberedListItem,
                              parse_inline(Trim(trimmed_left.substr(content_start))),
                              "",
                              ""});
            ++i;
            continue;
        }

        if (IsQuoteLine(line))
        {
            std::string quote = Trim(TrimLeft(line).substr(1));
            ++i;
            while (i < lines.size() && IsQuoteLine(lines[i]))
            {
                quote += "\n";
                quote += Trim(TrimLeft(lines[i]).substr(1));
                ++i;
            }
            std::string callout_text;
            if (const std::optional<MarkdownAlertStyle> style = ExtractMarkdownAlertFromQuote(quote, &callout_text);
                style.has_value() && !Trim(callout_text).empty())
            {
                blocks.push_back(
                    {MarkdownBlock::Type::Callout, parse_inline(callout_text), style->emoji, style->color});
                continue;
            }
            blocks.push_back({MarkdownBlock::Type::Quote, parse_inline(DedentBlockText(quote)), "", ""});
            continue;
        }

        if (IsMarkdownTableStart(lines, i))
        {
            const std::size_t expected_columns = *MarkdownTableColumnCount(line);
            std::string table = line;
            ++i;
            while (i < lines.size())
            {
                // 跨越表格行之间的空行：先探测下一非空行是否仍是同宽表格行。
                std::size_t probe = i;
                while (probe < lines.size() && Trim(lines[probe]).empty())
                {
                    ++probe;
                }
                if (probe >= lines.size())
                {
                    break;
                }
                const std::optional<std::size_t> columns = MarkdownTableColumnCount(lines[probe]);
                if (!columns.has_value() || *columns != expected_columns)
                {
                    break;
                }
                table += "\n";
                table += lines[probe];
                i = probe + 1;
            }
            blocks.push_back({MarkdownBlock::Type::Table, {}, table, ""});
            continue;
        }

        std::string paragraph = line;
        ++i;
        while (i < lines.size() && !IsParagraphBoundary(lines[i]))
        {
            paragraph += "\n";
            paragraph += lines[i];
            ++i;
        }
        blocks.push_back({MarkdownBlock::Type::Paragraph, parse_inline(DedentBlockText(paragraph)), "", ""});
    }
    return blocks;
}

std::string BuildTextRichText(const std::string &text, bool bold, bool code, bool strikethrough,
                              const std::string &link_url, bool italic, bool underline, const std::string &color)
{
    const std::string safe_color = color.empty() ? "default" : color;
    return "{\"type\":\"text\",\"text\":{\"content\":\"" + EscapeJson(text) +
           "\",\"link\":" + (link_url.empty() ? "null" : "{\"url\":\"" + EscapeJson(link_url) + "\"}") +
           "},\"annotations\":{\"bold\":" + (bold ? "true" : "false") +
           ",\"italic\":" + (italic ? "true" : "false") +
           ",\"strikethrough\":" + (strikethrough ? "true" : "false") +
           ",\"underline\":" + (underline ? "true" : "false") + ",\"code\":" +
           (code ? "true" : "false") + ",\"color\":\"" + EscapeJson(safe_color) + "\"}}";
}

std::vector<InlineSegment> NormalizeRichTextSegmentsForNotion(const std::vector<InlineSegment> &segments)
{
    constexpr std::size_t kTextContentLimit = 1800;
    constexpr std::size_t kEquationExpressionLimit = 1000;

    std::vector<InlineSegment> normalized;
    normalized.reserve(segments.size());
    for (const InlineSegment &segment : segments)
    {
        if (segment.content.empty())
        {
            continue;
        }

        if (segment.type == InlineSegment::Type::Equation)
        {
            const std::string repaired = RepairLatexExpression(segment.content);
            if (repaired.empty())
            {
                continue;
            }
            if (repaired.size() <= kEquationExpressionLimit)
            {
                normalized.push_back({InlineSegment::Type::Equation, repaired, false, false});
                continue;
            }

            const std::string fallback = "$" + CollapseWhitespace(segment.content) + "$";
            for (const std::string &chunk : SplitUtf8ByCharLimit(fallback, kTextContentLimit))
            {
                normalized.push_back({InlineSegment::Type::Text, chunk, false, false, false});
            }
            continue;
        }

        for (const std::string &chunk : SplitUtf8ByCharLimit(segment.content, kTextContentLimit))
        {
            if (!chunk.empty())
            {
                normalized.push_back(
                    {InlineSegment::Type::Text, chunk, segment.bold, segment.code, segment.strikethrough,
                     segment.underline, segment.link_url, segment.italic, segment.color});
            }
        }
    }
    return normalized;
}

std::string BuildRichTextJson(const std::vector<InlineSegment> &segments)
{
    const std::vector<InlineSegment> normalized = NormalizeRichTextSegmentsForNotion(segments);
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const InlineSegment &segment : normalized)
    {
        if (segment.content.empty())
        {
            continue;
        }
        if (segment.type == InlineSegment::Type::Equation)
        {
            if (!first)
            {
                oss << ",";
            }
            oss << "{\"type\":\"equation\",\"equation\":{\"expression\":\"" << EscapeJson(segment.content)
                << "\"},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,"
                   "\"underline\":false,\"code\":false,\"color\":\"default\"}}";
            first = false;
            continue;
        }

        if (!first)
        {
            oss << ",";
        }
        oss << BuildTextRichText(segment.content, segment.bold, segment.code, segment.strikethrough, segment.link_url,
                                 segment.italic, segment.underline, segment.color);
        first = false;
    }
    oss << "]";
    return oss.str();
}

std::vector<std::vector<InlineSegment>> SplitRichTextSegmentsForBlocks(const std::vector<InlineSegment> &segments)
{
    constexpr std::size_t kMaxRichTextObjectsPerBlock = 90;
    const std::vector<InlineSegment> normalized = NormalizeRichTextSegmentsForNotion(segments);
    std::vector<std::vector<InlineSegment>> groups;
    std::vector<InlineSegment> current;
    current.reserve(kMaxRichTextObjectsPerBlock);

    for (const InlineSegment &segment : normalized)
    {
        if (current.size() >= kMaxRichTextObjectsPerBlock)
        {
            groups.push_back(std::move(current));
            current.clear();
            current.reserve(kMaxRichTextObjectsPerBlock);
        }
        current.push_back(segment);
    }
    if (!current.empty())
    {
        groups.push_back(std::move(current));
    }
    return groups;
}

std::string BuildRichTextBlock(const std::string &type, const std::vector<InlineSegment> &rich_text)
{
    return "{\"object\":\"block\",\"type\":\"" + type + "\",\"" + type + "\":{\"rich_text\":" +
           BuildRichTextJson(rich_text) + "}}";
}

std::string BuildCalloutBlock(const std::vector<InlineSegment> &rich_text, const std::string &emoji,
                              const std::string &color)
{
    return "{\"object\":\"block\",\"type\":\"callout\",\"callout\":{\"rich_text\":" + BuildRichTextJson(rich_text) +
           ",\"icon\":{\"type\":\"emoji\",\"emoji\":\"" + EscapeJson(emoji) + "\"},\"color\":\"" +
           EscapeJson(color) + "\"}}";
}

void AppendRichTextBlocks(std::vector<std::string> *blocks, const std::string &type,
                          const std::vector<InlineSegment> &rich_text)
{
    for (const std::vector<InlineSegment> &group : SplitRichTextSegmentsForBlocks(rich_text))
    {
        blocks->push_back(BuildRichTextBlock(type, group));
    }
}

void AppendCalloutBlocks(std::vector<std::string> *blocks, const std::vector<InlineSegment> &rich_text,
                         const std::string &emoji, const std::string &color)
{
    for (const std::vector<InlineSegment> &group : SplitRichTextSegmentsForBlocks(rich_text))
    {
        blocks->push_back(BuildCalloutBlock(group, emoji, color));
    }
}

std::string BuildToDoBlock(const std::vector<InlineSegment> &rich_text, bool checked)
{
    return "{\"object\":\"block\",\"type\":\"to_do\",\"to_do\":{\"rich_text\":" + BuildRichTextJson(rich_text) +
           ",\"checked\":" + (checked ? "true" : "false") + "}}";
}

void AppendToDoBlocks(std::vector<std::string> *blocks, const std::vector<InlineSegment> &rich_text, bool checked)
{
    for (const std::vector<InlineSegment> &group : SplitRichTextSegmentsForBlocks(rich_text))
    {
        blocks->push_back(BuildToDoBlock(group, checked));
    }
}

std::string BuildEquationBlock(const std::string &expression)
{
    return "{\"object\":\"block\",\"type\":\"equation\",\"equation\":{\"expression\":\"" +
           EscapeJson(RepairLatexExpression(expression)) + "\"}}";
}

std::string BuildDividerBlock()
{
    return "{\"object\":\"block\",\"type\":\"divider\",\"divider\":{}}";
}

std::string BuildPlainTextRichTextJson(const std::string &text)
{
    constexpr std::size_t kTextContentLimit = 1800;
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    for (const std::string &chunk : SplitUtf8ByCharLimit(text, kTextContentLimit))
    {
        if (chunk.empty())
        {
            continue;
        }
        if (!first)
        {
            oss << ",";
        }
        oss << "{\"type\":\"text\",\"text\":{\"content\":\"" << EscapeJson(chunk) << "\"}}";
        first = false;
    }
    oss << "]";
    return oss.str();
}

std::string BuildImageBlock(const std::string &url, const std::string &caption)
{
    std::string caption_json = "[]";
    if (!Trim(caption).empty())
    {
        caption_json = BuildRichTextJson(ParseInlineMarkdown(caption));
    }
    return "{\"object\":\"block\",\"type\":\"image\",\"image\":{\"type\":\"external\",\"external\":{\"url\":\"" +
           EscapeJson(url) + "\"},\"caption\":" + caption_json + "}}";
}

std::string BuildCodeBlock(const std::string &code, const std::string &language)
{
    std::string safe_language = ToLowerAscii(Trim(language));
    if (safe_language.empty() || safe_language.size() > 40 || safe_language == "text" || safe_language == "txt" ||
        safe_language == "plain" || safe_language == "plaintext")
    {
        safe_language = "plain text";
    }
    if (safe_language == "cpp" || safe_language == "cxx")
    {
        safe_language = "c++";
    }
    else if (safe_language == "js")
    {
        safe_language = "javascript";
    }
    else if (safe_language == "py")
    {
        safe_language = "python";
    }
    else if (safe_language == "sh" || safe_language == "shell")
    {
        safe_language = "bash";
    }
    else if (safe_language == "md")
    {
        safe_language = "markdown";
    }
    else if (safe_language == "mysql" || safe_language == "postgres" || safe_language == "postgresql" ||
             safe_language == "psql" || safe_language == "sqlite" || safe_language == "sqlite3" ||
             safe_language == "tsql" || safe_language == "plsql")
    {
        safe_language = "sql";
    }

    static const std::vector<std::string> allowed_languages = {
        "abap",       "abc",      "agda",       "arduino",  "ascii art", "assembly", "bash",       "basic",
        "bnf",        "c",        "c#",         "c++",      "clojure",   "coffeescript",
        "coq",        "css",      "dart",       "dhall",    "diff",      "docker",   "ebnf",       "elixir",
        "elm",        "erlang",   "f#",         "flow",     "fortran",   "gherkin",  "glsl",       "go",
        "graphql",    "groovy",   "haskell",    "hcl",      "html",      "idris",    "java",       "javascript",
        "json",       "julia",    "kotlin",     "latex",    "less",      "lisp",     "livescript", "llvm ir",
        "lua",        "makefile", "markdown",   "markup",   "matlab",    "mathematica",
        "mermaid",    "nix",      "objective-c","ocaml",    "pascal",    "perl",     "php",        "plain text",
        "powershell", "prolog",   "protobuf",   "purescript","python",   "r",        "racket",     "reason",
        "ruby",       "rust",     "sass",       "scala",    "scheme",    "scss",     "shell",      "smalltalk",
        "solidity",   "sql",      "swift",      "toml",     "typescript","vb.net",   "verilog",    "vhdl",
        "visual basic","webassembly","xml",      "yaml",     "java/c/c++/c#",
    };
    if (std::find(allowed_languages.begin(), allowed_languages.end(), safe_language) == allowed_languages.end())
    {
        safe_language = "plain text";
    }

    return "{\"object\":\"block\",\"type\":\"code\",\"code\":{\"rich_text\":" + BuildPlainTextRichTextJson(code) +
           ",\"language\":\"" + EscapeJson(safe_language) + "\"}}";
}

void AppendCodeBlocks(std::vector<std::string> *blocks, const std::string &code, const std::string &language)
{
    const std::vector<InlineSegment> code_text = {{InlineSegment::Type::Text, code, false, false}};
    for (const std::vector<InlineSegment> &group : SplitRichTextSegmentsForBlocks(code_text))
    {
        std::string merged;
        for (const InlineSegment &segment : group)
        {
            merged += segment.content;
        }
        blocks->push_back(BuildCodeBlock(merged, language));
    }
}

void AppendEquationBlocks(std::vector<std::string> *blocks, const std::string &expression)
{
    constexpr std::size_t kEquationExpressionLimit = 1000;
    const std::string repaired = RepairLatexExpression(expression);
    if (repaired.empty())
    {
        return;
    }
    if (repaired.size() <= kEquationExpressionLimit)
    {
        blocks->push_back(BuildEquationBlock(repaired));
        return;
    }
    AppendCodeBlocks(blocks, repaired, "latex");
}

bool BuildTableCellJson(const std::string &cell, const LinkReferenceMap *references, std::string *json)
{
    const std::vector<InlineSegment> normalized =
        NormalizeRichTextSegmentsForNotion(ParseInlineMarkdown(cell, references));
    if (normalized.size() > 90)
    {
        return false;
    }
    *json = BuildRichTextJson(normalized);
    return true;
}

bool BuildTableRowJson(const std::vector<std::string> &row, std::size_t width, const LinkReferenceMap *references,
                       std::string *json)
{
    std::ostringstream oss;
    oss << "{\"object\":\"block\",\"type\":\"table_row\",\"table_row\":{\"cells\":[";
    for (std::size_t column = 0; column < width; ++column)
    {
        if (column > 0)
        {
            oss << ",";
        }
        std::string cell_json;
        if (!BuildTableCellJson(column < row.size() ? row[column] : "", references, &cell_json))
        {
            return false;
        }
        oss << cell_json;
    }
    oss << "]}}";
    *json = oss.str();
    return true;
}

bool BuildTableBlock(const std::vector<std::vector<std::string>> &rows, std::size_t begin, std::size_t end,
                     std::size_t width, bool has_column_header, const LinkReferenceMap *references, std::string *json)
{
    if (begin >= end || width == 0)
    {
        return false;
    }

    std::ostringstream oss;
    oss << "{\"object\":\"block\",\"type\":\"table\",\"table\":{\"table_width\":" << width
        << ",\"has_column_header\":" << (has_column_header ? "true" : "false")
        << ",\"has_row_header\":false,\"children\":[";
    for (std::size_t row = begin; row < end; ++row)
    {
        if (row > begin)
        {
            oss << ",";
        }
        std::string row_json;
        if (!BuildTableRowJson(rows[row], width, references, &row_json))
        {
            return false;
        }
        oss << row_json;
    }
    oss << "]}}";
    *json = oss.str();
    return true;
}

void AppendTableBlocks(std::vector<std::string> *blocks, const std::string &table_text,
                       const LinkReferenceMap *references)
{
    const std::vector<std::string> lines = SplitLinesPreserveEmpty(NormalizeLineEndings(table_text));
    std::vector<std::vector<std::string>> rows;
    std::size_t width = 0;
    for (const std::string &line : lines)
    {
        if (IsMarkdownTableSeparatorLine(line))
        {
            continue;
        }
        std::vector<std::string> cells = SplitMarkdownTableCells(line);
        if (cells.empty())
        {
            continue;
        }
        if (width == 0)
        {
            width = cells.size();
        }
        if (cells.size() < width)
        {
            cells.resize(width);
        }
        else if (cells.size() > width)
        {
            cells.resize(width);
        }
        rows.push_back(std::move(cells));
    }

    if (rows.empty() || width == 0)
    {
        AppendCodeBlocks(blocks, table_text, "plain text");
        return;
    }

    const bool has_column_header = rows.size() >= 2;
    const std::size_t max_rows_per_block = std::max<std::size_t>(1, 90 / width);
    std::size_t begin = 0;
    while (begin < rows.size())
    {
        const std::size_t end = std::min(rows.size(), begin + max_rows_per_block);
        std::string table_json;
        if (!BuildTableBlock(rows, begin, end, width, has_column_header && begin == 0, references, &table_json))
        {
            AppendCodeBlocks(blocks, table_text, "plain text");
            return;
        }
        blocks->push_back(std::move(table_json));
        begin = end;
    }
}

std::vector<std::string> BuildTextBlocks(const std::string &content)
{
    const LinkReferenceMap link_references =
        CollectMarkdownLinkReferences(SplitLinesPreserveEmpty(StripUtf8Bom(NormalizeLineEndings(content))));
    const std::vector<MarkdownBlock> markdown_blocks = ParseMarkdownBlocks(content);
    std::vector<std::string> blocks;
    blocks.reserve(markdown_blocks.size());
    for (const MarkdownBlock &block : markdown_blocks)
    {
        switch (block.type)
        {
        case MarkdownBlock::Type::Paragraph:
            AppendRichTextBlocks(&blocks, "paragraph", block.rich_text);
            break;
        case MarkdownBlock::Type::Heading1:
            AppendRichTextBlocks(&blocks, "heading_1", block.rich_text);
            break;
        case MarkdownBlock::Type::Heading2:
            AppendRichTextBlocks(&blocks, "heading_2", block.rich_text);
            break;
        case MarkdownBlock::Type::Heading3:
            AppendRichTextBlocks(&blocks, "heading_3", block.rich_text);
            break;
        case MarkdownBlock::Type::BulletedListItem:
            AppendRichTextBlocks(&blocks, "bulleted_list_item", block.rich_text);
            break;
        case MarkdownBlock::Type::NumberedListItem:
            AppendRichTextBlocks(&blocks, "numbered_list_item", block.rich_text);
            break;
        case MarkdownBlock::Type::Quote:
            AppendRichTextBlocks(&blocks, "quote", block.rich_text);
            break;
        case MarkdownBlock::Type::Callout:
            AppendCalloutBlocks(&blocks, block.rich_text, block.text, block.language);
            break;
        case MarkdownBlock::Type::ToDo:
            AppendToDoBlocks(&blocks, block.rich_text, block.checked);
            break;
        case MarkdownBlock::Type::Divider:
            blocks.push_back(BuildDividerBlock());
            break;
        case MarkdownBlock::Type::Equation:
            AppendEquationBlocks(&blocks, block.text);
            break;
        case MarkdownBlock::Type::Table:
            AppendTableBlocks(&blocks, block.text, &link_references);
            break;
        case MarkdownBlock::Type::Image:
            if (IsSupportedExternalImageUrl(block.text))
            {
                blocks.push_back(BuildImageBlock(block.text, block.language));
            }
            break;
        case MarkdownBlock::Type::Code:
            AppendCodeBlocks(&blocks, block.text, block.language);
            break;
        }
    }
    if (blocks.empty() && !Trim(content).empty())
    {
        AppendRichTextBlocks(&blocks, "paragraph", ParseInlineMarkdown(content));
    }
    return blocks;
}

std::size_t EstimateAppendChildrenBodyBytes(const std::vector<std::string> &blocks, std::size_t begin, std::size_t end)
{
    std::size_t bytes = std::string("{\"children\":[]}").size();
    for (std::size_t i = begin; i < end; ++i)
    {
        bytes += blocks[i].size();
        if (i != begin)
        {
            bytes += 1;
        }
    }
    return bytes;
}

std::size_t SelectAppendBatchEnd(const std::vector<std::string> &blocks, std::size_t begin, std::size_t max_blocks,
                                 std::size_t max_body_bytes)
{
    if (begin >= blocks.size())
    {
        return begin;
    }

    const std::size_t block_limit = std::max<std::size_t>(1, max_blocks);
    std::size_t end = begin;
    std::size_t bytes = std::string("{\"children\":[]}").size();
    while (end < blocks.size() && end - begin < block_limit)
    {
        const std::size_t next_bytes = bytes + blocks[end].size() + (end == begin ? 0 : 1);
        if (end > begin && next_bytes > max_body_bytes)
        {
            break;
        }
        bytes = next_bytes;
        ++end;
    }
    return std::max(begin + 1, end);
}

void AppendUtf8CodePoint(std::string *output, unsigned int code_point)
{
    if (code_point <= 0x7f)
    {
        output->push_back(static_cast<char>(code_point));
    }
    else if (code_point <= 0x7ff)
    {
        output->push_back(static_cast<char>(0xc0 | (code_point >> 6)));
        output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    else if (code_point <= 0xffff)
    {
        output->push_back(static_cast<char>(0xe0 | (code_point >> 12)));
        output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
    else if (code_point <= 0x10ffff)
    {
        output->push_back(static_cast<char>(0xf0 | (code_point >> 18)));
        output->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3f)));
        output->push_back(static_cast<char>(0x80 | (code_point & 0x3f)));
    }
}

std::string DecodeHtmlEntities(const std::string &text)
{
    std::string output;
    output.reserve(text.size());
    for (std::size_t i = 0; i < text.size();)
    {
        if (text[i] != '&')
        {
            output.push_back(text[i++]);
            continue;
        }

        const std::size_t semi = text.find(';', i + 1);
        if (semi == std::string::npos || semi - i > 16)
        {
            output.push_back(text[i++]);
            continue;
        }

        const std::string entity = ToLowerAscii(text.substr(i + 1, semi - i - 1));
        if (entity == "amp")
        {
            output.push_back('&');
        }
        else if (entity == "lt")
        {
            output.push_back('<');
        }
        else if (entity == "gt")
        {
            output.push_back('>');
        }
        else if (entity == "quot")
        {
            output.push_back('"');
        }
        else if (entity == "apos" || entity == "#39")
        {
            output.push_back('\'');
        }
        else if (entity == "nbsp")
        {
            output.push_back(' ');
        }
        else if (entity.size() > 1 && entity[0] == '#')
        {
            try
            {
                const bool hex = entity.size() > 2 && entity[1] == 'x';
                const std::string number = hex ? entity.substr(2) : entity.substr(1);
                const unsigned int code_point = static_cast<unsigned int>(std::stoul(number, nullptr, hex ? 16 : 10));
                if (code_point > 0 && code_point <= 0x10ffff)
                {
                    AppendUtf8CodePoint(&output, code_point);
                }
                else
                {
                    output += text.substr(i, semi - i + 1);
                }
            }
            catch (...)
            {
                output += text.substr(i, semi - i + 1);
            }
        }
        else
        {
            output += text.substr(i, semi - i + 1);
        }
        i = semi + 1;
    }
    return output;
}

std::string CompactMarkdownNewlines(const std::string &text)
{
    std::string output;
    output.reserve(text.size());
    int newlines = 0;
    for (char ch : NormalizeLineEndings(text))
    {
        if (ch == '\n')
        {
            if (newlines < 2)
            {
                output.push_back(ch);
            }
            ++newlines;
            continue;
        }
        output.push_back(ch);
        newlines = 0;
    }
    return Trim(output);
}

bool ContainsNonWhitespace(const std::string &text)
{
    return std::any_of(text.begin(), text.end(), [](unsigned char ch)
                       { return std::isspace(ch) == 0; });
}

bool IsMarkdownFenceLine(const std::string &line)
{
    const std::string trimmed = Trim(line);
    if (trimmed.size() < 3 || (trimmed[0] != '`' && trimmed[0] != '~'))
    {
        return false;
    }
    const char ch = trimmed[0];
    std::size_t count = 0;
    while (count < trimmed.size() && trimmed[count] == ch)
    {
        ++count;
    }
    return count >= 3 && Trim(trimmed.substr(count)).empty();
}

std::string RemoveEmptyMarkdownCodeFences(const std::string &text)
{
    const std::vector<std::string> lines = SplitLinesPreserveEmpty(NormalizeLineEndings(text));
    std::vector<std::string> kept;
    kept.reserve(lines.size());

    for (std::size_t i = 0; i < lines.size();)
    {
        if (!IsMarkdownFenceLine(lines[i]))
        {
            kept.push_back(lines[i++]);
            continue;
        }

        std::size_t cursor = i + 1;
        while (cursor < lines.size() && Trim(lines[cursor]).empty())
        {
            ++cursor;
        }
        if (cursor < lines.size() && IsMarkdownFenceLine(lines[cursor]))
        {
            i = cursor + 1;
            continue;
        }

        kept.push_back(lines[i++]);
    }

    std::ostringstream oss;
    for (std::size_t i = 0; i < kept.size(); ++i)
    {
        if (i != 0)
        {
            oss << "\n";
        }
        oss << kept[i];
    }
    return CompactMarkdownNewlines(oss.str());
}

bool HasEmptyMarkdownCodeFenceArtifact(const std::string &text)
{
    const std::vector<std::string> lines = SplitLinesPreserveEmpty(NormalizeLineEndings(text));
    for (std::size_t i = 0; i + 1 < lines.size(); ++i)
    {
        if (!IsMarkdownFenceLine(lines[i]))
        {
            continue;
        }
        std::size_t cursor = i + 1;
        while (cursor < lines.size() && Trim(lines[cursor]).empty())
        {
            ++cursor;
        }
        if (cursor < lines.size() && IsMarkdownFenceLine(lines[cursor]))
        {
            return true;
        }
    }
    return false;
}

std::string DecodeHtmlTextWithoutTags(const std::string &html)
{
    std::string output;
    output.reserve(html.size());
    for (std::size_t i = 0; i < html.size();)
    {
        if (html[i] != '<')
        {
            const std::size_t next = html.find('<', i);
            output += DecodeHtmlEntities(html.substr(i, next == std::string::npos ? std::string::npos : next - i));
            if (next == std::string::npos)
            {
                break;
            }
            i = next;
            continue;
        }
        const std::size_t end = html.find('>', i + 1);
        if (end == std::string::npos)
        {
            break;
        }
        i = end + 1;
    }
    return output;
}

std::string HtmlFragmentToMarkdown(std::string html);
std::optional<std::pair<std::size_t, std::size_t>> FindMatchingHtmlEnd(const std::string &lower_html,
                                                                       std::size_t tag_start,
                                                                       std::size_t tag_end,
                                                                       const std::string &name);

std::string BuildMarkdownInlineCode(std::string code)
{
    code = DecodeHtmlTextWithoutTags(std::move(code));
    std::size_t max_run = 0;
    for (std::size_t i = 0; i < code.size();)
    {
        if (code[i] != '`')
        {
            ++i;
            continue;
        }
        const std::size_t run = CountRepeatedChar(code, i, '`');
        max_run = std::max(max_run, run);
        i += run;
    }
    const std::string fence(max_run + 1, '`');
    return fence + code + fence;
}

std::string BuildMarkdownCodeFence(std::string code, const std::string &language)
{
    code = NormalizeLineEndings(DecodeHtmlTextWithoutTags(std::move(code)));
    code = Trim(code);
    std::size_t max_run = 0;
    for (std::size_t i = 0; i < code.size();)
    {
        if (code[i] != '`')
        {
            ++i;
            continue;
        }
        const std::size_t run = CountRepeatedChar(code, i, '`');
        max_run = std::max(max_run, run);
        i += run;
    }

    const std::string fence(std::max<std::size_t>(3, max_run + 1), '`');
    const std::string trimmed_language = Trim(language);
    return fence + trimmed_language + "\n" + code + "\n" + fence;
}

std::optional<std::string> ExtractHtmlAttribute(const std::string &raw_tag, const std::string &attribute_name)
{
    std::size_t pos = 0;
    while (pos < raw_tag.size())
    {
        while (pos < raw_tag.size() &&
               (std::isspace(static_cast<unsigned char>(raw_tag[pos])) || raw_tag[pos] == '/'))
        {
            ++pos;
        }
        const std::size_t name_begin = pos;
        while (pos < raw_tag.size() && (std::isalnum(static_cast<unsigned char>(raw_tag[pos])) ||
                                        raw_tag[pos] == '-' || raw_tag[pos] == '_' || raw_tag[pos] == ':'))
        {
            ++pos;
        }
        if (name_begin == pos)
        {
            ++pos;
            continue;
        }
        const std::string name = ToLowerAscii(raw_tag.substr(name_begin, pos - name_begin));
        while (pos < raw_tag.size() && std::isspace(static_cast<unsigned char>(raw_tag[pos])))
        {
            ++pos;
        }
        if (pos >= raw_tag.size() || raw_tag[pos] != '=')
        {
            continue;
        }
        ++pos;
        while (pos < raw_tag.size() && std::isspace(static_cast<unsigned char>(raw_tag[pos])))
        {
            ++pos;
        }
        if (pos >= raw_tag.size())
        {
            break;
        }

        std::string value;
        if (raw_tag[pos] == '"' || raw_tag[pos] == '\'')
        {
            const char quote = raw_tag[pos++];
            const std::size_t value_begin = pos;
            while (pos < raw_tag.size() && raw_tag[pos] != quote)
            {
                ++pos;
            }
            value = raw_tag.substr(value_begin, pos - value_begin);
            if (pos < raw_tag.size())
            {
                ++pos;
            }
        }
        else
        {
            const std::size_t value_begin = pos;
            while (pos < raw_tag.size() && !std::isspace(static_cast<unsigned char>(raw_tag[pos])) &&
                   raw_tag[pos] != '>')
            {
                ++pos;
            }
            value = raw_tag.substr(value_begin, pos - value_begin);
        }

        if (name == attribute_name)
        {
            return DecodeHtmlEntities(value);
        }
    }
    return std::nullopt;
}

bool HasHtmlAttribute(const std::string &raw_tag, const std::string &attribute_name)
{
    std::size_t pos = 0;
    while (pos < raw_tag.size())
    {
        while (pos < raw_tag.size() &&
               (std::isspace(static_cast<unsigned char>(raw_tag[pos])) || raw_tag[pos] == '/'))
        {
            ++pos;
        }
        const std::size_t name_begin = pos;
        while (pos < raw_tag.size() && (std::isalnum(static_cast<unsigned char>(raw_tag[pos])) ||
                                        raw_tag[pos] == '-' || raw_tag[pos] == '_' || raw_tag[pos] == ':'))
        {
            ++pos;
        }
        if (name_begin == pos)
        {
            ++pos;
            continue;
        }
        const std::string name = ToLowerAscii(raw_tag.substr(name_begin, pos - name_begin));
        if (name == attribute_name)
        {
            return true;
        }
        while (pos < raw_tag.size() && std::isspace(static_cast<unsigned char>(raw_tag[pos])))
        {
            ++pos;
        }
        if (pos < raw_tag.size() && raw_tag[pos] == '=')
        {
            ++pos;
            while (pos < raw_tag.size() && std::isspace(static_cast<unsigned char>(raw_tag[pos])))
            {
                ++pos;
            }
            if (pos < raw_tag.size() && (raw_tag[pos] == '"' || raw_tag[pos] == '\''))
            {
                const char quote = raw_tag[pos++];
                while (pos < raw_tag.size() && raw_tag[pos] != quote)
                {
                    ++pos;
                }
                if (pos < raw_tag.size())
                {
                    ++pos;
                }
            }
            else
            {
                while (pos < raw_tag.size() && !std::isspace(static_cast<unsigned char>(raw_tag[pos])) &&
                       raw_tag[pos] != '>')
                {
                    ++pos;
                }
            }
        }
    }
    return false;
}

bool IsHtmlCheckboxInput(const std::string &raw_tag, bool *checked)
{
    const std::optional<std::string> type = ExtractHtmlAttribute(raw_tag, "type");
    if (!type.has_value() || ToLowerAscii(Trim(*type)) != "checkbox")
    {
        return false;
    }
    if (checked != nullptr)
    {
        *checked = HasHtmlAttribute(raw_tag, "checked");
    }
    return true;
}

bool IsHtmlHeadingTag(const std::string &name)
{
    return name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6';
}

bool IsHtmlInlineCodeTag(const std::string &name)
{
    return name == "code" || name == "kbd" || name == "samp" || name == "tt";
}

std::string MarkdownHeadingPrefixFromHtmlName(const std::string &name)
{
    if (name == "h1")
    {
        return "# ";
    }
    if (name == "h2")
    {
        return "## ";
    }
    return "### ";
}

std::string EscapeMarkdownLinkLabel(const std::string &label)
{
    std::string output;
    output.reserve(label.size());
    for (char ch : label)
    {
        if (ch == '\\' || ch == '[' || ch == ']')
        {
            output.push_back('\\');
        }
        output.push_back(ch);
    }
    return output;
}

std::optional<std::string> BuildMarkdownLinkFromHtmlAnchor(const std::string &raw_tag, const std::string &inner_html)
{
    const std::optional<std::string> href = ExtractHtmlAttribute(raw_tag, "href");
    if (!href.has_value() || !IsSupportedMarkdownLinkUrl(*href) || href->find_first_of("<>\r\n") != std::string::npos)
    {
        return std::nullopt;
    }

    const std::string label = CollapseWhitespace(HtmlFragmentToMarkdown(inner_html));
    if (label.empty())
    {
        return std::nullopt;
    }
    return "[" + EscapeMarkdownLinkLabel(label) + "](<" + *href + ">)";
}

std::optional<std::string> BuildMarkdownImageFromHtmlImg(const std::string &raw_tag)
{
    const std::optional<std::string> src = ExtractHtmlAttribute(raw_tag, "src");
    if (!src.has_value() || !IsSupportedExternalImageUrl(*src))
    {
        return std::nullopt;
    }
    const std::string alt = ExtractHtmlAttribute(raw_tag, "alt").value_or("");
    return "![" + EscapeMarkdownLinkLabel(CollapseWhitespace(alt)) + "](<" + *src + ">)";
}

std::optional<std::string> BuildMarkdownFigureFromHtml(const std::string &figure_html)
{
    const std::string lower_figure = ToLowerAscii(figure_html);
    const std::size_t img_start = lower_figure.find("<img");
    if (img_start == std::string::npos)
    {
        return std::nullopt;
    }
    const std::size_t img_end = lower_figure.find('>', img_start + 1);
    if (img_end == std::string::npos)
    {
        return std::nullopt;
    }

    const std::string img_tag = Trim(figure_html.substr(img_start + 1, img_end - img_start - 1));
    const std::optional<std::string> src = ExtractHtmlAttribute(img_tag, "src");
    if (!src.has_value() || !IsSupportedExternalImageUrl(*src))
    {
        return std::nullopt;
    }

    std::string caption;
    const std::size_t caption_start = lower_figure.find("<figcaption");
    if (caption_start != std::string::npos)
    {
        const std::size_t caption_tag_end = lower_figure.find('>', caption_start + 1);
        if (caption_tag_end != std::string::npos)
        {
            const auto match = FindMatchingHtmlEnd(lower_figure, caption_start, caption_tag_end, "figcaption");
            if (match.has_value())
            {
                caption = CollapseWhitespace(HtmlFragmentToMarkdown(
                    figure_html.substr(caption_tag_end + 1, match->first - caption_tag_end - 1)));
            }
        }
    }
    if (caption.empty())
    {
        caption = CollapseWhitespace(ExtractHtmlAttribute(img_tag, "alt").value_or(""));
    }
    return "![" + EscapeMarkdownLinkLabel(caption) + "](<" + *src + ">)";
}

std::optional<std::string> ExtractLanguageFromHtmlClass(std::string class_attr)
{
    class_attr = ToLowerAscii(DecodeHtmlEntities(std::move(class_attr)));
    ReplaceAllInPlace(&class_attr, ";", " ");
    std::istringstream iss(class_attr);
    std::string token;
    std::string previous;
    while (iss >> token)
    {
        if (token.rfind("language-", 0) == 0 && token.size() > 9)
        {
            return token.substr(9);
        }
        if (token.rfind("lang-", 0) == 0 && token.size() > 5)
        {
            return token.substr(5);
        }
        if (previous == "brush:" && !token.empty())
        {
            return token;
        }
        previous = token;
    }
    return std::nullopt;
}

bool HtmlClassHasMathToken(std::string class_attr)
{
    class_attr = ToLowerAscii(DecodeHtmlEntities(std::move(class_attr)));
    ReplaceAllInPlace(&class_attr, ";", " ");
    std::istringstream iss(class_attr);
    std::string token;
    while (iss >> token)
    {
        if (token == "math" || token == "tex" || token == "latex" || token == "katex" || token == "mathjax" ||
            token == "math-inline" || token == "math-display" || token == "mathjax_display")
        {
            return true;
        }
        if (token.rfind("katex-", 0) == 0 || token.rfind("mathjax-", 0) == 0)
        {
            return true;
        }
    }
    return false;
}

bool HtmlTagHasMathClass(const std::string &raw_tag)
{
    const std::optional<std::string> class_attr = ExtractHtmlAttribute(raw_tag, "class");
    return class_attr.has_value() && HtmlClassHasMathToken(*class_attr);
}

std::optional<std::string> ExtractHtmlCodeLanguage(const std::string &raw_tag)
{
    for (const std::string &attribute : {"data-language", "data-lang", "lang"})
    {
        if (const std::optional<std::string> value = ExtractHtmlAttribute(raw_tag, attribute))
        {
            const std::string trimmed = Trim(*value);
            if (!trimmed.empty())
            {
                return trimmed;
            }
        }
    }
    if (const std::optional<std::string> class_attr = ExtractHtmlAttribute(raw_tag, "class"))
    {
        return ExtractLanguageFromHtmlClass(*class_attr);
    }
    return std::nullopt;
}

std::optional<std::string> NormalizeHtmlMathAttributeTex(std::string value)
{
    value = Trim(std::move(value));
    if (value.empty() || value.find_first_of("<>\r\n") != std::string::npos)
    {
        return std::nullopt;
    }
    value = StripLatexOuterDelimiters(std::move(value));
    return value.empty() ? std::nullopt : std::make_optional(value);
}

bool LooksLikeFormulaAttributeValue(const std::string &value)
{
    return value.find_first_of("\\$_^=+-*/<>|{}") != std::string::npos;
}

std::optional<std::string> ExtractHtmlMathAttributeTex(const std::string &raw_tag, bool allow_aria_label)
{
    for (const std::string &attribute : {"data-tex", "data-latex", "data-math", "data-equation", "alttext"})
    {
        if (const std::optional<std::string> value = ExtractHtmlAttribute(raw_tag, attribute))
        {
            if (const std::optional<std::string> tex = NormalizeHtmlMathAttributeTex(*value))
            {
                return tex;
            }
        }
    }

    if (allow_aria_label)
    {
        if (const std::optional<std::string> value = ExtractHtmlAttribute(raw_tag, "aria-label"))
        {
            if (LooksLikeFormulaAttributeValue(*value))
            {
                return NormalizeHtmlMathAttributeTex(*value);
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> ExtractHtmlMathAttributeTexFromFragment(const std::string &html, bool allow_aria_label)
{
    const std::string lower_html = ToLowerAscii(html);
    std::size_t pos = 0;
    while ((pos = lower_html.find('<', pos)) != std::string::npos)
    {
        const std::size_t end = html.find('>', pos + 1);
        if (end == std::string::npos)
        {
            break;
        }
        std::string raw_tag = Trim(html.substr(pos + 1, end - pos - 1));
        if (!raw_tag.empty() && raw_tag[0] != '/')
        {
            if (const std::optional<std::string> tex = ExtractHtmlMathAttributeTex(raw_tag, allow_aria_label))
            {
                return tex;
            }
        }
        pos = end + 1;
    }
    return std::nullopt;
}

bool IsHtmlMathContainer(const std::string &name, const std::string &raw_tag)
{
    if (name == "math" || name == "mjx-container")
    {
        return true;
    }
    if (name == "span" || name == "div")
    {
        return HtmlTagHasMathClass(raw_tag) || ExtractHtmlMathAttributeTex(raw_tag, false).has_value();
    }
    return false;
}

std::optional<std::pair<std::string, std::string>> ExtractHtmlPreCodeContent(const std::string &pre_raw_tag,
                                                                            const std::string &inner_html)
{
    std::string language = ExtractHtmlCodeLanguage(pre_raw_tag).value_or("");
    const std::string lower_inner = ToLowerAscii(inner_html);
    const std::size_t code_open = lower_inner.find("<code");
    if (code_open == std::string::npos)
    {
        return std::make_pair(language, inner_html);
    }
    const std::size_t code_tag_end = inner_html.find('>', code_open + 5);
    if (code_tag_end == std::string::npos)
    {
        return std::make_pair(language, inner_html);
    }
    const std::string code_raw_tag = Trim(inner_html.substr(code_open + 1, code_tag_end - code_open - 1));
    if (language.empty())
    {
        language = ExtractHtmlCodeLanguage(code_raw_tag).value_or("");
    }
    const std::size_t code_close = lower_inner.find("</code>", code_tag_end + 1);
    if (code_close == std::string::npos)
    {
        return std::make_pair(language, inner_html.substr(code_tag_end + 1));
    }
    return std::make_pair(language, inner_html.substr(code_tag_end + 1, code_close - code_tag_end - 1));
}

std::string BuildHtmlColorRichTextMarker(const std::string &color)
{
    return kHtmlColorRichTextMarkerPrefix + color + kHtmlColorRichTextMarkerSuffix;
}

std::optional<std::string> ExtractCssColorToken(std::string color)
{
    color = ToLowerAscii(Trim(std::move(color)));
    if (color.empty() || color.find_first_of("<>\r\n") != std::string::npos)
    {
        return std::nullopt;
    }
    if (color.rfind("rgb(", 0) == 0 || color.rfind("rgba(", 0) == 0)
    {
        const std::size_t close = color.find(')');
        if (close == std::string::npos)
        {
            return std::nullopt;
        }
        std::string rgb = color.substr(0, close + 1);
        rgb.erase(std::remove_if(rgb.begin(), rgb.end(), [](unsigned char ch)
                                 { return std::isspace(ch) != 0; }),
                  rgb.end());
        return rgb;
    }
    if (color[0] == '#')
    {
        std::size_t end = 1;
        while (end < color.size() && std::isxdigit(static_cast<unsigned char>(color[end])) != 0)
        {
            ++end;
        }
        return color.substr(0, end);
    }

    std::size_t end = 0;
    while (end < color.size() && std::isalpha(static_cast<unsigned char>(color[end])) != 0)
    {
        ++end;
    }
    if (end == 0)
    {
        return std::nullopt;
    }
    return color.substr(0, end);
}

std::optional<std::string> NotionColorFromCssColor(std::string color)
{
    std::optional<std::string> normalized_color = ExtractCssColorToken(std::move(color));
    if (!normalized_color.has_value())
    {
        return std::nullopt;
    }
    color = *normalized_color;
    if (color == "grey")
    {
        color = "gray";
    }

    static const std::map<std::string, std::string> color_map = {
        {"gray", "gray"},       {"brown", "brown"},   {"orange", "orange"}, {"yellow", "yellow"},
        {"green", "green"},     {"blue", "blue"},     {"purple", "purple"}, {"pink", "pink"},
        {"red", "red"},         {"#808080", "gray"},  {"#888888", "gray"},  {"#999999", "gray"},
        {"#a52a2a", "brown"},   {"#ffa500", "orange"},{"#ffff00", "yellow"},{"#008000", "green"},
        {"#00ff00", "green"},   {"#0000ff", "blue"},  {"#800080", "purple"},{"#ffc0cb", "pink"},
        {"#ff0000", "red"},     {"#f00", "red"},      {"#00f", "blue"},     {"#0f0", "green"},
        {"rgb(255,0,0)", "red"},{"rgb(0,0,255)", "blue"},{"rgb(0,128,0)", "green"},
        {"rgb(128,128,128)", "gray"},
    };
    const auto found = color_map.find(color);
    if (found == color_map.end())
    {
        return std::nullopt;
    }
    return found->second;
}

std::optional<std::string> NotionBackgroundColorFromCssColor(std::string color)
{
    if (std::optional<std::string> notion_color = NotionColorFromCssColor(std::move(color)))
    {
        return *notion_color + "_background";
    }
    return std::nullopt;
}

std::optional<std::string> ExtractCssDeclarationValue(const std::string &style, const std::string &property)
{
    std::size_t pos = 0;
    while (pos < style.size())
    {
        const std::size_t end = style.find(';', pos);
        const std::string declaration =
            Trim(style.substr(pos, end == std::string::npos ? std::string::npos : end - pos));
        const std::size_t colon = declaration.find(':');
        if (colon != std::string::npos)
        {
            const std::string name = ToLowerAscii(Trim(declaration.substr(0, colon)));
            if (name == property)
            {
                return Trim(declaration.substr(colon + 1));
            }
        }
        if (end == std::string::npos)
        {
            break;
        }
        pos = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> ExtractHtmlTextColor(const std::string &raw_tag, const std::string &name)
{
    if (const std::optional<std::string> style = ExtractHtmlAttribute(raw_tag, "style"))
    {
        if (const std::optional<std::string> background_color = ExtractCssDeclarationValue(*style, "background-color"))
        {
            if (const std::optional<std::string> notion_color = NotionBackgroundColorFromCssColor(*background_color))
            {
                return notion_color;
            }
        }
        if (const std::optional<std::string> background = ExtractCssDeclarationValue(*style, "background"))
        {
            if (const std::optional<std::string> notion_color = NotionBackgroundColorFromCssColor(*background))
            {
                return notion_color;
            }
        }
        if (const std::optional<std::string> color = ExtractCssDeclarationValue(*style, "color"))
        {
            if (const std::optional<std::string> notion_color = NotionColorFromCssColor(*color))
            {
                return notion_color;
            }
        }
    }
    if (name == "font")
    {
        if (const std::optional<std::string> color = ExtractHtmlAttribute(raw_tag, "color"))
        {
            return NotionColorFromCssColor(*color);
        }
    }
    return std::nullopt;
}

bool CssFontWeightIsBold(std::string value)
{
    value = ToLowerAscii(Trim(std::move(value)));
    if (value == "bold" || value == "bolder")
    {
        return true;
    }
    if (value.size() == 3 && std::all_of(value.begin(), value.end(), [](unsigned char ch)
                                         { return std::isdigit(ch) != 0; }))
    {
        return std::stoi(value) >= 600;
    }
    return false;
}

std::string BuildHtmlInlineStyleOpenMarkers(const std::string &raw_tag)
{
    const std::optional<std::string> style = ExtractHtmlAttribute(raw_tag, "style");
    if (!style.has_value())
    {
        return "";
    }

    std::string markers;
    if (const std::optional<std::string> font_weight = ExtractCssDeclarationValue(*style, "font-weight"))
    {
        if (CssFontWeightIsBold(*font_weight))
        {
            markers += "**";
        }
    }
    if (const std::optional<std::string> font_style = ExtractCssDeclarationValue(*style, "font-style"))
    {
        const std::string normalized = ToLowerAscii(Trim(*font_style));
        if (normalized == "italic" || normalized == "oblique")
        {
            markers += "*";
        }
    }
    if (const std::optional<std::string> text_decoration = ExtractCssDeclarationValue(*style, "text-decoration"))
    {
        const std::string normalized = ToLowerAscii(*text_decoration);
        if (normalized.find("underline") != std::string::npos)
        {
            markers += "++";
        }
        if (normalized.find("line-through") != std::string::npos || normalized.find("strikethrough") != std::string::npos)
        {
            markers += "~~";
        }
    }
    if (const std::optional<std::string> text_decoration_line = ExtractCssDeclarationValue(*style, "text-decoration-line"))
    {
        const std::string normalized = ToLowerAscii(*text_decoration_line);
        if (normalized.find("underline") != std::string::npos && markers.find("++") == std::string::npos)
        {
            markers += "++";
        }
        if ((normalized.find("line-through") != std::string::npos ||
             normalized.find("strikethrough") != std::string::npos) &&
            markers.find("~~") == std::string::npos)
        {
            markers += "~~";
        }
    }
    return markers;
}

std::string ReverseHtmlInlineStyleMarkers(const std::string &open_markers)
{
    std::string close_markers;
    for (std::size_t i = open_markers.size(); i > 0;)
    {
        const char marker = open_markers[i - 1];
        std::size_t begin = i - 1;
        while (begin > 0 && open_markers[begin - 1] == marker)
        {
            --begin;
        }
        close_markers += open_markers.substr(begin, i - begin);
        i = begin;
    }
    return close_markers;
}

std::optional<std::string> BuildMarkdownBlockquoteFromHtml(const std::string &inner_html)
{
    const std::string markdown = Trim(HtmlFragmentToMarkdown(inner_html));
    if (markdown.empty())
    {
        return std::nullopt;
    }

    const std::vector<std::string> lines = SplitLinesPreserveEmpty(markdown);
    std::ostringstream oss;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (i != 0)
        {
            oss << "\n";
        }
        oss << "> ";
        if (!Trim(lines[i]).empty())
        {
            oss << lines[i];
        }
    }
    return oss.str();
}

std::optional<std::string> BuildMarkdownDetailsFromHtml(const std::string &inner_html)
{
    const std::string lower_html = ToLowerAscii(inner_html);
    const std::size_t summary_start = lower_html.find("<summary");
    if (summary_start == std::string::npos)
    {
        return std::nullopt;
    }
    const std::size_t summary_tag_end = lower_html.find('>', summary_start + 1);
    if (summary_tag_end == std::string::npos)
    {
        return std::nullopt;
    }
    const auto summary_match = FindMatchingHtmlEnd(lower_html, summary_start, summary_tag_end, "summary");
    if (!summary_match.has_value())
    {
        return std::nullopt;
    }

    const std::string summary =
        CollapseWhitespace(HtmlFragmentToMarkdown(inner_html.substr(summary_tag_end + 1,
                                                                    summary_match->first - summary_tag_end - 1)));
    const std::string body_html =
        inner_html.substr(0, summary_start) + inner_html.substr(summary_match->second);
    const std::string body = Trim(HtmlFragmentToMarkdown(body_html));
    if (summary.empty() && body.empty())
    {
        return std::nullopt;
    }

    std::ostringstream oss;
    oss << "> [!NOTE]";
    if (!summary.empty())
    {
        oss << " " << summary;
    }
    if (!body.empty())
    {
        const std::vector<std::string> lines = SplitLinesPreserveEmpty(body);
        for (const std::string &line : lines)
        {
            oss << "\n> " << line;
        }
    }
    return oss.str();
}

std::optional<std::string> BuildMarkdownDefinitionListFromHtml(const std::string &inner_html)
{
    const std::string lower_html = ToLowerAscii(inner_html);
    std::ostringstream oss;
    bool wrote_item = false;

    for (std::size_t i = 0; i < inner_html.size();)
    {
        const std::size_t tag_start = lower_html.find('<', i);
        if (tag_start == std::string::npos)
        {
            break;
        }
        const std::size_t tag_end = inner_html.find('>', tag_start + 1);
        if (tag_end == std::string::npos)
        {
            break;
        }

        std::string raw_tag = Trim(inner_html.substr(tag_start + 1, tag_end - tag_start - 1));
        std::string tag = ToLowerAscii(raw_tag);
        if (!tag.empty() && tag[0] == '/')
        {
            i = tag_end + 1;
            continue;
        }
        const std::size_t space = tag.find_first_of(" \t\r\n/");
        const std::string name = space == std::string::npos ? tag : tag.substr(0, space);
        if (name != "dt" && name != "dd")
        {
            i = tag_end + 1;
            continue;
        }

        const auto match = FindMatchingHtmlEnd(lower_html, tag_start, tag_end, name);
        if (!match.has_value())
        {
            i = tag_end + 1;
            continue;
        }
        const std::string item_html = inner_html.substr(tag_end + 1, match->first - tag_end - 1);
        const std::string item_markdown = CollapseWhitespace(HtmlFragmentToMarkdown(item_html));
        if (!item_markdown.empty())
        {
            if (wrote_item)
            {
                oss << "\n";
            }
            oss << "- ";
            if (name == "dt")
            {
                oss << "**" << item_markdown << "**";
            }
            else
            {
                oss << item_markdown;
            }
            wrote_item = true;
        }
        i = match->second;
    }

    if (!wrote_item)
    {
        return std::nullopt;
    }
    return oss.str();
}

std::optional<std::string> ConvertSimpleSupSubText(const std::string &text, bool superscript)
{
    std::string output;
    output.reserve(text.size() * 3);
    for (char ch : text)
    {
        if (superscript)
        {
            switch (ch)
            {
            case '0':
                output += "⁰";
                break;
            case '1':
                output += "¹";
                break;
            case '2':
                output += "²";
                break;
            case '3':
                output += "³";
                break;
            case '4':
                output += "⁴";
                break;
            case '5':
                output += "⁵";
                break;
            case '6':
                output += "⁶";
                break;
            case '7':
                output += "⁷";
                break;
            case '8':
                output += "⁸";
                break;
            case '9':
                output += "⁹";
                break;
            case '+':
                output += "⁺";
                break;
            case '-':
                output += "⁻";
                break;
            case '=':
                output += "⁼";
                break;
            case '(':
                output += "⁽";
                break;
            case ')':
                output += "⁾";
                break;
            case 'n':
                output += "ⁿ";
                break;
            case 'i':
                output += "ⁱ";
                break;
            default:
                return std::nullopt;
            }
        }
        else
        {
            switch (ch)
            {
            case '0':
                output += "₀";
                break;
            case '1':
                output += "₁";
                break;
            case '2':
                output += "₂";
                break;
            case '3':
                output += "₃";
                break;
            case '4':
                output += "₄";
                break;
            case '5':
                output += "₅";
                break;
            case '6':
                output += "₆";
                break;
            case '7':
                output += "₇";
                break;
            case '8':
                output += "₈";
                break;
            case '9':
                output += "₉";
                break;
            case '+':
                output += "₊";
                break;
            case '-':
                output += "₋";
                break;
            case '=':
                output += "₌";
                break;
            case '(':
                output += "₍";
                break;
            case ')':
                output += "₎";
                break;
            default:
                return std::nullopt;
            }
        }
    }
    return output;
}

std::string BuildSupSubFallback(const std::string &text, bool superscript)
{
    if (const std::optional<std::string> converted = ConvertSimpleSupSubText(text, superscript))
    {
        return *converted;
    }
    return std::string(superscript ? "^(" : "_(") + text + ")";
}

std::optional<std::pair<std::size_t, std::size_t>> FindMatchingHtmlEnd(const std::string &lower_html,
                                                                       std::size_t open_tag_start,
                                                                       std::size_t open_tag_end,
                                                                       const std::string &tag_name)
{
    int depth = 1;
    for (std::size_t pos = open_tag_end + 1; pos < lower_html.size();)
    {
        const std::size_t tag_start = lower_html.find('<', pos);
        if (tag_start == std::string::npos)
        {
            break;
        }
        if (lower_html.compare(tag_start, 4, "<!--") == 0)
        {
            const std::size_t comment_end = lower_html.find("-->", tag_start + 4);
            pos = (comment_end == std::string::npos) ? lower_html.size() : comment_end + 3;
            continue;
        }

        const std::size_t tag_end = lower_html.find('>', tag_start + 1);
        if (tag_end == std::string::npos)
        {
            break;
        }

        std::string tag = Trim(lower_html.substr(tag_start + 1, tag_end - tag_start - 1));
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
        {
            tag = Trim(tag.substr(1));
        }
        const bool self_closing = !tag.empty() && tag.back() == '/';
        const std::size_t space = tag.find_first_of(" \t\r\n/");
        const std::string name = space == std::string::npos ? tag : tag.substr(0, space);
        if (name == tag_name)
        {
            if (closing)
            {
                --depth;
                if (depth == 0)
                {
                    return std::make_pair(tag_start, tag_end + 1);
                }
            }
            else if (!self_closing)
            {
                ++depth;
            }
        }
        pos = tag_end + 1;
    }
    (void)open_tag_start;
    return std::nullopt;
}

std::optional<std::string> ExtractTexAnnotation(const std::string &html)
{
    const std::string lower_html = ToLowerAscii(html);
    std::size_t pos = 0;
    while ((pos = lower_html.find("<annotation", pos)) != std::string::npos)
    {
        const std::size_t tag_end = lower_html.find('>', pos + 1);
        if (tag_end == std::string::npos)
        {
            return std::nullopt;
        }
        const std::string tag = lower_html.substr(pos + 1, tag_end - pos - 1);
        const std::size_t close = lower_html.find("</annotation>", tag_end + 1);
        if (close == std::string::npos)
        {
            return std::nullopt;
        }
        if (tag.find("application/x-tex") != std::string::npos || tag.find("math/tex") != std::string::npos)
        {
            const std::string tex = Trim(DecodeHtmlTextWithoutTags(html.substr(tag_end + 1, close - tag_end - 1)));
            if (!tex.empty())
            {
                return tex;
            }
        }
        pos = close + std::strlen("</annotation>");
    }
    return std::nullopt;
}

std::string BuildMarkdownTableCellText(const std::string &inner_html)
{
    std::string text = CollapseWhitespace(HtmlFragmentToMarkdown(inner_html));
    ReplaceAllInPlace(&text, "|", "｜");
    return text;
}

std::optional<std::vector<std::string>> ExtractHtmlTableRowCells(const std::string &row_html)
{
    const std::string lower_row = ToLowerAscii(row_html);
    std::vector<std::string> cells;
    for (std::size_t pos = 0; pos < row_html.size();)
    {
        const std::size_t tag_start = lower_row.find('<', pos);
        if (tag_start == std::string::npos)
        {
            break;
        }
        const std::size_t tag_end = lower_row.find('>', tag_start + 1);
        if (tag_end == std::string::npos)
        {
            break;
        }

        std::string tag = Trim(lower_row.substr(tag_start + 1, tag_end - tag_start - 1));
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
        {
            pos = tag_end + 1;
            continue;
        }
        const std::size_t space = tag.find_first_of(" \t\r\n/");
        const std::string name = space == std::string::npos ? tag : tag.substr(0, space);
        if (name != "td" && name != "th")
        {
            pos = tag_end + 1;
            continue;
        }

        const std::string raw_tag = Trim(row_html.substr(tag_start + 1, tag_end - tag_start - 1));
        if (ExtractHtmlAttribute(raw_tag, "rowspan").has_value() || ExtractHtmlAttribute(raw_tag, "colspan").has_value())
        {
            return std::nullopt;
        }

        const auto match = FindMatchingHtmlEnd(lower_row, tag_start, tag_end, name);
        if (!match.has_value())
        {
            return std::nullopt;
        }
        cells.push_back(BuildMarkdownTableCellText(row_html.substr(tag_end + 1, match->first - tag_end - 1)));
        pos = match->second;
    }
    return cells;
}

std::optional<std::string> BuildMarkdownTableFromHtmlTable(const std::string &table_html)
{
    const std::string lower_table = ToLowerAscii(table_html);
    std::vector<std::vector<std::string>> rows;
    for (std::size_t pos = 0; pos < table_html.size();)
    {
        const std::size_t tag_start = lower_table.find("<tr", pos);
        if (tag_start == std::string::npos)
        {
            break;
        }
        const std::size_t tag_end = lower_table.find('>', tag_start + 1);
        if (tag_end == std::string::npos)
        {
            break;
        }
        const auto match = FindMatchingHtmlEnd(lower_table, tag_start, tag_end, "tr");
        if (!match.has_value())
        {
            return std::nullopt;
        }

        std::optional<std::vector<std::string>> cells =
            ExtractHtmlTableRowCells(table_html.substr(tag_end + 1, match->first - tag_end - 1));
        if (!cells.has_value())
        {
            return std::nullopt;
        }
        if (!cells->empty())
        {
            rows.push_back(std::move(*cells));
        }
        pos = match->second;
    }

    if (rows.empty())
    {
        return std::nullopt;
    }
    const std::size_t width = rows.front().size();
    if (width < 2)
    {
        return std::nullopt;
    }
    for (std::vector<std::string> &row : rows)
    {
        if (row.size() != width)
        {
            return std::nullopt;
        }
    }

    std::ostringstream oss;
    for (std::size_t row = 0; row < rows.size(); ++row)
    {
        if (row != 0)
        {
            oss << "\n";
        }
        oss << "|";
        for (const std::string &cell : rows[row])
        {
            oss << " " << cell << " |";
        }
        if (row == 0)
        {
            oss << "\n|";
            for (std::size_t column = 0; column < width; ++column)
            {
                oss << " --- |";
            }
        }
    }
    return oss.str();
}

std::string HtmlFragmentToMarkdown(std::string html)
{
    html = NormalizeLineEndings(std::move(html));
    const std::string lower_html = ToLowerAscii(html);
    std::string output;
    output.reserve(std::min<std::size_t>(html.size(), 262144));
    int pre_depth = 0;
    int li_depth = 0;
    bool just_started_li = false;
    struct HtmlListState
    {
        bool ordered = false;
        int next_number = 1;
    };
    std::vector<HtmlListState> list_stack;
    std::vector<std::string> color_stack;
    struct HtmlSpanFontState
    {
        bool colored = false;
        std::string close_style_markers;
    };
    std::vector<HtmlSpanFontState> span_font_tag_stack;
    std::string current_text_color = "default";

    auto append_break = [&](int count)
    {
        while (!output.empty() && output.back() == ' ')
        {
            output.pop_back();
        }
        for (int i = 0; i < count; ++i)
        {
            if (output.empty() || output.back() != '\n' || i > 0)
            {
                output.push_back('\n');
            }
        }
    };

    auto append_math = [&](const std::string &tex, bool display)
    {
        const std::string trimmed_tex = Trim(tex);
        if (trimmed_tex.empty())
        {
            return;
        }
        just_started_li = false;
        if (display)
        {
            append_break(2);
            output += "$$\n";
            output += trimmed_tex;
            output += "\n$$";
            append_break(2);
            return;
        }
        if (!output.empty() && output.back() != '\n' && output.back() != ' ')
        {
            output.push_back(' ');
        }
        output += "$" + trimmed_tex + "$";
    };

    auto current_line_is_plain_bullet_marker = [&]() -> bool
    {
        const std::size_t line_start = output.find_last_of('\n');
        const std::string line = output.substr(line_start == std::string::npos ? 0 : line_start + 1);
        return Trim(line) == "-";
    };

    for (std::size_t i = 0; i < html.size();)
    {
        if (html[i] != '<')
        {
            const std::size_t next = html.find('<', i);
            const std::string decoded =
                DecodeHtmlEntities(html.substr(i, next == std::string::npos ? std::string::npos : next - i));
            if (ContainsNonWhitespace(decoded))
            {
                just_started_li = false;
            }
            output += decoded;
            if (next == std::string::npos)
            {
                break;
            }
            i = next;
            continue;
        }

        if (lower_html.compare(i, 4, "<!--") == 0)
        {
            const std::size_t end = lower_html.find("-->", i + 4);
            i = (end == std::string::npos) ? html.size() : end + 3;
            continue;
        }

        const std::size_t end = html.find('>', i + 1);
        if (end == std::string::npos)
        {
            break;
        }

        const std::string raw_tag = Trim(html.substr(i + 1, end - i - 1));
        std::string tag = ToLowerAscii(raw_tag);
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
        {
            tag = Trim(tag.substr(1));
        }
        const std::size_t space = tag.find_first_of(" \t\r\n/");
        const std::string name = space == std::string::npos ? tag : tag.substr(0, space);

        bool checkbox_checked = false;
        if (!closing && pre_depth == 0 && name == "input" && IsHtmlCheckboxInput(raw_tag, &checkbox_checked))
        {
            if (li_depth > 0 && just_started_li && current_line_is_plain_bullet_marker())
            {
                output += checkbox_checked ? "[x] " : "[ ] ";
            }
            i = end + 1;
            continue;
        }

        if (!closing && IsHtmlInlineCodeTag(name) && pre_depth == 0)
        {
            const std::string close_tag = "</" + name + ">";
            const std::size_t close_pos = lower_html.find(close_tag, end + 1);
            if (close_pos != std::string::npos)
            {
                output += BuildMarkdownInlineCode(html.substr(end + 1, close_pos - end - 1));
                just_started_li = false;
                i = close_pos + close_tag.size();
                continue;
            }
        }

        if (!closing && name == "pre")
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string inner_html = html.substr(end + 1, match->first - end - 1);
                const std::optional<std::pair<std::string, std::string>> code =
                    ExtractHtmlPreCodeContent(raw_tag, inner_html);
                if (code.has_value())
                {
                    append_break(2);
                    output += BuildMarkdownCodeFence(code->second, code->first);
                    append_break(2);
                    just_started_li = false;
                    i = match->second;
                    continue;
                }
            }
        }

        if (!closing && name == "a")
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string inner_html = html.substr(end + 1, match->first - end - 1);
                const std::optional<std::string> markdown_link = BuildMarkdownLinkFromHtmlAnchor(raw_tag, inner_html);
                if (markdown_link.has_value())
                {
                    if (!output.empty() && output.back() != '\n' && output.back() != ' ')
                    {
                        output.push_back(' ');
                    }
                    output += *markdown_link;
                    just_started_li = false;
                    i = match->second;
                    continue;
                }
            }
        }

        if (!closing && name == "figure")
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string figure_html = html.substr(i, match->second - i);
                const std::optional<std::string> markdown_figure = BuildMarkdownFigureFromHtml(figure_html);
                if (markdown_figure.has_value())
                {
                    append_break(2);
                    output += *markdown_figure;
                    append_break(2);
                    just_started_li = false;
                    i = match->second;
                    continue;
                }
            }
        }

        if (!closing && name == "img")
        {
            if (const std::optional<std::string> markdown_image = BuildMarkdownImageFromHtmlImg(raw_tag))
            {
                append_break(2);
                output += *markdown_image;
                append_break(2);
                just_started_li = false;
                i = end + 1;
                continue;
            }
        }

        if (!closing && (name == "sup" || name == "sub"))
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string inner_html = html.substr(end + 1, match->first - end - 1);
                const std::string text = CollapseWhitespace(HtmlFragmentToMarkdown(inner_html));
                if (!text.empty())
                {
                    output += BuildSupSubFallback(text, name == "sup");
                    just_started_li = false;
                }
                i = match->second;
                continue;
            }
        }

        if (!closing && name == "table")
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string table_html = html.substr(i, match->second - i);
                const std::optional<std::string> markdown_table = BuildMarkdownTableFromHtmlTable(table_html);
                if (markdown_table.has_value())
                {
                    append_break(2);
                    output += *markdown_table;
                    append_break(2);
                    just_started_li = false;
                    i = match->second;
                    continue;
                }
            }
        }

        if (!closing && name == "blockquote")
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string inner_html = html.substr(end + 1, match->first - end - 1);
                const std::optional<std::string> markdown_quote = BuildMarkdownBlockquoteFromHtml(inner_html);
                if (markdown_quote.has_value())
                {
                    append_break(2);
                    output += *markdown_quote;
                    append_break(2);
                    just_started_li = false;
                    i = match->second;
                    continue;
                }
            }
        }

        if (!closing && name == "details")
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string inner_html = html.substr(end + 1, match->first - end - 1);
                const std::optional<std::string> markdown_details = BuildMarkdownDetailsFromHtml(inner_html);
                if (markdown_details.has_value())
                {
                    append_break(2);
                    output += *markdown_details;
                    append_break(2);
                    just_started_li = false;
                    i = match->second;
                    continue;
                }
            }
        }

        if (!closing && name == "dl")
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string inner_html = html.substr(end + 1, match->first - end - 1);
                const std::optional<std::string> markdown_definition_list =
                    BuildMarkdownDefinitionListFromHtml(inner_html);
                if (markdown_definition_list.has_value())
                {
                    append_break(2);
                    output += *markdown_definition_list;
                    append_break(2);
                    just_started_li = false;
                    i = match->second;
                    continue;
                }
            }
        }

        if (!closing && name == "script" && tag.find("math/tex") != std::string::npos)
        {
            const std::size_t close_pos = lower_html.find("</script>", end + 1);
            if (close_pos != std::string::npos)
            {
                append_math(DecodeHtmlTextWithoutTags(html.substr(end + 1, close_pos - end - 1)),
                            tag.find("mode=display") != std::string::npos || tag.find("display") != std::string::npos);
                i = close_pos + std::strlen("</script>");
                continue;
            }
        }

        if (!closing && IsHtmlMathContainer(name, raw_tag))
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string fragment = html.substr(i, match->second - i);
                std::optional<std::string> tex = ExtractTexAnnotation(fragment);
                if (!tex.has_value())
                {
                    tex = ExtractHtmlMathAttributeTex(raw_tag, true);
                }
                if (!tex.has_value())
                {
                    tex = ExtractHtmlMathAttributeTexFromFragment(fragment, true);
                }
                if (tex.has_value())
                {
                    append_math(*tex, tag.find("display") != std::string::npos);
                }
                i = match->second;
                continue;
            }
            if (const std::optional<std::string> tex = ExtractHtmlMathAttributeTex(raw_tag, true))
            {
                append_math(*tex, tag.find("display") != std::string::npos);
                i = end + 1;
                continue;
            }
        }

        if (!closing && (name == "script" || name == "style" || name == "head" || name == "svg" ||
                         name == "noscript" || name == "template" || name == "canvas"))
        {
            const std::string close_tag = "</" + name + ">";
            const std::size_t close_pos = lower_html.find(close_tag, end + 1);
            i = (close_pos == std::string::npos) ? html.size() : close_pos + close_tag.size();
            continue;
        }

        if (!closing && name == "ol")
        {
            int start = 1;
            if (const std::optional<std::string> start_attr = ExtractHtmlAttribute(raw_tag, "start"))
            {
                start = std::max(1, ParseIntOrDefault(*start_attr, 1));
            }
            list_stack.push_back({true, start});
            append_break(1);
        }
        else if (!closing && name == "ul")
        {
            list_stack.push_back({false, 1});
            append_break(1);
        }
        else if (closing && (name == "ol" || name == "ul"))
        {
            if (!list_stack.empty())
            {
                list_stack.pop_back();
            }
            append_break(li_depth > 0 ? 1 : 2);
        }
        else if (!closing && IsHtmlHeadingTag(name))
        {
            append_break(2);
            output += MarkdownHeadingPrefixFromHtmlName(name);
        }
        else if (pre_depth == 0 && (name == "strong" || name == "b"))
        {
            output += "**";
            just_started_li = false;
        }
        else if (pre_depth == 0 && (name == "em" || name == "i"))
        {
            output += "*";
            just_started_li = false;
        }
        else if (pre_depth == 0 && (name == "del" || name == "s" || name == "strike"))
        {
            output += "~~";
            just_started_li = false;
        }
        else if (pre_depth == 0 && name == "u")
        {
            output += "++";
            just_started_li = false;
        }
        else if (pre_depth == 0 && name == "mark")
        {
            output += kHtmlMarkRichTextMarker;
            just_started_li = false;
        }
        else if (!closing && pre_depth == 0 && (name == "span" || name == "font"))
        {
            HtmlSpanFontState state;
            if (const std::optional<std::string> text_color = ExtractHtmlTextColor(raw_tag, name))
            {
                state.colored = true;
                color_stack.push_back(current_text_color);
                current_text_color = *text_color;
                output += BuildHtmlColorRichTextMarker(current_text_color);
                just_started_li = false;
            }

            const std::string open_style_markers = BuildHtmlInlineStyleOpenMarkers(raw_tag);
            if (!open_style_markers.empty())
            {
                output += open_style_markers;
                state.close_style_markers = ReverseHtmlInlineStyleMarkers(open_style_markers);
                just_started_li = false;
            }
            span_font_tag_stack.push_back(std::move(state));
        }
        else if (closing && pre_depth == 0 && (name == "span" || name == "font") && !span_font_tag_stack.empty())
        {
            const HtmlSpanFontState state = span_font_tag_stack.back();
            span_font_tag_stack.pop_back();
            if (!state.close_style_markers.empty())
            {
                output += state.close_style_markers;
            }
            if (state.colored && !color_stack.empty())
            {
                current_text_color = color_stack.back();
                color_stack.pop_back();
                output += BuildHtmlColorRichTextMarker(current_text_color);
            }
        }
        else if (!closing && name == "li")
        {
            ++li_depth;
            append_break(1);
            if (!list_stack.empty() && list_stack.back().ordered)
            {
                output += std::to_string(list_stack.back().next_number++) + ". ";
            }
            else
            {
                output += "- ";
            }
            just_started_li = true;
        }
        else if (!closing && name == "br")
        {
            append_break(1);
        }
        else if (!closing && name == "hr")
        {
            append_break(2);
            output += "---";
            append_break(2);
            just_started_li = false;
        }
        else if (!closing && name == "pre")
        {
            ++pre_depth;
            append_break(2);
            output += "```\n";
        }
        else if (closing && name == "pre")
        {
            if (pre_depth > 0)
            {
                --pre_depth;
            }
            append_break(1);
            output += "```";
            append_break(2);
        }
        else if (!closing && (name == "p" || name == "div" || name == "section" || name == "article"))
        {
            if (!(li_depth > 0 && just_started_li))
            {
                append_break(2);
            }
        }
        else if (closing && name == "li")
        {
            if (li_depth > 0)
            {
                --li_depth;
            }
            just_started_li = false;
            append_break(2);
        }
        else if (closing && (name == "p" || name == "div" || name == "section" || name == "article" ||
                             IsHtmlHeadingTag(name)))
        {
            append_break(li_depth > 0 ? 1 : 2);
        }
        else if (!closing && (name == "td" || name == "th"))
        {
            if (output.empty() || output.back() == '\n')
            {
                output += "| ";
            }
            else
            {
                output += " | ";
            }
        }
        else if (closing && name == "tr")
        {
            append_break(1);
        }

        i = end + 1;
    }

    return RemoveEmptyMarkdownCodeFences(output);
}

std::optional<std::string> ExtractCfHtmlFragment(const std::string &html)
{
    auto read_offset = [&](const char *key) -> std::optional<std::size_t>
    {
        const std::size_t pos = html.find(key);
        if (pos == std::string::npos)
        {
            return std::nullopt;
        }
        std::size_t cursor = pos + std::strlen(key);
        while (cursor < html.size() && std::isspace(static_cast<unsigned char>(html[cursor])))
        {
            ++cursor;
        }
        std::size_t end = cursor;
        while (end < html.size() && std::isdigit(static_cast<unsigned char>(html[end])))
        {
            ++end;
        }
        if (end == cursor)
        {
            return std::nullopt;
        }
        return static_cast<std::size_t>(ParseU64OrDefault(html.substr(cursor, end - cursor), 0));
    };

    const auto start = read_offset("StartFragment:");
    const auto end = read_offset("EndFragment:");
    if (start.has_value() && end.has_value() && *start < *end && *end <= html.size())
    {
        return html.substr(*start, *end - *start);
    }

    const std::string start_marker = "<!--StartFragment-->";
    const std::string end_marker = "<!--EndFragment-->";
    const std::size_t marker_start = html.find(start_marker);
    const std::size_t marker_end = html.find(end_marker);
    if (marker_start != std::string::npos && marker_end != std::string::npos &&
        marker_start + start_marker.size() < marker_end)
    {
        return html.substr(marker_start + start_marker.size(), marker_end - marker_start - start_marker.size());
    }
    return std::nullopt;
}

std::size_t CountBlocksContaining(const std::vector<std::string> &blocks, const std::string &needle)
{
    return static_cast<std::size_t>(std::count_if(blocks.begin(), blocks.end(), [&](const std::string &block)
                                                 { return block.find(needle) != std::string::npos; }));
}

std::size_t CountOccurrencesInBlocks(const std::vector<std::string> &blocks, const std::string &needle)
{
    std::size_t count = 0;
    for (const std::string &block : blocks)
    {
        std::size_t pos = 0;
        while (!needle.empty() && (pos = block.find(needle, pos)) != std::string::npos)
        {
            ++count;
            pos += needle.size();
        }
    }
    return count;
}

std::size_t CountOccurrencesInString(const std::string &text, const std::string &needle)
{
    std::size_t count = 0;
    std::size_t pos = 0;
    while (!needle.empty() && (pos = text.find(needle, pos)) != std::string::npos)
    {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::size_t RichTextObjectCountForJsonBlock(const std::string &block)
{
    return CountOccurrencesInString(block, "\"type\":\"text\"") +
           CountOccurrencesInString(block, "\"type\":\"equation\"");
}

bool BlocksContain(const std::vector<std::string> &blocks, const std::string &needle)
{
    return std::any_of(blocks.begin(), blocks.end(), [&](const std::string &block)
                       { return block.find(needle) != std::string::npos; });
}

struct ConversionTestCase
{
    std::string name;
    std::string input;
    std::string expected_title;
    std::size_t expected_equations = 0;
    std::size_t expected_code_blocks = 0;
    std::size_t expected_todo_blocks = 0;
    std::size_t expected_quote_blocks = 0;
    std::vector<std::string> required;
    std::vector<std::string> forbidden;
};

bool RunConversionTest(const ConversionTestCase &test)
{
    const std::vector<std::string> blocks = BuildTextBlocks(test.input);
    const std::string title = BuildTitleFromContent(test.input);
    const std::size_t equations = CountOccurrencesInBlocks(blocks, "\"type\":\"equation\"");
    const std::size_t code_blocks = CountBlocksContaining(blocks, "\"type\":\"code\"");
    const std::size_t todo_blocks = CountBlocksContaining(blocks, "\"type\":\"to_do\"");
    const std::size_t quote_blocks = CountBlocksContaining(blocks, "\"type\":\"quote\"");

    auto fail = [&](const std::string &message)
    {
        std::cout << "[FAIL] " << test.name << ": " << message << "\n";
        std::cout << "       title=" << title << ", blocks=" << blocks.size() << ", equations=" << equations
                  << ", code=" << code_blocks << ", todo=" << todo_blocks << ", quote=" << quote_blocks << "\n";
        return false;
    };

    if (!test.expected_title.empty() && title != test.expected_title)
    {
        return fail("title mismatch");
    }
    if (equations != test.expected_equations)
    {
        return fail("equation count mismatch");
    }
    if (code_blocks != test.expected_code_blocks)
    {
        return fail("code block count mismatch");
    }
    if (todo_blocks != test.expected_todo_blocks)
    {
        return fail("todo block count mismatch");
    }
    if (quote_blocks != test.expected_quote_blocks)
    {
        return fail("quote block count mismatch");
    }
    for (const std::string &block : blocks)
    {
        if (RichTextObjectCountForJsonBlock(block) > 90)
        {
            return fail("rich_text object count exceeds safety limit");
        }
        if (block.size() > 400ull * 1024ull)
        {
            return fail("single block JSON exceeds safety byte limit");
        }
    }
    for (const std::string &needle : test.required)
    {
        if (!BlocksContain(blocks, needle))
        {
            return fail("missing required fragment: " + needle);
        }
    }
    for (const std::string &needle : test.forbidden)
    {
        if (BlocksContain(blocks, needle))
        {
            return fail("found forbidden fragment: " + needle);
        }
    }

    std::cout << "[PASS] " << test.name << "\n";
    return true;
}

int RunSelfTest()
{
    std::string many_inline_equations = "Many equations\n\n";
    for (int i = 0; i < 120; ++i)
    {
        many_inline_equations += "$x_" + std::to_string(i) + "$ ";
    }

    std::string long_code = "```text\n";
    long_code.append(170000, 'a');
    long_code += "\n```";

    std::string long_display_equation = "$$\n";
    long_display_equation.append(1200, 'x');
    long_display_equation += "\n$$";

    std::string unclosed_code = "```cpp\nint main() { return 0; }\n";

    const std::string loose_pipe_table =
        "| 常见科目 | 通俗理解 | 借方 | 贷方\n"
        "| 库存现金 | 手里的现金 | 增加 | 减少\n"
        "| 银行存款 | 银行账户里的钱 | 增加 | 减少\n"
        "| 应收账款 | 客户欠我的钱 | 增加 | 减少\n"
        "| 应收票据 | 客户给我的商业汇票 | 增加 | 减少\n"
        "| 预付账款 | 我先付给供应商的钱 | 增加 | 减少\n"
        "| 其他应收款 | 员工借款、赔偿款等 | 增加 | 减少\n"
        "| 原材料 | 买来的材料 | 增加 | 减少\n"
        "| 库存商品 | 已完工、可出售的商品 | 增加 | 减少\n"
        "| 固定资产 | 机器、设备、房屋 | 增加 | 减少\n"
        "| 累计折旧 | 固定资产备抵科目 | 减少 | 增加\n"
        "| 无形资产 | 专利、商标、软件等 | 增加 | 减少\n"
        "| 累计摊销 | 无形资产备抵科目 | 减少 | 增加";
    const std::string fenced_pipe_table = "```\n" + loose_pipe_table + "\n```";

    // 每行之间夹空行的表格（部分来源粘贴 Markdown 时会这样输出）。
    const std::string blank_line_separated_table =
        "| 原稿位置 | 原有内容 | 简略问题 | 本版补齐内容 |\n"
        "\n"
        "|---|---|---|---|\n"
        "\n"
        "| 题一总体路线 | 季节性基线 + 特征 + GBDT | 未说明样本如何展开 | 增加样本构造与验证 |\n"
        "\n"
        "| 题二总体路线 | beam mask 候选生成 | 未给出评分函数 | 增加数学抽象与打分 |\n"
        "\n"
        "| 开发建议 | 三天/一周粗略方向 | 没有任务拆分 | 增加排期与风险检查表 |";

    const std::vector<ConversionTestCase> tests = {
        {"plain algorithm explanation",
         "对，这题正解就是：SCC 缩点成 DAG，然后在 DAG 上做最大路径和 DP。\n\n"
         "定义：\n\n"
         "dp[u] = 以缩点后的点 u 结尾时，最多能收集多少金币\n\n"
         "如果 DAG 边为 u -> v，那么：\n\n"
         "dp[v] = max(dp[v], dp[u] + sum[v])\n\n"
         "复杂度：\n\n"
         "Tarjan/Kosaraju SCC: O(n + m)\n建 DAG: O(n + m)\nDAG DP: O(n + m)\n\n"
         "1e5 * 1e9 = 1e14\n",
         "对，这题正解就是：SCC 缩点成 DAG，然后在 DAG 上做最大路径和 DP。",
         0,
         0,
         0,
         0,
         {},
         {"\"type\":\"code\"", "\"type\":\"equation\""}},
        {"html empty pre cleanup",
         HtmlFragmentToMarkdown("<p>定义：</p><pre></pre><p>dp[u] = sum[u]</p><pre></pre><p>答案：</p>"),
         "定义：",
         0,
         0,
         0,
         0,
         {},
         {"```", "\"type\":\"code\""}},
        {"code language normalization",
         "```text\nTarjan/Kosaraju SCC: O(n + m)\n```\n\n```cpp\nint main() { return 0; }\n```\n\n```unknownlang\nx\n```",
         "Tarjan/Kosaraju SCC: O(n + m)",
         0,
         3,
         0,
         0,
         {"\"language\":\"plain text\"", "\"language\":\"c++\""},
         {"\"language\":\"text\"", "\"language\":\"unknownlang\""}},
        {"sql code blocks use clean rich text",
         "```SQL\ncreate table department (\n    dept_name varchar(20),\n    primary key (dept_name)\n);\n```\n\n```postgresql\nselect * from department;\n```",
         "create table department (",
         0,
         2,
         0,
         0,
         {"\"language\":\"sql\"", "\"rich_text\":[{\"type\":\"text\",\"text\":{\"content\":\"create table",
          "select * from department;"},
         {"\"language\":\"SQL\"", "\"language\":\"postgresql\"", "\"annotations\""}},
        {"markdown latex conversion",
         "# LaTeX smoke test\n\n普通段落 $E=mc^2$，还有 \\(\\alpha+\\beta\\)。\n\n$$\n\\int_0^1 x^2 \\, dx = \\frac{1}{3}\n$$\n\n$$x+1$$\n\n\\[y+1\\]\n\n```cpp\nint main() { return 0; }\n```",
         "LaTeX smoke test",
         5,
         1,
         0,
         0,
         {"\"language\":\"c++\"", "\"expression\":\"E=mc^2\"", "\"expression\":\"x+1\"",
          "\"expression\":\"y+1\""},
         {}},
        {"bare latex environments",
         "分段函数：\n\n"
         "\\begin{cases}\n"
         "x, & x>0 \\\\\n"
         "-x, & x\\le 0\n"
         "\\end{cases}\n\n"
         "单行矩阵：\n\n"
         "\\begin{pmatrix}1 & 0 \\\\ 0 & 1\\end{pmatrix}\n\n"
         "对齐：\n\n"
         "\\begin{align}\n"
         "a&=b+c\\\\\n"
         " &=d\n"
         "\\end{align}\n",
         "分段函数：",
         3,
         0,
         0,
         0,
         {"\"expression\":\"\\\\begin{cases}", "x\\\\le 0", "\\\\end{cases}\"",
          "\"expression\":\"\\\\begin{pmatrix}1 & 0 \\\\\\\\ 0 & 1\\\\end{pmatrix}\"",
          "\"expression\":\"\\\\begin{aligned}", "a&=b+c", "&=d", "\\\\end{aligned}\""},
         {"\"type\":\"paragraph\",\"paragraph\":{\"rich_text\":[{\"type\":\"text\",\"text\":{\"content\":\"\\\\begin{cases}\"",
          "\"type\":\"code\""}},
        {"setext headings",
         "Setext 一级\n"
         "============\n\n"
         "Setext 二级\n"
         "------------\n\n"
         "正文",
         "Setext 一级",
         0,
         0,
         0,
         0,
         {"\"type\":\"heading_1\"", "\"type\":\"heading_2\"", "Setext 一级", "Setext 二级", "正文"},
         {}},
        {"markdown heading attribute lists",
         "## 安装 {#install .tabset data-title=\"hello world\"}\n\n"
         "Setext 标题 {.wide}\n"
         "------------\n\n"
         "正文 {不是属性}",
         "安装",
         0,
         0,
         0,
         0,
         {"\"type\":\"heading_2\"", "\"content\":\"安装\"", "\"content\":\"Setext 标题\"", "正文 {不是属性}"},
         {"{#install", "{.wide}", "data-title"}},
        {"latex underline is not setext heading",
         "\\tau\\left(\\frac nd\\right)\n"
         "=========================\n\n"
         "正文",
         "\\tau\\left(\\frac nd\\right)",
         0,
         0,
         0,
         0,
         {"=========================", "正文"},
         {"\"type\":\"heading_1\"", "\"type\":\"heading_2\""}},
        {"explicit single dollar inline formulas",
         "$A$ 有两个块： $AAA$ 和 $AA$；\n\n"
         "$B$ 有两个块： $BB$ 和 $B$；\n\n"
         "复杂度是 $O(q)$，多项式是 $F(x)^q$，环境变量 $HOME$ 不是公式。",
         "$A$ 有两个块： AAA 和 AA；",
         5,
         0,
         0,
         0,
         {"\"expression\":\"A\"", "\"expression\":\"B\"", "\"expression\":\"O(q)\"", "\"expression\":\"F(x)^q\"",
          "AAA", "AA", "BB", "HOME"},
        {"\"expression\":\"AAA\"", "\"expression\":\"AA\"", "\"expression\":\"BB\"", "\"expression\":\"HOME\"",
         "$AAA$", "$AA$", "$BB$", "$HOME$"}},
        {"plain uppercase dollar markers next to text",
         "普通文本：$AAA$word、$AA$1、$BB$；公式 $x$ 保留。",
         "普通文本：AAAword、AA1、BB；公式 $x$ 保留。",
         1,
         0,
         0,
         0,
         {"AAAword", "AA1", "BB", "\"expression\":\"x\""},
         {"$AAA$", "$AA$", "$BB$", "\"expression\":\"AAA\"", "\"expression\":\"AA\"", "\"expression\":\"BB\""}},
        {"gfm strikethrough inline formatting",
         "状态：~~旧结论~~，保留 **粗体**、`code` 和 ~~公式 $x+1$~~。\n\n未闭合 ~~marker 保持原样。",
         "状态：旧结论，保留 粗体、code 和 公式 $x+1$。",
         1,
         0,
         0,
         0,
         {"\"strikethrough\":true", "\"content\":\"旧结论\"",
          "\"annotations\":{\"bold\":true,\"italic\":false,\"strikethrough\":false",
          "\"code\":true", "\"expression\":\"x+1\"", "~~marker 保持原样"},
         {"\"content\":\"~~旧结论~~\"", "\"content\":\"~~公式", "\"content\":\"公式 $x+1$\""}},
        {"markdown italic inline formatting",
         "这是 *斜体* 和 _强调_，++下划线++，C++ 保持原样，snake_case 保持原样，`*code*` 不解析，链接 [*斜体链接*](https://example.com/i)。",
         "这是 斜体 和 _强调_，下划线，C++ 保持原样，snake_case 保持原样，code 不解析，链接 [斜体链接](https://example.com...",
         0,
         0,
         0,
         0,
         {"\"content\":\"斜体\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":true",
          "\"content\":\"强调\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":true",
          "\"content\":\"下划线\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":true",
          "C++ 保持原样",
          "snake_case 保持原样", "\"content\":\"*code*\"", "\"code\":true",
          "\"content\":\"斜体链接\",\"link\":{\"url\":\"https://example.com/i\"}},\"annotations\":{\"bold\":false,\"italic\":true"},
         {"\"content\":\"*斜体*\"", "\"content\":\"_强调_\"", "\"content\":\"++下划线++\""}},
        {"markdown inline links",
         "参考 [项目文档](https://example.com/docs) 和 ~~[旧链接](https://example.com/old)~~。\n\n"
         "代码 `[x](https://bad.example)` 不解析，公式 $x+1$ 继续识别。",
         "参考 [项目文档](https://example.com/docs) 和 [旧链接](https://example.com/old)。",
         1,
         0,
         0,
         0,
         {"\"content\":\"项目文档\",\"link\":{\"url\":\"https://example.com/docs\"}",
          "\"content\":\"旧链接\",\"link\":{\"url\":\"https://example.com/old\"}",
          "\"strikethrough\":true", "\"content\":\"[x](https://bad.example)\"", "\"code\":true",
          "\"expression\":\"x+1\""},
         {"\"content\":\"[项目文档](https://example.com/docs)\"",
          "\"content\":\"[旧链接](https://example.com/old)\"",
          "\"link\":{\"url\":\"https://bad.example\"}"}},
        {"markdown autolinks",
         "访问 <https://example.com/a?x=1>，联系 <user@example.com>，忽略 <ftp://example.com>，代码 `<https://bad.example>`。",
         "",
         0,
         0,
         0,
         0,
         {"\"content\":\"https://example.com/a?x=1\",\"link\":{\"url\":\"https://example.com/a?x=1\"}",
          "\"content\":\"user@example.com\",\"link\":{\"url\":\"mailto:user@example.com\"}",
          "<ftp://example.com>", "\"content\":\"<https://bad.example>\"", "\"code\":true"},
         {"\"link\":{\"url\":\"ftp://example.com\"}", "\"link\":{\"url\":\"https://bad.example\"}"}},
        {"markdown reference links",
         "参考 [项目文档][Docs]、[API][] 和 [快捷]。\n\n"
         "| 名称 | 链接 |\n"
         "|---|---|\n"
         "| 表格 | [表格链接][tbl] |\n\n"
         "[docs]: https://example.com/docs\n"
         "[api]: <https://example.com/api>\n"
         "[快捷]: mailto:user@example.com\n"
         "[tbl]: notion://docs/table",
         "",
         0,
         0,
         0,
         0,
         {"\"content\":\"项目文档\",\"link\":{\"url\":\"https://example.com/docs\"}",
          "\"content\":\"API\",\"link\":{\"url\":\"https://example.com/api\"}",
          "\"content\":\"快捷\",\"link\":{\"url\":\"mailto:user@example.com\"}",
          "\"content\":\"表格链接\",\"link\":{\"url\":\"notion://docs/table\"}",
          "\"type\":\"table\""},
         {"[docs]:", "[api]:", "[tbl]:", "\"content\":\"[Docs]\"", "\"content\":\"[tbl]\""}},
        {"reference definitions do not interrupt paragraphs",
         "段落第一行\n"
         "[foo]: https://example.com/foo\n\n"
         "[foo]",
         "段落第一行",
         0,
         0,
         0,
         0,
         {"[foo]: https://example.com/foo", "[foo]"},
         {"\"link\":{\"url\":\"https://example.com/foo\"}"}},
        {"markdown footnotes",
         "正文有脚注[^1] 和命名脚注[^note]。\n\n"
         "[^1]: 第一条 $x+1$。\n"
         "    续行保留。\n"
         "[^note]: 参考 [链接](https://example.com/note)。",
         "正文有脚注¹ 和命名脚注^(note)。",
         1,
         0,
         0,
         0,
         {"正文有脚注¹ 和命名脚注^(note)。",
          "\"content\":\"¹: \",\"link\":null},\"annotations\":{\"bold\":true",
          "\"expression\":\"x+1\"", "续行保留",
          "\"content\":\"链接\",\"link\":{\"url\":\"https://example.com/note\"}",
          "\"content\":\"^(note): \",\"link\":null},\"annotations\":{\"bold\":true"},
         {"[^1]", "[^note]", "[^1]:", "[^note]:"}},
        {"markdown images",
         "![架构图](https://example.com/arch.png)\n\n"
         "段落里的 ![图标](https://example.com/icon.png) 可查看。\n\n"
         "![坏图](ftp://example.com/bad.png)",
         "",
         0,
         0,
         0,
         0,
         {"\"type\":\"image\"", "\"url\":\"https://example.com/arch.png\"", "\"content\":\"架构图\"",
          "\"content\":\"图标\",\"link\":{\"url\":\"https://example.com/icon.png\"}",
          "![坏图](ftp://example.com/bad.png)"},
         {"\"url\":\"ftp://example.com/bad.png\"", "\"content\":\"!\""}},
        {"markdown backslash escapes",
         "\\*不是斜体\\*，\\[不是链接\\](https://bad.example)，价格 \\$100，Windows 路径 E:\\code\\notion\\file.txt，"
         "代码 `\\*literal\\*`，公式 $x+1$。",
         "*不是斜体*，[不是链接](https://bad.example)，价格 $100，Windows 路径 E:\\code\\notion\\file.txt，代码...",
         1,
         0,
         0,
         0,
         {"*不是斜体*", "[不是链接](https://bad.example)",
          "价格 $100", "E:\\\\code\\\\notion\\\\file.txt", "\"content\":\"\\\\*literal\\\\*\"",
          "\"code\":true", "\"expression\":\"x+1\""},
         {"\"italic\":true", "\"link\":{\"url\":\"https://bad.example\"}", "\"content\":\"\\\\$100\""}},
        {"markdown highlight inline formatting",
         "这是 ==重点 **高亮** 和 $x+1$==，但 a==b 不高亮，代码 `==raw==`。",
         "这是 重点 高亮 和 $x+1$，但 a==b 不高亮，代码 raw。",
         1,
         0,
         0,
         0,
         {"\"content\":\"重点 \",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"yellow_background\"",
          "\"content\":\"高亮\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"expression\":\"x+1\"", "a==b",
          "\"content\":\"==raw==\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":true,\"color\":\"default\""},
         {"\"content\":\"==重点", "\"content\":\"==raw==\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":true,\"color\":\"yellow_background\""}},
        {"plain double equals is not highlight",
         "条件 a==b 不高亮，C++ 也保持普通文本。",
         "条件 a==b 不高亮，C++ 也保持普通文本。",
         0,
         0,
         0,
         0,
         {"a==b", "C++"},
         {"yellow_background"}},
        {"html anchor links",
         HtmlFragmentToMarkdown("<p>查看 <a href=\"https://example.com/docs?a=1&amp;b=2\">文档</a> 和 "
                                "<a href=\"javascript:alert(1)\">坏链接</a>，公式 <span>$x$</span>，代码 "
                                "<code>[x](https://bad.example)</code>。</p>"),
         "",
         1,
         0,
         0,
         0,
         {"\"content\":\"文档\",\"link\":{\"url\":\"https://example.com/docs?a=1&b=2\"}",
          "坏链接", "\"expression\":\"x\"", "\"content\":\"[x](https://bad.example)\"", "\"code\":true"},
         {"javascript:alert", "\"link\":{\"url\":\"https://bad.example\"}"}},
        {"html deep headings",
         HtmlFragmentToMarkdown("<h4>小节 $x$</h4><p>正文</p><h6><strong>细节</strong></h6>"),
         "小节 $x$",
         1,
         0,
         0,
         0,
         {"\"type\":\"heading_3\"", "\"content\":\"小节 \"", "\"expression\":\"x\"",
          "\"content\":\"细节\",\"link\":null},\"annotations\":{\"bold\":true"},
         {"\"type\":\"paragraph\",\"paragraph\":{\"rich_text\":[{\"type\":\"text\",\"text\":{\"content\":\"小节"}},
        {"html inline formatting",
         HtmlFragmentToMarkdown("<p><strong>粗体</strong>、<em>斜体</em> 和 <b>加粗</b>、<i>倾斜</i>，<u>下划线</u>，<mark>高亮</mark>，<del>删除 $x+1$</del>，"
                                "<code><strong>literal</strong></code></p>"),
         "粗体、斜体 和 加粗、倾斜，下划线，高亮，删除 $x+1$，literal",
         1,
         0,
         0,
         0,
         {"\"content\":\"粗体\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"content\":\"加粗\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"content\":\"斜体\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":true",
          "\"content\":\"倾斜\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":true",
          "\"content\":\"下划线\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":true",
          "\"content\":\"高亮\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"yellow_background\"",
          "\"strikethrough\":true", "\"content\":\"删除 \"", "\"expression\":\"x+1\"",
         "\"content\":\"literal\"", "\"code\":true"},
         {"<strong>", "</strong>", "<u>", "</u>", "<mark>", "</mark>", "\"content\":\"++下划线++\"", "\"content\":\"~~删除", "\"content\":\"$x+1$\""}},
        {"html kbd samp tt inline code",
         HtmlFragmentToMarkdown("<p>按 <kbd>Ctrl</kbd>+<kbd>K</kbd>，输出 <samp>$HOME$</samp>，旧标签 <tt>a`b</tt>。</p>"),
         "按 Ctrl+K，输出 HOME，旧标签 a`b。",
         0,
         0,
         0,
         0,
         {"\"content\":\"Ctrl\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":true",
          "\"content\":\"K\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":true",
          "\"content\":\"$HOME$\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":true",
          "\"content\":\"a`b\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":true"},
         {"\"expression\":\"HOME\"", "\"content\":\"<kbd>\"", "\"content\":\"<samp>\"", "\"content\":\"<tt>\""}},
        {"html css inline styles",
         HtmlFragmentToMarkdown("<p><span style=\"font-weight: 700\">粗</span>、"
                                "<span style=\"font-style: italic\">斜</span>、"
                                "<span style=\"text-decoration: underline line-through\">下删</span>、"
                                "<span style=\"font-weight:bold;color:green\">绿粗</span>、普通</p>"),
         "粗、斜、下删、绿粗、普通",
         0,
         0,
         0,
         0,
         {"\"content\":\"粗\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"content\":\"斜\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":true",
          "\"content\":\"下删\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":true,\"underline\":true",
          "\"content\":\"绿粗\",\"link\":null},\"annotations\":{\"bold\":true,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"green\"",
          "\"content\":\"、普通\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"default\""},
         {"font-weight", "text-decoration", "\"content\":\"**粗**\"", "\"content\":\"*斜*\"", "\"content\":\"++下删++\""}},
        {"html text colors",
         HtmlFragmentToMarkdown("<p><span style=\"color: red\">红色</span>、<font color=\"#0000ff\">蓝色</font>、"
                                "<span style=\"color:#123456\">未知</span>、<span style=\"color:red\">嵌套 <span>红内</span> 普通</span>。</p>"),
         "红色、蓝色、未知、嵌套 红内 普通。",
         0,
         0,
         0,
         0,
         {"\"content\":\"红色\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"red\"",
          "\"content\":\"蓝色\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"blue\"",
          "未知", "嵌套 红内 普通", "\"color\":\"red\""},
         {"#123456", "\"content\":\"未知\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"red\""}},
        {"html background colors",
         HtmlFragmentToMarkdown("<p><span style=\"background-color: yellow\">黄底</span>、"
                                "<span style=\"background:#ff0000 none repeat\">红底</span>、"
                                "<span style=\"color:red;background-color: rgb(0, 0, 255)\">蓝底优先</span>、"
                                "<span style=\"background-color:#123456\">未知底</span></p>"),
         "黄底、红底、蓝底优先、未知底",
         0,
         0,
         0,
         0,
         {"\"content\":\"黄底\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"yellow_background\"",
          "\"content\":\"红底\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"red_background\"",
          "\"content\":\"蓝底优先\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"blue_background\"",
          "未知底"},
         {"#123456", "\"content\":\"未知底\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"yellow_background\"",
          "\"content\":\"蓝底优先\",\"link\":null},\"annotations\":{\"bold\":false,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":false,\"color\":\"red\""}},
        {"html superscript and subscript fallback",
         HtmlFragmentToMarkdown("<p>水 H<sub>2</sub>O，面积 x<sup>2</sup>，电荷 10<sup>+</sup>，脚注 <sup>note</sup>，复杂 a<sub>ij</sub>。</p>"),
         "水 H₂O，面积 x²，电荷 10⁺，脚注 ^(note)，复杂 a_(ij)。",
         0,
         0,
         0,
         0,
         {"H₂O", "x²", "10⁺", "^(note)", "a_(ij)"},
         {"<sup>", "</sup>", "<sub>", "</sub>"}},
        {"html ordered and unordered lists",
         HtmlFragmentToMarkdown("<ol start=\"3\"><li><p>第三步</p></li><li>第四步 $x$</li></ol>"
                                "<ul><li>普通项</li></ul>"),
         "第三步",
         1,
         0,
         0,
         0,
         {"\"type\":\"numbered_list_item\"", "\"type\":\"bulleted_list_item\"", "第三步", "第四步",
          "\"expression\":\"x\"", "普通项"},
         {}},
        {"html checkbox task list",
         HtmlFragmentToMarkdown("<ul><li><input type=\"checkbox\" checked> 已完成 $x$</li>"
                                "<li><input type=\"checkbox\"> 待办</li></ul>"
                                "<p><input type=\"checkbox\" checked> 表单项</p>"),
         "已完成 $x$",
         1,
         0,
         2,
         0,
         {"\"type\":\"to_do\"", "\"checked\":true", "\"checked\":false", "\"expression\":\"x\"", "表单项"},
         {"\"type\":\"bulleted_list_item\""}},
        {"markdown paren ordered list",
         "1) 第一步 $x$\n2) 第二步 [链接](https://example.com/step)",
         "第一步 $x$",
         1,
         0,
         0,
         0,
         {"\"type\":\"numbered_list_item\"", "\"expression\":\"x\"",
          "\"content\":\"链接\",\"link\":{\"url\":\"https://example.com/step\"}"},
         {"\"content\":\"1)\"", "\"content\":\"2)\"", "\"type\":\"paragraph\""}},
        {"html blockquote",
         HtmlFragmentToMarkdown("<blockquote><p>引用 <strong>重点</strong> 和 $x$</p></blockquote><p>正文</p>"),
         "",
         1,
         0,
         0,
         1,
         {"\"type\":\"quote\"", "\"content\":\"重点\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"expression\":\"x\"", "正文"},
         {"<blockquote>", "</blockquote>"}},
        {"html details summary callout",
         HtmlFragmentToMarkdown("<details><summary><strong>证明</strong> $x$</summary>"
                                "<p>步骤 <a href=\"https://example.com/proof\">链接</a> 和 \\(y+1\\)</p>"
                                "</details><p>正文</p>"),
         "证明 $x$",
         2,
         0,
         0,
         0,
         {"\"type\":\"callout\"", "\"color\":\"blue_background\"",
          "\"content\":\"证明\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"expression\":\"x\"", "\"content\":\"链接\",\"link\":{\"url\":\"https://example.com/proof\"}",
          "\"expression\":\"y+1\"", "正文"},
         {"<details>", "<summary>", "\"type\":\"quote\""}},
        {"html definition list",
         HtmlFragmentToMarkdown("<dl><dt>颜色数</dt><dd>共有 <a href=\"https://example.com/q\">$q$ 种颜色</a>。</dd>"
                                "<dt><code>p</code></dt><dd>每种颜色的珠子数。</dd></dl>"),
         "颜色数",
         1,
         0,
         0,
         0,
         {"\"type\":\"bulleted_list_item\"", "\"content\":\"颜色数\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"expression\":\"q\"", "\"link\":{\"url\":\"https://example.com/q\"}",
          "\"content\":\"p\",\"link\":null},\"annotations\":{\"bold\":true,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":true",
          "每种颜色的珠子数"},
         {"<dl>", "<dt>", "<dd>", "\"type\":\"paragraph\""}},
        {"markdown definition list",
         "颜色数\n"
         ": 共有 $q$ 种颜色。\n\n"
         "`p`\n"
         ": 每种颜色的珠子数。\n"
         ": 可以用 $O(p)$ 枚举。\n\n"
         "正文",
         "颜色数",
         2,
         0,
         0,
         0,
         {"\"type\":\"bulleted_list_item\"",
          "\"content\":\"颜色数\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"expression\":\"q\"",
          "\"content\":\"p\",\"link\":null},\"annotations\":{\"bold\":true,\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":true",
          "\"expression\":\"O(p)\"", "正文"},
         {"\"content\":\": 共有", "\"type\":\"code\""}},
        {"html horizontal rule",
         HtmlFragmentToMarkdown("<p>上文</p><hr><p>下文 $x$</p>"),
         "上文",
         1,
         0,
         0,
         0,
         {"\"type\":\"divider\"", "上文", "下文", "\"expression\":\"x\""},
         {"\"content\":\"---\""}},
        {"html image",
         HtmlFragmentToMarkdown("<p>上文</p><img src=\"https://example.com/pic.jpg\" alt=\"配图\"><p>下文</p>"),
         "上文",
         0,
         0,
         0,
         0,
         {"\"type\":\"image\"", "\"url\":\"https://example.com/pic.jpg\"", "\"content\":\"配图\"", "下文"},
         {"<img", "\"content\":\"!\""}},
        {"html figure image caption",
         HtmlFragmentToMarkdown("<figure><img src=\"https://example.com/chart.png\" alt=\"备用说明\">"
                                "<figcaption>图 1：<strong>趋势</strong> 和 $x$</figcaption></figure><p>正文</p>"),
         "图 1：趋势 和 $x$",
         1,
         0,
         0,
         0,
         {"\"type\":\"image\"", "\"url\":\"https://example.com/chart.png\"",
          "\"content\":\"图 1：趋势 和 \"", "\"expression\":\"x\"", "正文"},
         {"备用说明", "<figure", "<figcaption", "\"content\":\"!\""}},
        {"html table becomes native table",
         HtmlFragmentToMarkdown("<table><thead><tr><th>项目</th><th>值</th></tr></thead>"
                                "<tbody><tr><td><strong>A</strong></td><td><a href=\"https://example.com/a\">链接</a></td></tr>"
                                "<tr><td>B</td><td>$x+1$</td></tr></tbody></table>"),
         "",
         1,
         0,
         0,
         0,
         {"\"type\":\"table\"", "\"type\":\"table_row\"", "项目", "值",
          "\"content\":\"A\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"content\":\"链接\",\"link\":{\"url\":\"https://example.com/a\"}",
          "\"expression\":\"x+1\""},
         {"<table", "<td", "\"type\":\"paragraph\"", "\"type\":\"code\""}},
        {"loose bracket math and dollar math",
         "这里是因为我们令了\n\n"
         "[\n"
         "r=de.\n"
         "]\n\n"
         "而 $e$ 是正整数，所以 $r$ 一定是 $d$ 的倍数。\n\n"
         "[\n"
         "\\tau\\left(\\frac nd\\right)\n"
         "=========================\n"
         "\n"
         "\\sum_{e\\mid \\frac nd}1,\n"
         "]\n\n"
         "# [\n"
         "\n"
         "\\sum_{d\\mid n}\\mu(d)\\tau\\left(\\frac nd\\right)\n"
         "\n"
         "\\sum_{d\\mid n}\\mu(d)\n"
         "\\sum_{e\\mid \\frac nd}1.\n"
         "]\n\n"
         "$$\n"
         "\\sum_{r\\mid n}\\sum_{d\\mid r}\\mu(d).\n"
         "$$\n",
         "这里是因为我们令了",
         7,
         0,
         0,
         0,
         {"\"expression\":\"r=de.\"", "\"expression\":\"e\"", "\"expression\":\"r\"", "\"expression\":\"d\"",
          "\"expression\":\"\\\\tau\\\\left(\\\\frac nd\\\\right)\\n\\n\\\\sum_{e\\\\mid \\\\frac nd}1,\"",
          "\"expression\":\"\\\\sum_{d\\\\mid n}\\\\mu(d)\\\\tau\\\\left(\\\\frac nd\\\\right)\\n\\n\\\\sum_{d\\\\mid n}\\\\mu(d)\\n\\\\sum_{e\\\\mid \\\\frac nd}1.\"",
         "\"expression\":\"\\\\sum_{r\\\\mid n}\\\\sum_{d\\\\mid r}\\\\mu(d).\""},
         {"=========================", "\"type\":\"heading_1\""}},
        {"alternating segment loose bracket formulas",
         "比如交替段长度 (L=6)：\n\n"
         "```text\n"
         "010101\n"
         "```\n\n"
         "它的长度是：\n\n"
         "[\n"
         "r-l+1\n"
         "]\n\n"
         "公式：\n\n"
         "[\n"
         "\\left\\lfloor \\frac{(5-1)^2}{4} \\right\\rfloor\n"
         "============================================\n\n"
         "# \\frac{16}{4}\n\n"
         "4\n"
         "]\n",
         "比如交替段长度 (L=6)：",
         2,
         1,
         0,
         0,
         {"\"expression\":\"r-l+1\"", "\"expression\":\"\\\\left\\\\lfloor \\\\frac{(5-1)^2}{4} \\\\right\\\\rfloor\\n\\n\\\\frac{16}{4}\\n\\n4\"",
          "\"language\":\"plain text\"", "010101"},
         {"=========================", "\"expression\":\"# \\\\frac{16}{4}\"", "\"type\":\"heading_1\""}},
        {"screenshot loose bracket answer formulas",
         "那么：\n\n"
         "> 一个非空二进制串是 beautiful，当且仅当\n\n"
         "1. (sum(t)\\neq 0)\n\n"
         "所以答案可以拆成两部分：\n\n"
         "## [\n"
         "\\text{答案}=\n"
         "\\text{sum 非 0 的子串数}\n\n"
         "\\text{奇数长度且长度至少 3 的交替子串数}\n"
         "]\n\n"
         "为什么 (sum \\bmod 3) 有用？\n\n"
         "复杂度：\n\n"
         "[\n"
         "O(n)\n"
         "]\n",
         "那么：",
         4,
         0,
         0,
         1,
         {"\"expression\":\"(\\\\sum(t)\\\\neq 0)\"",
          "\"expression\":\"\\\\text{答案}=\\n\\\\text{sum 非 0 的子串数}\\n\\n\\\\text{奇数长度且长度至少 3 的交替子串数}\"",
          "\"expression\":\"(\\\\sum \\\\bmod 3)\"",
          "\"expression\":\"O(n)\""},
         {"\"content\":\"[\"", "\"content\":\"O(n)\"", "\"content\":\"(sum(t)\\\\neq 0)\""}},
        {"tasks quotes and table",
         "> 引用里有公式 $a^2+b^2=c^2$\n\n- [x] 已完成\n- [ ] 待办\n\n| A | B |\n|---|---|\n| $x$ | y |\n",
         "引用里有公式 $a^2+b^2=c^2$",
         2,
         0,
         2,
         1,
         {"\"checked\":true", "\"checked\":false", "\"type\":\"table\"", "\"type\":\"table_row\""},
         {"\"language\":\"markdown\""}},
        {"gfm alert callout",
         "> [!NOTE]\n"
         "> 记住 $x+1$ 和 **重点**。\n"
         "> \n"
         "> 第二行保留。\n\n"
         "正文",
         "记住 $x+1$ 和 重点。",
         1,
         0,
         0,
         0,
         {"\"type\":\"callout\"", "\"color\":\"blue_background\"", "\"expression\":\"x+1\"",
          "\"content\":\"重点\",\"link\":null},\"annotations\":{\"bold\":true", "第二行保留。", "正文"},
         {"[!NOTE]", "\"type\":\"quote\""}},
        {"colon admonition callout",
         ":::warning 自定义标题\n"
         "请检查 **条件** 和 $x+1$。\n"
         "第二行。\n"
         ":::\n\n"
         "正文",
         "自定义标题",
         1,
         0,
         0,
         0,
         {"\"type\":\"callout\"", "\"color\":\"yellow_background\"", "自定义标题",
          "\"content\":\"条件\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"expression\":\"x+1\"", "第二行。", "正文"},
         {":::", "\"type\":\"quote\"", "\"type\":\"code\""}},
        {"mkdocs admonition callout",
         "???+ tip \"折叠提示\"\n"
         "    使用 **缓存** 和 $O(n)$。\n"
         "    第二行。\n\n"
         "正文",
         "折叠提示",
         1,
         0,
         0,
         0,
         {"\"type\":\"callout\"", "\"color\":\"green_background\"", "折叠提示",
          "\"content\":\"缓存\",\"link\":null},\"annotations\":{\"bold\":true",
          "\"expression\":\"O(n)\"", "第二行。", "正文"},
         {"???+", "\"type\":\"quote\"", "\"type\":\"code\""}},
        {"loose pipe table without separator",
         loose_pipe_table,
         "",
         0,
         0,
         0,
         0,
         {"\"type\":\"table\"", "\"type\":\"table_row\"", "库存现金", "累计摊销"},
         {"\"language\":\"markdown\"", "\"type\":\"equation\"", "\"type\":\"code\""}},
        {"markdown table escaped pipes and code pipes",
         "| 表达式 | 说明 |\n"
         "|---|---|\n"
         "| a \\| b | `x|y` 和 $z$ |\n",
         "",
         1,
         0,
         0,
         0,
         {"\"type\":\"table\"", "\"type\":\"table_row\"", "a | b", "\"content\":\"x|y\"",
          "\"code\":true", "\"expression\":\"z\""},
         {"\"language\":\"markdown\"", "\"type\":\"code\"", "a \\\\| b"}},
        {"fenced pipe table becomes native table",
         fenced_pipe_table,
         "",
         0,
         0,
         0,
         0,
         {"\"type\":\"table\"", "\"type\":\"table_row\"", "库存现金", "累计摊销"},
         {"\"language\":\"markdown\"", "\"type\":\"code\""}},
        {"blank line separated table becomes native table",
         blank_line_separated_table,
         "",
         0,
         0,
         0,
         0,
         {"\"type\":\"table\"", "\"type\":\"table_row\"", "题一总体路线", "增加排期与风险检查表"},
         {"\"language\":\"markdown\"", "\"type\":\"code\"", "\"type\":\"paragraph\""}},
        {"many inline equations split safely",
         many_inline_equations,
         "Many equations",
         120,
         0,
         0,
         0,
         {},
         {}},
        {"long code block split safely",
         long_code,
         "",
         0,
         2,
         0,
         0,
         {"\"language\":\"plain text\""},
         {"\"language\":\"text\""}},
        {"long display equation fallback",
         long_display_equation,
         "",
         0,
         1,
         0,
         0,
         {"\"language\":\"latex\""},
         {"\"type\":\"equation\""}},
        {"currency dollars are not equations",
         "价格是 $100，另一个价格是 $200。这里不是公式。\n",
         "价格是 $100，另一个价格是 $200。这里不是公式。",
         0,
         0,
         0,
         0,
         {},
         {"\"type\":\"equation\""}},
        {"unclosed code fence remains safe",
         unclosed_code,
         "int main() { return 0; }",
         0,
         1,
         0,
         0,
         {"\"language\":\"c++\""},
         {}},
        {"html numeric entities",
         HtmlFragmentToMarkdown("<p>&#x03b1; + &#946; &lt; 3 &amp;&amp; ok</p>"),
         "α + β < 3 && ok",
         0,
         0,
         0,
         0,
         {},
         {"&#x03b1;", "&#946;", "&lt;"}},
        {"inline code escaped dollars and paths",
         "Windows 路径 `E:\\code\\notion\\file.txt`，价格 \\$100，代码 `$not_formula$`，公式 $x+1$。",
         "Windows 路径 E:\\code\\notion\\file.txt，价格 $100，代码 $not_formula$，公式 $x+1$。",
         1,
         0,
         0,
         0,
         {"\"code\":true", "\"expression\":\"x+1\""},
         {"\"expression\":\"not_formula\""}},
        {"html script style pollution skipped",
         HtmlFragmentToMarkdown("<style>.x{color:red}</style><script>window.bad='$x$';</script><p>正文 $y$</p>"),
         "正文 $y$",
         1,
         0,
         0,
         0,
         {"\"expression\":\"y\""},
         {"window.bad", ".x{color:red}", "\"expression\":\"x\""}},
        {"html inline code protects dollar math",
         HtmlFragmentToMarkdown("<p>环境变量 <code>$HOME$</code>，公式 <span>\\(x+1\\)</span></p>"),
         "环境变量 HOME，公式 \\(x+1\\)",
         1,
         0,
         0,
         0,
         {"\"code\":true", "\"expression\":\"x+1\""},
         {"\"expression\":\"HOME\""}},
        {"html pre code language",
         HtmlFragmentToMarkdown("<p>示例</p><pre><code class=\"language-python\">print(&quot;```&quot;)\nif x &lt; 3:\n    pass</code></pre>"),
         "示例",
         0,
         1,
         0,
         0,
         {"\"language\":\"python\"", "print(\\\"```\\\")", "if x < 3:", "    pass"},
         {"\"language\":\"plain text\"", "\"type\":\"paragraph\",\"paragraph\":{\"rich_text\":[{\"type\":\"text\",\"text\":{\"content\":\"print"}},
        {"url dollar segments are not equations",
         "下载链接 https://example.com/$metadata/$value?x=1，公式 $x+1$。",
         "下载链接 https://example.com/$metadata/$value?x=1，公式 $x+1$。",
         1,
         0,
         0,
         0,
         {"\"expression\":\"x+1\""},
         {"\"expression\":\"metadata/\""}},
        {"short alphabetic variables are equations",
         "状态 $dp$ 和规模 $N$ 都是公式，环境变量 $HOME$ 不是。",
         "状态 $dp$ 和规模 $N$ 都是公式，环境变量 HOME 不是。",
         2,
         0,
         0,
         0,
         {"\"expression\":\"dp\"", "\"expression\":\"N\""},
         {"\"expression\":\"HOME\""}},
        {"multi backtick inline code remains literal",
         "代码 ``a`b$not_formula$``，公式 $x$。",
         "代码 a`b$not_formula$，公式 $x$。",
         1,
         0,
         0,
         0,
         {"\"code\":true", "\"expression\":\"x\""},
         {"\"expression\":\"not_formula\""}},
        {"empty markdown fences are skipped",
         "定义：\n\n```\n\n```\n\ndp[u] = sum[u]\n\n```\n\n```\n答案：",
         "定义：",
         0,
         0,
         0,
         0,
         {"dp[u] = sum[u]"},
         {"\"type\":\"code\""}},
        {"non-closing fence-like code line",
         "```text\nfirst\n``` not a close\nsecond\n```\n",
         "first",
         0,
         1,
         0,
         0,
         {"``` not a close\\nsecond"},
         {}},
        {"katex annotation html",
         HtmlFragmentToMarkdown("<span class=\"katex\"><span class=\"katex-mathml\"><math><semantics><mrow><mi>x</mi></mrow><annotation encoding=\"application/x-tex\">x^2+1</annotation></semantics></math></span><span class=\"katex-html\"><span>x</span><span>2</span></span></span>"),
         "$x^2+1$",
         1,
         0,
         0,
         0,
         {"\"expression\":\"x^2+1\""},
         {"katex-html"}},
        {"mathjax script formula html",
         HtmlFragmentToMarkdown("<p>公式：</p><script type=\"math/tex; mode=display\">\\frac{1}{2}</script>"),
         "公式：",
         1,
         0,
         0,
         0,
         {"\"expression\":\"\\\\frac{1}{2}\""},
         {"math/tex"}},
        {"html math attributes",
         HtmlFragmentToMarkdown("<p>行内 <span class=\"math\" data-tex=\"x_1+1\"><span>x</span></span> 继续。</p>"
                                "<math display=\"block\" alttext=\"\\frac{a}{b}\"><mi>a</mi></math>"),
         "行内 $x_1+1$ 继续。",
         2,
         0,
         0,
         0,
         {"\"expression\":\"x_1+1\"", "\"expression\":\"\\\\frac{a}{b}\"", "继续。"},
         {"data-tex", "alttext", "\"content\":\"x\""}},
    };

    bool ok = true;
    for (const ConversionTestCase &test : tests)
    {
        ok = RunConversionTest(test) && ok;
    }

    const std::string html_list_markdown = HtmlFragmentToMarkdown(
        "<ul>"
        "<li><p><code>state</code> 表示当前已经匹配了 <code>t</code> 的前 <code>state</code> 个字符；</p></li>"
        "<li><p>读入字符 <code>c</code> 后，转移到 <code>go[state][c]</code>；</p></li>"
        "<li><p>如果转移后 <code>state == m</code>，说明有一个 <code>t</code> 在当前位置结尾。</p></li>"
        "</ul>");
    const std::vector<std::string> html_list_blocks = BuildTextBlocks(html_list_markdown);
    if (html_list_markdown.find("- `state` 表示当前已经匹配了") == std::string::npos ||
        html_list_markdown.find("-\n\n`state`") != std::string::npos ||
        CountBlocksContaining(html_list_blocks, "\"type\":\"bulleted_list_item\"") != 3)
    {
        std::cout << "[FAIL] html list paragraph keeps marker with content\n";
        std::cout << "       converted=" << html_list_markdown << "\n";
        ok = false;
    }
    else
    {
        std::cout << "[PASS] html list paragraph keeps marker with content\n";
    }

    std::vector<std::string> payload_blocks;
    for (int i = 0; i < 12; ++i)
    {
        payload_blocks.push_back(std::string(85000, 'x'));
    }
    for (std::size_t begin = 0; begin < payload_blocks.size();)
    {
        const std::size_t end = SelectAppendBatchEnd(payload_blocks, begin, 40, 400ull * 1024ull);
        const std::size_t bytes = EstimateAppendChildrenBodyBytes(payload_blocks, begin, end);
        if (end <= begin || end > payload_blocks.size() || bytes > 400ull * 1024ull)
        {
            std::cout << "[FAIL] append payload batch sizing: begin=" << begin << ", end=" << end
                      << ", bytes=" << bytes << "\n";
            ok = false;
            break;
        }
        begin = end;
    }
    if (ok)
    {
        std::cout << "[PASS] append payload batch sizing\n";
    }

    try
    {
        fs::path temp_config = fs::temp_directory_path();
        temp_config /= L"notion_clipboard_win_config_self_test.ini";
        AtomicWriteFile(temp_config, "notion_token=secret_should_stay\nhotkey=Ctrl+Shift+B\n");
        UpsertConfigValue(temp_config, "hotkey", "Ctrl+Alt+U");
        UpsertConfigValue(temp_config, "tray_notifications", "false");
        const std::string saved_config = ReadWholeFile(temp_config);
        std::error_code ignored;
        fs::remove(temp_config, ignored);
        if (saved_config.find("notion_token=secret_should_stay") == std::string::npos ||
            saved_config.find("hotkey=Ctrl+Alt+U") == std::string::npos ||
            saved_config.find("tray_notifications=false") == std::string::npos)
        {
            std::cout << "[FAIL] config value upsert preserves existing settings\n";
            ok = false;
        }
        else
        {
            std::cout << "[PASS] config value upsert preserves existing settings\n";
        }
    }
    catch (const std::exception &ex)
    {
        std::cout << "[FAIL] config value upsert preserves existing settings: " << ex.what() << "\n";
        ok = false;
    }

    std::cout << (ok ? "self-test passed\n" : "self-test failed\n");
    return ok ? 0 : 1;
}

int RunDryRunText(const std::string &text)
{
    const std::string normalized = Trim(NormalizeLineEndings(text));
    if (normalized.empty())
    {
        throw std::runtime_error("输入文件没有可转换的文本");
    }

    const std::vector<std::string> blocks = BuildTextBlocks(normalized);
    const std::size_t equation_count = CountOccurrencesInBlocks(blocks, "\"type\":\"equation\"");
    const std::size_t code_count = CountBlocksContaining(blocks, "\"type\":\"code\"");
    std::cout << "dry-run: title=" << BuildTitleFromContent(normalized) << "，bytes=" << normalized.size()
              << "，blocks=" << blocks.size() << "，equations=" << equation_count
              << "，code_blocks=" << code_count << "\n";
    return 0;
}


}
