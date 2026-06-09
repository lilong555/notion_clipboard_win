#include "converter.h"

#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ncw
{
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

std::string StripTitleMarkdownMarkers(std::string line)
{
    ReplaceAllInPlace(&line, "**", "");
    ReplaceAllInPlace(&line, "__", "");
    return StripInlineCodeDelimitersForTitle(line);
}

std::string StripTitleMarkdownPrefix(std::string line)
{
    line = Trim(line);
    if (line.empty())
    {
        return line;
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
        return StripTitleMarkdownMarkers(line);
    }

    std::size_t digits = 0;
    while (digits < line.size() && std::isdigit(static_cast<unsigned char>(line[digits])))
    {
        ++digits;
    }
    if (digits > 0 && digits + 1 < line.size() && line[digits] == '.' &&
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

std::string RepairLatexExpression(std::string expression)
{
    expression = StripUtf8Bom(std::move(expression));
    expression = NormalizeLineEndings(std::move(expression));
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
        ToDo,
        Divider,
        Equation,
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

bool IsBlockEquationFenceStart(const std::string &trimmed)
{
    return trimmed == "$$" || trimmed == "\\[" || trimmed == "\\begin{equation}" ||
           trimmed == "\\begin{equation*}" || trimmed == "\\begin{align}" || trimmed == "\\begin{align*}" ||
           trimmed == "\\begin{gather}" || trimmed == "\\begin{gather*}";
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
    if (opening.rfind("\\begin{", 0) == 0)
    {
        std::string env = opening.substr(7, opening.size() - 8);
        return trimmed == "\\end{" + env + "}";
    }
    return false;
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

bool IsNumberedListLine(const std::string &line)
{
    const std::string trimmed_left = TrimLeft(line);
    std::size_t i = 0;
    while (i < trimmed_left.size() && std::isdigit(static_cast<unsigned char>(trimmed_left[i])))
    {
        ++i;
    }
    return i > 0 && i + 1 < trimmed_left.size() && trimmed_left[i] == '.' &&
           std::isspace(static_cast<unsigned char>(trimmed_left[i + 1]));
}

bool IsQuoteLine(const std::string &line)
{
    const std::string trimmed_left = TrimLeft(line);
    return trimmed_left.size() >= 2 && trimmed_left[0] == '>' &&
           std::isspace(static_cast<unsigned char>(trimmed_left[1]));
}

bool IsMarkdownTableLine(const std::string &line)
{
    const std::string trimmed = Trim(line);
    return trimmed.size() >= 3 && trimmed.find('|') != std::string::npos;
}

bool IsParagraphBoundary(const std::string &line)
{
    const std::string trimmed = Trim(line);
    char fence_char = '\0';
    std::size_t fence_len = 0;
    return trimmed.empty() || IsDividerLine(trimmed) || IsHeadingLine(line) || IsTaskListLine(line, nullptr) ||
           IsBulletListLine(line) || IsNumberedListLine(line) || IsQuoteLine(line) ||
           IsBlockEquationFenceStart(trimmed) ||
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

bool LooksLikeInlineLatexExpression(const std::string &expression)
{
    const std::string trimmed = Trim(expression);
    if (trimmed.empty())
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
        if (alpha_token.size() == 1 || (has_lower && alpha_token.size() <= 3))
        {
            return true;
        }
        if (has_lower && ShouldInsertLatexBackslash(alpha_token))
        {
            return true;
        }
    }
    return false;
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

std::vector<InlineSegment> ParseInlineMarkdown(const std::string &text)
{
    std::vector<InlineSegment> segments;
    bool bold = false;
    std::size_t i = 0;

    auto push_text = [&](const std::string &content, bool is_code = false)
    {
        if (content.empty())
        {
            return;
        }
        if (!segments.empty() && segments.back().type == InlineSegment::Type::Text &&
            segments.back().bold == bold && segments.back().code == is_code)
        {
            segments.back().content += content;
            return;
        }
        segments.push_back({InlineSegment::Type::Text, content, bold, is_code});
    };

    while (i < text.size())
    {
        if (text.compare(i, 2, "**") == 0)
        {
            bold = !bold;
            i += 2;
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
                if (!IsEscaped(text, candidate) &&
                    (open != "$" || IsInlineDollarCloseAllowed(text, candidate)))
                {
                    close_pos = candidate;
                    break;
                }
                search = candidate + close.size();
            }
            if (close_pos == std::string::npos)
            {
                return false;
            }

            const std::string expr = text.substr(i + open.size(), close_pos - i - open.size());
            if (Trim(expr).empty() || (open == "$" && !LooksLikeInlineLatexExpression(expr)))
            {
                return false;
            }
            segments.push_back({InlineSegment::Type::Equation, expr, false, false});
            i = close_pos + close.size();
            return true;
        };

        if (try_equation("$$", "$$") || try_equation("\\(", "\\)") || try_equation("$", "$"))
        {
            continue;
        }

        std::size_t next = i + 1;
        while (next < text.size() && text.compare(next, 2, "**") != 0 && text[next] != '`' &&
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

std::vector<MarkdownBlock> ParseMarkdownBlocks(const std::string &content)
{
    const std::vector<std::string> lines = SplitLinesPreserveEmpty(StripUtf8Bom(NormalizeLineEndings(content)));
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
            blocks.push_back({MarkdownBlock::Type::Code, {}, code, language});
            continue;
        }

        if (IsBlockEquationFenceStart(trimmed))
        {
            const std::string opening = trimmed;
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
            if (i < lines.size())
            {
                ++i;
            }
            blocks.push_back({MarkdownBlock::Type::Equation, {}, DedentBlockText(expression), ""});
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
            const std::string heading_text = Trim(trimmed_left.substr(level));
            const MarkdownBlock::Type type =
                level == 1 ? MarkdownBlock::Type::Heading1
                           : (level == 2 ? MarkdownBlock::Type::Heading2 : MarkdownBlock::Type::Heading3);
            blocks.push_back({type, ParseInlineMarkdown(heading_text), "", ""});
            ++i;
            continue;
        }

        bool task_checked = false;
        if (IsTaskListLine(line, &task_checked))
        {
            const std::string trimmed_left = TrimLeft(line);
            blocks.push_back({MarkdownBlock::Type::ToDo,
                              ParseInlineMarkdown(Trim(trimmed_left.substr(6))),
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
                {MarkdownBlock::Type::BulletedListItem, ParseInlineMarkdown(Trim(trimmed_left.substr(1))), "", ""});
            ++i;
            continue;
        }

        if (IsNumberedListLine(line))
        {
            const std::string trimmed_left = TrimLeft(line);
            std::size_t pos = 0;
            while (pos < trimmed_left.size() && std::isdigit(static_cast<unsigned char>(trimmed_left[pos])))
            {
                ++pos;
            }
            blocks.push_back({MarkdownBlock::Type::NumberedListItem,
                              ParseInlineMarkdown(Trim(trimmed_left.substr(pos + 1))),
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
            blocks.push_back({MarkdownBlock::Type::Quote, ParseInlineMarkdown(DedentBlockText(quote)), "", ""});
            continue;
        }

        if (IsMarkdownTableLine(line) && i + 1 < lines.size() && IsMarkdownTableLine(lines[i + 1]) &&
            lines[i + 1].find("---") != std::string::npos)
        {
            std::string table = line;
            ++i;
            while (i < lines.size() && IsMarkdownTableLine(lines[i]))
            {
                table += "\n";
                table += lines[i];
                ++i;
            }
            blocks.push_back({MarkdownBlock::Type::Code, {}, table, "plain text"});
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
        blocks.push_back({MarkdownBlock::Type::Paragraph, ParseInlineMarkdown(DedentBlockText(paragraph)), "", ""});
    }
    return blocks;
}

std::string BuildTextRichText(const std::string &text, bool bold, bool code)
{
    return "{\"type\":\"text\",\"text\":{\"content\":\"" + EscapeJson(text) +
           "\"},\"annotations\":{\"bold\":" + (bold ? "true" : "false") +
           ",\"italic\":false,\"strikethrough\":false,\"underline\":false,\"code\":" +
           (code ? "true" : "false") + ",\"color\":\"default\"}}";
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
                normalized.push_back({InlineSegment::Type::Text, chunk, false, false});
            }
            continue;
        }

        for (const std::string &chunk : SplitUtf8ByCharLimit(segment.content, kTextContentLimit))
        {
            if (!chunk.empty())
            {
                normalized.push_back({InlineSegment::Type::Text, chunk, segment.bold, segment.code});
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
        oss << BuildTextRichText(segment.content, segment.bold, segment.code);
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

void AppendRichTextBlocks(std::vector<std::string> *blocks, const std::string &type,
                          const std::vector<InlineSegment> &rich_text)
{
    for (const std::vector<InlineSegment> &group : SplitRichTextSegmentsForBlocks(rich_text))
    {
        blocks->push_back(BuildRichTextBlock(type, group));
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

    const std::vector<InlineSegment> code_text = {{InlineSegment::Type::Text, code, false, false}};
    return "{\"object\":\"block\",\"type\":\"code\",\"code\":{\"rich_text\":" + BuildRichTextJson(code_text) +
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

std::vector<std::string> BuildTextBlocks(const std::string &content)
{
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
        case MarkdownBlock::Type::ToDo:
            AppendToDoBlocks(&blocks, block.rich_text, block.checked);
            break;
        case MarkdownBlock::Type::Divider:
            blocks.push_back(BuildDividerBlock());
            break;
        case MarkdownBlock::Type::Equation:
            AppendEquationBlocks(&blocks, block.text);
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

std::string HtmlFragmentToMarkdown(std::string html)
{
    html = NormalizeLineEndings(std::move(html));
    const std::string lower_html = ToLowerAscii(html);
    std::string output;
    output.reserve(std::min<std::size_t>(html.size(), 262144));
    int pre_depth = 0;
    int li_depth = 0;
    bool just_started_li = false;

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

        std::string tag = ToLowerAscii(Trim(html.substr(i + 1, end - i - 1)));
        const bool closing = !tag.empty() && tag[0] == '/';
        if (closing)
        {
            tag = Trim(tag.substr(1));
        }
        const std::size_t space = tag.find_first_of(" \t\r\n/");
        const std::string name = space == std::string::npos ? tag : tag.substr(0, space);

        if (!closing && name == "code" && pre_depth == 0)
        {
            const std::size_t close_pos = lower_html.find("</code>", end + 1);
            if (close_pos != std::string::npos)
            {
                output += BuildMarkdownInlineCode(html.substr(end + 1, close_pos - end - 1));
                just_started_li = false;
                i = close_pos + std::strlen("</code>");
                continue;
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

        if (!closing && ((name == "span" && tag.find("katex") != std::string::npos) ||
                         name == "mjx-container" || name == "math"))
        {
            const auto match = FindMatchingHtmlEnd(lower_html, i, end, name);
            if (match.has_value())
            {
                const std::string fragment = html.substr(i, match->second - i);
                const auto tex = ExtractTexAnnotation(fragment);
                if (tex.has_value())
                {
                    append_math(*tex, tag.find("display") != std::string::npos);
                }
                i = match->second;
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

        if (!closing && (name == "h1" || name == "h2" || name == "h3"))
        {
            append_break(2);
            output += (name == "h1") ? "# " : (name == "h2" ? "## " : "### ");
        }
        else if (!closing && name == "li")
        {
            ++li_depth;
            append_break(1);
            output += "- ";
            just_started_li = true;
        }
        else if (!closing && name == "br")
        {
            append_break(1);
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
                             name == "h1" || name == "h2" || name == "h3"))
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
        {"markdown latex conversion",
         "# LaTeX smoke test\n\n普通段落 $E=mc^2$，还有 \\(\\alpha+\\beta\\)。\n\n$$\n\\int_0^1 x^2 \\, dx = \\frac{1}{3}\n$$\n\n```cpp\nint main() { return 0; }\n```",
         "LaTeX smoke test",
         3,
         1,
         0,
         0,
         {"\"language\":\"c++\"", "\"expression\":\"E=mc^2\""},
         {}},
        {"tasks quotes and table",
         "> 引用里有公式 $a^2+b^2=c^2$\n\n- [x] 已完成\n- [ ] 待办\n\n| A | B |\n|---|---|\n| $x$ | y |\n",
         "引用里有公式 $a^2+b^2=c^2$",
         1,
         1,
         2,
         1,
         {"\"checked\":true", "\"checked\":false", "\"language\":\"plain text\""},
         {"\"language\":\"markdown\""}},
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
         "Windows 路径 E:\\code\\notion\\file.txt，价格 \\$100，代码 $not_formula$，公式 $x+1$。",
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
         "环境变量 $HOME$，公式 \\(x+1\\)",
         1,
         0,
         0,
         0,
         {"\"code\":true", "\"expression\":\"x+1\""},
         {"\"expression\":\"HOME\""}},
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
         "状态 $dp$ 和规模 $N$ 都是公式，环境变量 $HOME$ 不是。",
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
