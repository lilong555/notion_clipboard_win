#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>

#include "app_icon.h"
#include "http_client.h"
#include "json.h"
#include "win_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
using ncw::JsonValue;
using ncw::ParseJson;
using ncw::CreateGeneratedAppIcon;
using ncw::HttpResponse;
using ncw::LastErrorMessage;
using ncw::Utf8ToWide;
using ncw::WideToUtf8;
using ncw::WinHttpClient;

constexpr UINT_PTR kClipboardDebounceTimer = 1001;
constexpr UINT kUploadHotkeyId = 2001;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kMenuUploadNow = 3001;
constexpr UINT kMenuHotkeyStatus = 3002;
constexpr UINT kMenuToggleHotkey = 3003;
constexpr UINT kMenuRecordHotkey = 3004;
constexpr UINT kMenuToggleNotifications = 3005;
constexpr UINT kMenuToggleClipboardListener = 3006;
constexpr UINT kMenuOpenConfig = 3007;
constexpr UINT kMenuOpenLog = 3008;
constexpr UINT kMenuOpenStateDir = 3009;
constexpr UINT kMenuExit = 3010;
constexpr const wchar_t *kAppDisplayName = L"Notion Clipboard Win";

std::string Trim(const std::string &input)
{
    std::size_t begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])))
    {
        ++begin;
    }
    std::size_t end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
    {
        --end;
    }
    return input.substr(begin, end - begin);
}

std::string ToLowerAscii(std::string input)
{
    std::transform(input.begin(), input.end(), input.begin(), [](unsigned char ch)
                   { return static_cast<char>(std::tolower(ch)); });
    return input;
}

bool ParseBool(std::string value)
{
    value = ToLowerAscii(Trim(value));
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

int ParseIntOrDefault(const std::string &value, int fallback)
{
    try
    {
        return std::stoi(Trim(value));
    }
    catch (...)
    {
        return fallback;
    }
}

std::uint64_t ParseU64OrDefault(const std::string &value, std::uint64_t fallback)
{
    try
    {
        return static_cast<std::uint64_t>(std::stoull(Trim(value)));
    }
    catch (...)
    {
        return fallback;
    }
}

std::string ReadWholeFile(const fs::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        throw std::runtime_error("无法打开文件: " + WideToUtf8(path.wstring()));
    }
    std::ostringstream oss;
    oss << input.rdbuf();
    return oss.str();
}

void AtomicWriteFile(const fs::path &path, const std::string &content)
{
    const fs::path parent = path.parent_path();
    if (!parent.empty())
    {
        fs::create_directories(parent);
    }

    std::wstring suffix = L".tmp.";
    suffix += std::to_wstring(GetCurrentProcessId());
    suffix += L".";
    suffix += std::to_wstring(GetTickCount64());

    fs::path temp_path = path;
    temp_path += suffix;

    {
        std::ofstream output(temp_path, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            throw std::runtime_error("无法写入临时文件: " + WideToUtf8(temp_path.wstring()));
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.close();
        if (!output)
        {
            throw std::runtime_error("写入临时文件失败: " + WideToUtf8(temp_path.wstring()));
        }
    }

    if (!MoveFileExW(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        const std::string error = LastErrorMessage();
        std::error_code ignored;
        fs::remove(temp_path, ignored);
        throw std::runtime_error("原子替换文件失败: " + error);
    }
}

void UpsertConfigValue(const fs::path &path, const std::string &key, const std::string &value)
{
    std::vector<std::string> lines;
    if (fs::exists(path))
    {
        std::istringstream input(ReadWholeFile(path));
        std::string line;
        while (std::getline(input, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            lines.push_back(line);
        }
    }

    const std::string normalized_key = ToLowerAscii(Trim(key));
    bool updated = false;
    for (std::string &line : lines)
    {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';')
        {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        if (ToLowerAscii(Trim(line.substr(0, eq))) == normalized_key)
        {
            line = key + "=" + value;
            updated = true;
            break;
        }
    }

    if (!updated)
    {
        lines.push_back(key + "=" + value);
    }

    std::ostringstream output;
    for (const std::string &line : lines)
    {
        output << line << "\n";
    }
    AtomicWriteFile(path, output.str());
}

std::uint64_t NowUnixMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string LocalTimestamp()
{
    SYSTEMTIME time;
    GetLocalTime(&time);
    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << time.wYear << "-" << std::setw(2) << time.wMonth << "-"
        << std::setw(2) << time.wDay << " " << std::setw(2) << time.wHour << ":" << std::setw(2) << time.wMinute
        << ":" << std::setw(2) << time.wSecond;
    return oss.str();
}

std::string IsoUtcTimestampFromUnixMs(std::uint64_t unix_ms)
{
    constexpr std::uint64_t kFileTimeUnixEpoch = 116444736000000000ull;
    const std::uint64_t ticks = kFileTimeUnixEpoch + unix_ms * 10000ull;

    FILETIME file_time;
    file_time.dwLowDateTime = static_cast<DWORD>(ticks & 0xffffffffull);
    file_time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);

    SYSTEMTIME time;
    if (!FileTimeToSystemTime(&file_time, &time))
    {
        GetSystemTime(&time);
    }

    std::ostringstream oss;
    oss << std::setfill('0') << std::setw(4) << time.wYear << "-" << std::setw(2) << time.wMonth << "-"
        << std::setw(2) << time.wDay << "T" << std::setw(2) << time.wHour << ":" << std::setw(2) << time.wMinute
        << ":" << std::setw(2) << time.wSecond << "Z";
    return oss.str();
}

fs::path ModuleDirectory()
{
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size())
    {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (size == 0)
    {
        return fs::current_path();
    }
    buffer.resize(size);
    return fs::path(buffer).parent_path();
}

std::wstring GetEnvWide(const wchar_t *name)
{
    DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0)
    {
        return L"";
    }
    std::wstring value(size, L'\0');
    DWORD written = GetEnvironmentVariableW(name, value.data(), size);
    if (written == 0)
    {
        return L"";
    }
    value.resize(written);
    return value;
}

std::string GetEnvUtf8(const wchar_t *name)
{
    return WideToUtf8(GetEnvWide(name));
}

fs::path DefaultStateDir()
{
    std::wstring local_app_data = GetEnvWide(L"LOCALAPPDATA");
    if (!local_app_data.empty())
    {
        return fs::path(local_app_data) / L"NotionClipboardWin";
    }
    std::wstring temp(MAX_PATH, L'\0');
    const DWORD size = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    if (size > 0 && size < temp.size())
    {
        temp.resize(size);
        return fs::path(temp) / L"NotionClipboardWin";
    }
    return fs::current_path() / L"state";
}

fs::path DefaultConfigPath()
{
    return ModuleDirectory() / L"notion_clipboard_win.ini";
}

class Logger
{
public:
    Logger(fs::path log_path, bool mirror_console) : log_path_(std::move(log_path)), mirror_console_(mirror_console)
    {
        fs::create_directories(log_path_.parent_path());
    }

    void Info(const std::string &message)
    {
        Write("INFO", message);
    }

    void Warn(const std::string &message)
    {
        Write("WARN", message);
    }

    void Error(const std::string &message)
    {
        Write("ERROR", message);
    }

private:
    void Write(const char *level, const std::string &message)
    {
        const std::string line = LocalTimestamp() + " [" + level + "] " + message + "\n";
        std::lock_guard<std::mutex> lock(mutex_);
        std::ofstream output(log_path_, std::ios::binary | std::ios::app);
        if (output)
        {
            output.write(line.data(), static_cast<std::streamsize>(line.size()));
        }
        if (mirror_console_)
        {
            std::cout << line;
        }
        OutputDebugStringW(Utf8ToWide(line).c_str());
    }

    fs::path log_path_;
    bool mirror_console_ = false;
    std::mutex mutex_;
};

std::string EscapeJson(const std::string &input)
{
    std::ostringstream oss;
    for (unsigned char ch : input)
    {
        switch (ch)
        {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\b':
            oss << "\\b";
            break;
        case '\f':
            oss << "\\f";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
                    << std::dec << std::setfill(' ');
            }
            else
            {
                oss << static_cast<char>(ch);
            }
            break;
        }
    }
    return oss.str();
}

std::uint64_t Fnv1a64(const std::string &text)
{
    std::uint64_t hash = 14695981039346656037ull;
    for (unsigned char ch : text)
    {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string Hex64(std::uint64_t value)
{
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << value;
    return oss.str();
}

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

std::string SummarizeForLog(const std::string &text, std::size_t limit = 600)
{
    std::string trimmed = Trim(text);
    if (trimmed.size() <= limit)
    {
        return trimmed;
    }
    return trimmed.substr(0, limit) + "...";
}

std::string CanonicalizeNotionId(std::string input)
{
    input = Trim(input);
    std::string hex;
    for (unsigned char ch : input)
    {
        if (std::isxdigit(ch))
        {
            hex.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    if (hex.size() != 32)
    {
        return input;
    }
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
           hex.substr(20);
}

struct HotkeySpec
{
    UINT modifiers = 0;
    UINT vk = 0;
    std::string display;
};

std::vector<std::string> SplitHotkeyTokens(const std::string &value)
{
    std::vector<std::string> tokens;
    std::string current;
    for (char ch : value)
    {
        if (ch == '+')
        {
            std::string token = Trim(current);
            if (!token.empty())
            {
                tokens.push_back(token);
            }
            current.clear();
            continue;
        }
        if (ch != '<' && ch != '>')
        {
            current.push_back(ch);
        }
    }

    std::string token = Trim(current);
    if (!token.empty())
    {
        tokens.push_back(token);
    }
    return tokens;
}

std::optional<std::pair<UINT, std::string>> ParseHotkeyKey(const std::string &token)
{
    const std::string key = ToLowerAscii(token);
    if (key.size() == 1)
    {
        const unsigned char ch = static_cast<unsigned char>(key[0]);
        if (std::isalpha(ch))
        {
            return std::make_pair(static_cast<UINT>(std::toupper(ch)), std::string(1, static_cast<char>(std::toupper(ch))));
        }
        if (std::isdigit(ch))
        {
            return std::make_pair(static_cast<UINT>(ch), std::string(1, static_cast<char>(ch)));
        }
    }

    if (key.size() >= 2 && key[0] == 'f')
    {
        const int index = ParseIntOrDefault(key.substr(1), 0);
        if (index >= 1 && index <= 24)
        {
            return std::make_pair(static_cast<UINT>(VK_F1 + index - 1), "F" + std::to_string(index));
        }
    }

    const std::map<std::string, std::pair<UINT, std::string>> names = {
        {"backspace", {VK_BACK, "Backspace"}},
        {"del", {VK_DELETE, "Delete"}},
        {"delete", {VK_DELETE, "Delete"}},
        {"down", {VK_DOWN, "Down"}},
        {"end", {VK_END, "End"}},
        {"enter", {VK_RETURN, "Enter"}},
        {"esc", {VK_ESCAPE, "Esc"}},
        {"escape", {VK_ESCAPE, "Esc"}},
        {"home", {VK_HOME, "Home"}},
        {"ins", {VK_INSERT, "Insert"}},
        {"insert", {VK_INSERT, "Insert"}},
        {"left", {VK_LEFT, "Left"}},
        {"pagedown", {VK_NEXT, "PageDown"}},
        {"pageup", {VK_PRIOR, "PageUp"}},
        {"pause", {VK_PAUSE, "Pause"}},
        {"pgdn", {VK_NEXT, "PageDown"}},
        {"pgup", {VK_PRIOR, "PageUp"}},
        {"printscreen", {VK_SNAPSHOT, "PrintScreen"}},
        {"prtsc", {VK_SNAPSHOT, "PrintScreen"}},
        {"return", {VK_RETURN, "Enter"}},
        {"right", {VK_RIGHT, "Right"}},
        {"space", {VK_SPACE, "Space"}},
        {"tab", {VK_TAB, "Tab"}},
        {"up", {VK_UP, "Up"}},
    };
    const auto it = names.find(key);
    if (it != names.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::optional<std::string> HotkeyKeyLabelFromVk(UINT vk)
{
    if (vk >= 'A' && vk <= 'Z')
    {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= '0' && vk <= '9')
    {
        return std::string(1, static_cast<char>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24)
    {
        return "F" + std::to_string(static_cast<int>(vk - VK_F1 + 1));
    }

    const std::map<UINT, std::string> names = {
        {VK_BACK, "Backspace"},
        {VK_DELETE, "Delete"},
        {VK_DOWN, "Down"},
        {VK_END, "End"},
        {VK_RETURN, "Enter"},
        {VK_HOME, "Home"},
        {VK_INSERT, "Insert"},
        {VK_LEFT, "Left"},
        {VK_NEXT, "PageDown"},
        {VK_PRIOR, "PageUp"},
        {VK_PAUSE, "Pause"},
        {VK_SNAPSHOT, "PrintScreen"},
        {VK_RIGHT, "Right"},
        {VK_SPACE, "Space"},
        {VK_TAB, "Tab"},
        {VK_UP, "Up"},
    };
    const auto it = names.find(vk);
    if (it != names.end())
    {
        return it->second;
    }
    return std::nullopt;
}

std::string FormatHotkeyDisplay(UINT modifiers, const std::string &key_label)
{
    std::vector<std::string> parts;
    if ((modifiers & MOD_CONTROL) != 0)
    {
        parts.push_back("Ctrl");
    }
    if ((modifiers & MOD_ALT) != 0)
    {
        parts.push_back("Alt");
    }
    if ((modifiers & MOD_SHIFT) != 0)
    {
        parts.push_back("Shift");
    }
    if ((modifiers & MOD_WIN) != 0)
    {
        parts.push_back("Win");
    }
    parts.push_back(key_label);

    std::ostringstream display;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
        {
            display << "+";
        }
        display << parts[i];
    }
    return display.str();
}

HotkeySpec ParseHotkeyOrThrow(const std::string &value)
{
    HotkeySpec spec;
    std::string key_label;
    for (const std::string &raw_token : SplitHotkeyTokens(value))
    {
        const std::string token = ToLowerAscii(Trim(raw_token));
        if (token == "ctrl" || token == "control")
        {
            spec.modifiers |= MOD_CONTROL;
        }
        else if (token == "alt")
        {
            spec.modifiers |= MOD_ALT;
        }
        else if (token == "shift")
        {
            spec.modifiers |= MOD_SHIFT;
        }
        else if (token == "win" || token == "windows" || token == "super" || token == "meta")
        {
            spec.modifiers |= MOD_WIN;
        }
        else
        {
            const auto key = ParseHotkeyKey(token);
            if (!key.has_value())
            {
                throw std::runtime_error("无法解析热键: " + value);
            }
            if (spec.vk != 0)
            {
                throw std::runtime_error("热键只能包含一个主按键: " + value);
            }
            spec.vk = key->first;
            key_label = key->second;
        }
    }

    if (spec.vk == 0 || spec.modifiers == 0)
    {
        throw std::runtime_error("热键必须包含至少一个修饰键和一个主按键: " + value);
    }
    spec.display = FormatHotkeyDisplay(spec.modifiers, key_label);
    return spec;
}

bool IsModifierVirtualKey(UINT vk)
{
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU ||
           vk == VK_RMENU || vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LWIN ||
           vk == VK_RWIN;
}

bool IsVirtualKeyDown(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

UINT CurrentHotkeyModifiers()
{
    UINT modifiers = 0;
    if (IsVirtualKeyDown(VK_CONTROL) || IsVirtualKeyDown(VK_LCONTROL) || IsVirtualKeyDown(VK_RCONTROL))
    {
        modifiers |= MOD_CONTROL;
    }
    if (IsVirtualKeyDown(VK_MENU) || IsVirtualKeyDown(VK_LMENU) || IsVirtualKeyDown(VK_RMENU))
    {
        modifiers |= MOD_ALT;
    }
    if (IsVirtualKeyDown(VK_SHIFT) || IsVirtualKeyDown(VK_LSHIFT) || IsVirtualKeyDown(VK_RSHIFT))
    {
        modifiers |= MOD_SHIFT;
    }
    if (IsVirtualKeyDown(VK_LWIN) || IsVirtualKeyDown(VK_RWIN))
    {
        modifiers |= MOD_WIN;
    }
    return modifiers;
}

std::optional<HotkeySpec> HotkeySpecFromRecordedKey(UINT modifiers, UINT vk)
{
    if (modifiers == 0 || IsModifierVirtualKey(vk) || vk == VK_ESCAPE)
    {
        return std::nullopt;
    }
    const auto key_label = HotkeyKeyLabelFromVk(vk);
    if (!key_label.has_value())
    {
        return std::nullopt;
    }

    HotkeySpec spec;
    spec.modifiers = modifiers;
    spec.vk = vk;
    spec.display = FormatHotkeyDisplay(modifiers, *key_label);
    return spec;
}

struct AppConfig
{
    std::string notion_token;
    std::string data_source_id;
    std::string database_id;
    std::string title_property_name;
    std::string content_property_name;
    std::string created_time_property_name = "创建时间";
    int content_property_max_chars = 1800;
    fs::path state_dir = DefaultStateDir();
    std::string hotkey = "Ctrl+Shift+B";
    bool enable_hotkey = true;
    bool enable_clipboard_listener = false;
    bool tray_notifications = true;
    int debounce_ms = 750;
    int duplicate_suppression_ms = 3000;
    bool upload_initial_clipboard = false;
    std::uint64_t max_clipboard_bytes = 262144;
    int min_request_interval_ms = 400;
    int append_batch_size = 40;
    int max_retry_attempts = 12;
    int http_retry_attempts = 3;
};

void ApplyConfigValue(AppConfig *config, const std::string &key, const std::string &value)
{
    const std::string normalized = ToLowerAscii(Trim(key));
    const std::string trimmed_value = Trim(value);
    if (normalized == "notion_token")
    {
        config->notion_token = trimmed_value;
    }
    else if (normalized == "data_source_id")
    {
        config->data_source_id = CanonicalizeNotionId(trimmed_value);
    }
    else if (normalized == "database_id")
    {
        config->database_id = CanonicalizeNotionId(trimmed_value);
    }
    else if (normalized == "title_property_name")
    {
        config->title_property_name = trimmed_value;
    }
    else if (normalized == "content_property_name")
    {
        config->content_property_name = trimmed_value;
    }
    else if (normalized == "created_time_property_name")
    {
        config->created_time_property_name = trimmed_value;
    }
    else if (normalized == "content_property_max_chars")
    {
        config->content_property_max_chars = std::max(100, ParseIntOrDefault(trimmed_value, 1800));
    }
    else if (normalized == "state_dir")
    {
        if (!trimmed_value.empty())
        {
            config->state_dir = fs::path(Utf8ToWide(trimmed_value));
        }
    }
    else if (normalized == "hotkey")
    {
        if (!trimmed_value.empty())
        {
            config->hotkey = trimmed_value;
        }
    }
    else if (normalized == "enable_hotkey")
    {
        config->enable_hotkey = ParseBool(trimmed_value);
    }
    else if (normalized == "enable_clipboard_listener")
    {
        config->enable_clipboard_listener = ParseBool(trimmed_value);
    }
    else if (normalized == "tray_notifications")
    {
        config->tray_notifications = ParseBool(trimmed_value);
    }
    else if (normalized == "debounce_ms")
    {
        config->debounce_ms = std::max(100, ParseIntOrDefault(trimmed_value, 750));
    }
    else if (normalized == "duplicate_suppression_ms")
    {
        config->duplicate_suppression_ms = std::max(0, ParseIntOrDefault(trimmed_value, 3000));
    }
    else if (normalized == "upload_initial_clipboard")
    {
        config->upload_initial_clipboard = ParseBool(trimmed_value);
    }
    else if (normalized == "max_clipboard_bytes")
    {
        config->max_clipboard_bytes = std::max<std::uint64_t>(1024, ParseU64OrDefault(trimmed_value, 262144));
    }
    else if (normalized == "min_request_interval_ms")
    {
        config->min_request_interval_ms = std::max(0, ParseIntOrDefault(trimmed_value, 400));
    }
    else if (normalized == "append_batch_size")
    {
        config->append_batch_size = std::max(1, std::min(90, ParseIntOrDefault(trimmed_value, 40)));
    }
    else if (normalized == "max_retry_attempts")
    {
        config->max_retry_attempts = std::max(0, ParseIntOrDefault(trimmed_value, 12));
    }
    else if (normalized == "http_retry_attempts")
    {
        config->http_retry_attempts = std::max(0, std::min(8, ParseIntOrDefault(trimmed_value, 3)));
    }
}

AppConfig LoadConfig(const fs::path &path)
{
    AppConfig config;
    if (fs::exists(path))
    {
        std::istringstream input(ReadWholeFile(path));
        std::string line;
        while (std::getline(input, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#' || line[0] == ';')
            {
                continue;
            }
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos)
            {
                continue;
            }
            ApplyConfigValue(&config, line.substr(0, eq), line.substr(eq + 1));
        }
    }

    if (config.notion_token.empty())
    {
        config.notion_token = GetEnvUtf8(L"NOTION_TOKEN");
    }
    if (config.data_source_id.empty())
    {
        config.data_source_id = CanonicalizeNotionId(GetEnvUtf8(L"NOTION_DATA_SOURCE_ID"));
    }
    if (config.database_id.empty())
    {
        config.database_id = CanonicalizeNotionId(GetEnvUtf8(L"NOTION_DATABASE_ID"));
    }
    return config;
}

struct CliOptions
{
    fs::path config_path = DefaultConfigPath();
    fs::path dry_run_file_path;
    bool once = false;
    bool validate_config = false;
    bool dry_run = false;
    bool self_test = false;
    bool help = false;
};

CliOptions ParseCli(int argc, wchar_t **argv)
{
    CliOptions options;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h" || arg == L"/?")
        {
            options.help = true;
        }
        else if (arg == L"--once")
        {
            options.once = true;
        }
        else if (arg == L"--validate-config")
        {
            options.validate_config = true;
        }
        else if (arg == L"--dry-run")
        {
            options.dry_run = true;
        }
        else if (arg == L"--dry-run-file")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--dry-run-file 缺少路径");
            }
            options.dry_run = true;
            options.dry_run_file_path = fs::path(argv[++i]);
        }
        else if (arg == L"--self-test")
        {
            options.self_test = true;
        }
        else if (arg == L"--config")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--config 缺少路径");
            }
            options.config_path = fs::path(argv[++i]);
        }
        else
        {
            throw std::runtime_error("未知参数: " + WideToUtf8(arg));
        }
    }
    return options;
}

void PrintHelp()
{
    std::cout << "Notion Clipboard Win\n\n"
              << "用法:\n"
              << "  notion_clipboard_win.exe [--config path]              启动后台托盘进程\n"
              << "  notion_clipboard_win.exe --once [--config path]       只上传当前剪贴板一次\n"
              << "  notion_clipboard_win.exe --validate-config            验证 Notion 配置\n"
              << "  notion_clipboard_win.exe --dry-run --once             读取剪贴板但不上传\n\n"
              << "  notion_clipboard_win.exe --self-test                  运行本地转换回归测试\n\n"
              << "  notion_clipboard_win.exe --dry-run-file path          读取文件并转换统计，不上传\n\n"
              << "默认热键:\n"
              << "  Ctrl+Shift+B\n\n"
              << "配置默认路径:\n"
              << "  " << WideToUtf8(DefaultConfigPath().wstring()) << "\n";
}

class UploadFailure : public std::runtime_error
{
public:
    UploadFailure(std::string message, bool retryable, int retry_after_seconds)
        : std::runtime_error(std::move(message)), retryable_(retryable), retry_after_seconds_(retry_after_seconds)
    {
    }

    bool retryable() const
    {
        return retryable_;
    }

    int retry_after_seconds() const
    {
        return retry_after_seconds_;
    }

private:
    bool retryable_ = false;
    int retry_after_seconds_ = 0;
};

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

std::string BuildTextRichText(const std::string &text, bool bold = false, bool code = false)
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

struct UploadJob
{
    std::string id;
    std::uint64_t created_at_ms = 0;
    std::uint64_t not_before_ms = 0;
    int attempts = 0;
    std::string hash;
    std::string title;
    std::string content;
    std::string page_id;
    std::string page_url;
    std::size_t appended_block_count = 0;
    std::string last_error;
};

std::atomic<std::uint64_t> g_job_counter{0};

UploadJob MakeUploadJob(const std::string &content)
{
    UploadJob job;
    job.created_at_ms = NowUnixMs();
    job.not_before_ms = 0;
    job.hash = Hex64(Fnv1a64(content));
    job.title = BuildTitleFromContent(content);
    job.content = content;

    std::ostringstream id;
    id << job.created_at_ms << "-" << job.hash.substr(0, 12) << "-" << GetCurrentProcessId() << "-"
       << g_job_counter.fetch_add(1);
    job.id = id.str();
    return job;
}

std::string JobToJson(const UploadJob &job)
{
    std::ostringstream oss;
    oss << "{\n"
        << "  \"id\":\"" << EscapeJson(job.id) << "\",\n"
        << "  \"created_at_ms\":" << job.created_at_ms << ",\n"
        << "  \"not_before_ms\":" << job.not_before_ms << ",\n"
        << "  \"attempts\":" << job.attempts << ",\n"
        << "  \"hash\":\"" << EscapeJson(job.hash) << "\",\n"
        << "  \"title\":\"" << EscapeJson(job.title) << "\",\n"
        << "  \"content\":\"" << EscapeJson(job.content) << "\",\n"
        << "  \"page_id\":\"" << EscapeJson(job.page_id) << "\",\n"
        << "  \"page_url\":\"" << EscapeJson(job.page_url) << "\",\n"
        << "  \"appended_block_count\":" << static_cast<unsigned long long>(job.appended_block_count) << ",\n"
        << "  \"last_error\":\"" << EscapeJson(job.last_error) << "\"\n"
        << "}\n";
    return oss.str();
}

std::uint64_t JsonNumberAsU64(const JsonValue *value, std::uint64_t fallback)
{
    if (value == nullptr)
    {
        return fallback;
    }
    if (value->is_number())
    {
        return static_cast<std::uint64_t>(value->as_number());
    }
    if (value->is_string())
    {
        return ParseU64OrDefault(value->as_string(), fallback);
    }
    return fallback;
}

std::size_t JsonNumberAsSize(const JsonValue *value, std::size_t fallback)
{
    return static_cast<std::size_t>(JsonNumberAsU64(value, static_cast<std::uint64_t>(fallback)));
}

int JsonNumberAsInt(const JsonValue *value, int fallback)
{
    return static_cast<int>(JsonNumberAsU64(value, static_cast<std::uint64_t>(fallback)));
}

std::string JsonStringOrEmpty(const JsonValue *value)
{
    if (value == nullptr || !value->is_string())
    {
        return "";
    }
    return value->as_string();
}

UploadJob JobFromJson(const std::string &text)
{
    const JsonValue json = ParseJson(text);
    if (!json.is_object())
    {
        throw std::runtime_error("任务文件不是 JSON object");
    }
    UploadJob job;
    job.id = JsonStringOrEmpty(json.find("id"));
    job.created_at_ms = JsonNumberAsU64(json.find("created_at_ms"), 0);
    job.not_before_ms = JsonNumberAsU64(json.find("not_before_ms"), 0);
    job.attempts = JsonNumberAsInt(json.find("attempts"), 0);
    job.hash = JsonStringOrEmpty(json.find("hash"));
    job.title = JsonStringOrEmpty(json.find("title"));
    job.content = JsonStringOrEmpty(json.find("content"));
    job.page_id = JsonStringOrEmpty(json.find("page_id"));
    job.page_url = JsonStringOrEmpty(json.find("page_url"));
    job.appended_block_count = JsonNumberAsSize(json.find("appended_block_count"), 0);
    job.last_error = JsonStringOrEmpty(json.find("last_error"));
    if (job.id.empty() || job.content.empty())
    {
        throw std::runtime_error("任务文件缺少 id 或 content");
    }
    return job;
}

class PersistentQueue
{
public:
    PersistentQueue(fs::path state_dir, int max_retry_attempts)
        : queue_dir_(std::move(state_dir) / L"queue"),
          failed_dir_(queue_dir_.parent_path() / L"failed"),
          max_retry_attempts_(max_retry_attempts)
    {
        fs::create_directories(queue_dir_);
        fs::create_directories(failed_dir_);
    }

    void Enqueue(const UploadJob &job)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AtomicWriteFile(JobPath(job.id), JobToJson(job));
    }

    void Update(const fs::path &path, const UploadJob &job)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        AtomicWriteFile(path, JobToJson(job));
    }

    void MarkSuccess(const fs::path &path)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::error_code ignored;
        fs::remove(path, ignored);
    }

    void MarkFailure(const fs::path &path, UploadJob job, const std::string &error, bool retryable,
                     int retry_after_seconds, Logger *logger)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        job.last_error = error;
        job.attempts += 1;

        if (!retryable || job.attempts > max_retry_attempts_)
        {
            const fs::path failed_path = failed_dir_ / path.filename();
            AtomicWriteFile(failed_path, JobToJson(job));
            std::error_code ignored;
            fs::remove(path, ignored);
            if (logger != nullptr)
            {
                logger->Error("任务移入 failed: " + job.id + "，原因: " + SummarizeForLog(error));
            }
            return;
        }

        job.not_before_ms = NowUnixMs() + ComputeBackoffMs(job.attempts, retry_after_seconds);
        AtomicWriteFile(path, JobToJson(job));
        if (logger != nullptr)
        {
            logger->Warn("任务稍后重试: " + job.id + "，attempt=" + std::to_string(job.attempts));
        }
    }

    std::optional<std::pair<UploadJob, fs::path>> NextDueJob(std::uint64_t now_ms, std::uint64_t *next_due_ms,
                                                             Logger *logger)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        *next_due_ms = 0;
        for (const fs::path &path : ListJobFiles())
        {
            UploadJob job;
            try
            {
                job = JobFromJson(ReadWholeFile(path));
            }
            catch (const std::exception &ex)
            {
                const fs::path failed_path = failed_dir_ / path.filename();
                std::error_code ignored;
                fs::rename(path, failed_path, ignored);
                if (logger != nullptr)
                {
                    logger->Error("任务文件损坏，已移动到 failed: " + WideToUtf8(path.filename().wstring()) +
                                  "，原因: " + ex.what());
                }
                continue;
            }

            if (job.not_before_ms == 0 || job.not_before_ms <= now_ms)
            {
                return std::make_pair(job, path);
            }
            if (*next_due_ms == 0 || job.not_before_ms < *next_due_ms)
            {
                *next_due_ms = job.not_before_ms;
            }
        }
        return std::nullopt;
    }

private:
    fs::path JobPath(const std::string &id) const
    {
        return queue_dir_ / (Utf8ToWide(id) + L".job");
    }

    std::vector<fs::path> ListJobFiles() const
    {
        std::vector<fs::path> files;
        if (!fs::exists(queue_dir_))
        {
            return files;
        }
        for (const fs::directory_entry &entry : fs::directory_iterator(queue_dir_))
        {
            if (entry.is_regular_file() && entry.path().extension() == L".job")
            {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    static std::uint64_t ComputeBackoffMs(int attempts, int retry_after_seconds)
    {
        if (retry_after_seconds > 0)
        {
            return static_cast<std::uint64_t>(retry_after_seconds) * 1000ull;
        }

        const int exponent = std::min(attempts, 10);
        const std::uint64_t base = 2000ull;
        const std::uint64_t max_delay = 15ull * 60ull * 1000ull;
        const std::uint64_t delay = std::min(max_delay, base << exponent);
        const std::uint64_t jitter = Fnv1a64(std::to_string(NowUnixMs())) % 1000ull;
        return delay + jitter;
    }

    fs::path queue_dir_;
    fs::path failed_dir_;
    int max_retry_attempts_ = 12;
    std::mutex mutex_;
};

class NotionClient
{
public:
    NotionClient(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger)
    {
        http_.SetToken(config_.notion_token);
    }

    void Validate()
    {
        EnsureMetadata();
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint)
    {
        EnsureMetadata();

        const std::vector<std::string> blocks = BuildTextBlocks(job->content);
        if (job->page_id.empty())
        {
            const auto created = CreatePage(*job);
            job->page_id = created.first;
            job->page_url = created.second;
            checkpoint();
            if (logger_ != nullptr)
            {
                logger_->Info("已创建 Notion 页面: " + job->id);
            }
        }

        if (job->appended_block_count > blocks.size())
        {
            job->appended_block_count = 0;
        }

        for (std::size_t begin = job->appended_block_count; begin < blocks.size();)
        {
            constexpr std::size_t kMaxAppendRequestBytes = 400ull * 1024ull;
            const std::size_t end =
                SelectAppendBatchEnd(blocks, begin, static_cast<std::size_t>(config_.append_batch_size),
                                     kMaxAppendRequestBytes);
            AppendBlocks(job->page_id, blocks, begin, end);
            job->appended_block_count = end;
            checkpoint();
            begin = end;
        }
    }

private:
    void EnsureMetadata()
    {
        if (!resolved_data_source_id_.empty() && !resolved_title_property_name_.empty())
        {
            return;
        }

        resolved_data_source_id_ = ResolveDataSourceId();
        ResolvePropertyMetadata(resolved_data_source_id_);
        if (logger_ != nullptr)
        {
            logger_->Info("Notion 目标已就绪: data_source_id=" + resolved_data_source_id_ +
                          ", title_property=" + resolved_title_property_name_ +
                          (resolved_created_time_date_property_name_.empty()
                               ? ""
                               : ", created_time_property=" + resolved_created_time_date_property_name_));
        }
    }

    std::string ResolveDataSourceId()
    {
        if (!config_.data_source_id.empty())
        {
            return CanonicalizeNotionId(config_.data_source_id);
        }
        if (config_.database_id.empty())
        {
            throw UploadFailure("缺少 data_source_id 或 database_id", false, 0);
        }

        const HttpResponse response = RequestWithRetry("GET", "/v1/databases/" + config_.database_id, "");
        const JsonValue json = ParseJson(response.body);
        const JsonValue *data_sources = json.find("data_sources");
        if (data_sources == nullptr || !data_sources->is_array() || data_sources->as_array().empty())
        {
            throw UploadFailure("数据库响应中没有 data_sources", false, 0);
        }
        const JsonValue *id = data_sources->as_array().front().find("id");
        if (id == nullptr || !id->is_string())
        {
            throw UploadFailure("数据库第一个 data_source 缺少 id", false, 0);
        }
        return CanonicalizeNotionId(id->as_string());
    }

    void ResolvePropertyMetadata(const std::string &data_source_id)
    {
        const HttpResponse response = RequestWithRetry("GET", "/v1/data_sources/" + data_source_id, "");
        const JsonValue json = ParseJson(response.body);
        const JsonValue *properties = json.find("properties");
        if (properties == nullptr || !properties->is_object())
        {
            throw UploadFailure("data_source 响应中没有 properties", false, 0);
        }

        bool configured_created_time_seen = false;
        std::string configured_created_time_type;
        for (const auto &item : properties->as_object())
        {
            const JsonValue *type = item.second.find("type");
            const std::string type_name = (type != nullptr && type->is_string()) ? type->as_string() : "";

            if (resolved_title_property_name_.empty() && !config_.title_property_name.empty() &&
                item.first == config_.title_property_name)
            {
                if (type_name != "title")
                {
                    throw UploadFailure("配置的 title_property_name 不是 title 类型: " + config_.title_property_name,
                                        false, 0);
                }
                resolved_title_property_name_ = item.first;
            }
            else if (resolved_title_property_name_.empty() && config_.title_property_name.empty() &&
                     type_name == "title")
            {
                resolved_title_property_name_ = item.first;
            }

            if (!config_.created_time_property_name.empty() && item.first == config_.created_time_property_name)
            {
                configured_created_time_seen = true;
                configured_created_time_type = type_name;
                if (type_name == "date")
                {
                    resolved_created_time_date_property_name_ = item.first;
                }
            }
        }

        if (resolved_title_property_name_.empty())
        {
            throw UploadFailure("data_source 中没有 title 类型属性", false, 0);
        }

        if (configured_created_time_seen && configured_created_time_type == "created_time" && logger_ != nullptr)
        {
            logger_->Info("创建时间属性是 Notion 内置 created_time 类型，将由 Notion 自动填写");
        }
        else if (!config_.created_time_property_name.empty() && configured_created_time_seen &&
                 configured_created_time_type != "date" && logger_ != nullptr)
        {
            logger_->Warn("创建时间属性不是 date 类型，已跳过自动填写: " + config_.created_time_property_name +
                          ", type=" + configured_created_time_type);
        }
    }

    std::pair<std::string, std::string> CreatePage(const UploadJob &job)
    {
        std::ostringstream body;
        body << "{\"parent\":{\"type\":\"data_source_id\",\"data_source_id\":\"" << EscapeJson(resolved_data_source_id_)
             << "\"},\"properties\":{\"" << EscapeJson(resolved_title_property_name_)
             << "\":{\"type\":\"title\",\"title\":[" << BuildTextRichText(job.title) << "]}";

        if (!config_.content_property_name.empty())
        {
            const std::string content_preview = TruncateUtf8(CollapseWhitespace(job.content),
                                                             static_cast<std::size_t>(config_.content_property_max_chars));
            body << ",\"" << EscapeJson(config_.content_property_name) << "\":{\"type\":\"rich_text\",\"rich_text\":["
                 << BuildTextRichText(content_preview) << "]}";
        }
        if (!resolved_created_time_date_property_name_.empty())
        {
            body << ",\"" << EscapeJson(resolved_created_time_date_property_name_)
                 << "\":{\"type\":\"date\",\"date\":{\"start\":\"" << IsoUtcTimestampFromUnixMs(job.created_at_ms)
                 << "\"}}";
        }

        body << "}}";

        const HttpResponse response = RequestWithRetry("POST", "/v1/pages", body.str());
        const JsonValue json = ParseJson(response.body);
        const JsonValue *id = json.find("id");
        if (id == nullptr || !id->is_string())
        {
            throw UploadFailure("创建页面响应缺少 id", true, 0);
        }
        const JsonValue *url = json.find("url");
        return {CanonicalizeNotionId(id->as_string()), (url != nullptr && url->is_string()) ? url->as_string() : ""};
    }

    void AppendBlocks(const std::string &page_id, const std::vector<std::string> &blocks, std::size_t begin,
                      std::size_t end)
    {
        std::ostringstream body;
        body << "{\"children\":[";
        for (std::size_t i = begin; i < end; ++i)
        {
            if (i != begin)
            {
                body << ",";
            }
            body << blocks[i];
        }
        body << "]}";
        RequestWithRetry("PATCH", "/v1/blocks/" + page_id + "/children", body.str());
    }

    HttpResponse RequestWithRetry(const std::string &method, const std::string &path, const std::string &body)
    {
        std::string last_error;
        int retry_after = 0;
        for (int attempt = 0; attempt <= config_.http_retry_attempts; ++attempt)
        {
            try
            {
                Throttle();
                const HttpResponse response = http_.Request(Utf8ToWide(method), Utf8ToWide(path), body);
                retry_after = response.retry_after_seconds;
                if (response.status_code >= 200 && response.status_code < 300)
                {
                    return response;
                }

                const bool retryable = response.status_code == 408 || response.status_code == 429 ||
                                       (response.status_code >= 500 && response.status_code <= 599);
                last_error = "HTTP " + std::to_string(response.status_code) + ": " + SummarizeForLog(response.body);
                if (!retryable)
                {
                    throw UploadFailure(last_error, false, retry_after);
                }
            }
            catch (const UploadFailure &)
            {
                throw;
            }
            catch (const std::exception &ex)
            {
                last_error = ex.what();
            }

            if (attempt < config_.http_retry_attempts)
            {
                const int delay_ms = retry_after > 0 ? retry_after * 1000 : std::min(30000, 500 * (1 << attempt));
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        throw UploadFailure(last_error.empty() ? "Notion 请求失败" : last_error, true, retry_after);
    }

    void Throttle()
    {
        if (config_.min_request_interval_ms <= 0)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(throttle_mutex_);
        const auto now = std::chrono::steady_clock::now();
        const auto next_allowed = last_request_at_ + std::chrono::milliseconds(config_.min_request_interval_ms);
        if (next_allowed > now)
        {
            std::this_thread::sleep_until(next_allowed);
        }
        last_request_at_ = std::chrono::steady_clock::now();
    }

    AppConfig config_;
    Logger *logger_ = nullptr;
    WinHttpClient http_;
    std::string resolved_data_source_id_;
    std::string resolved_title_property_name_;
    std::string resolved_created_time_date_property_name_;
    std::mutex throttle_mutex_;
    std::chrono::steady_clock::time_point last_request_at_;
};

class UploadWorker
{
public:
    UploadWorker(PersistentQueue *queue, NotionClient *notion, Logger *logger)
        : queue_(queue), notion_(notion), logger_(logger)
    {
    }

    void Start()
    {
        stop_.store(false);
        thread_ = std::thread([this]
                              { Run(); });
    }

    void Stop()
    {
        stop_.store(true);
        cv_.notify_all();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    void Notify()
    {
        cv_.notify_all();
    }

private:
    void Run()
    {
        if (logger_ != nullptr)
        {
            logger_->Info("上传线程已启动");
        }

        while (!stop_.load())
        {
            std::uint64_t next_due = 0;
            auto item = queue_->NextDueJob(NowUnixMs(), &next_due, logger_);
            if (!item.has_value())
            {
                WaitForNextJob(next_due);
                continue;
            }

            UploadJob job = item->first;
            const fs::path path = item->second;
            try
            {
                notion_->ProcessJob(&job, [&]
                                    { queue_->Update(path, job); });
                queue_->MarkSuccess(path);
                if (logger_ != nullptr)
                {
                    logger_->Info("上传成功: " + job.id + (job.page_url.empty() ? "" : " -> " + job.page_url));
                }
            }
            catch (const UploadFailure &ex)
            {
                queue_->MarkFailure(path, job, ex.what(), ex.retryable(), ex.retry_after_seconds(), logger_);
            }
            catch (const std::exception &ex)
            {
                queue_->MarkFailure(path, job, ex.what(), true, 0, logger_);
            }
        }

        if (logger_ != nullptr)
        {
            logger_->Info("上传线程已停止");
        }
    }

    void WaitForNextJob(std::uint64_t next_due)
    {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        if (next_due == 0)
        {
            cv_.wait_for(lock, std::chrono::seconds(30), [&]
                         { return stop_.load(); });
            return;
        }

        const std::uint64_t now = NowUnixMs();
        const std::uint64_t delay = next_due > now ? next_due - now : 0;
        cv_.wait_for(lock, std::chrono::milliseconds(delay), [&]
                     { return stop_.load(); });
    }

    PersistentQueue *queue_ = nullptr;
    NotionClient *notion_ = nullptr;
    Logger *logger_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::condition_variable cv_;
    std::mutex wait_mutex_;
};

class ClipboardReader
{
public:
    std::optional<std::string> ReadText(Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        const UINT html_format = RegisterClipboardFormatW(L"HTML Format");
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT) &&
            (html_format == 0 || !IsClipboardFormatAvailable(html_format)))
        {
            return std::nullopt;
        }

        bool opened = false;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            if (OpenClipboard(nullptr))
            {
                opened = true;
                break;
            }
            Sleep(static_cast<DWORD>(20 * (attempt + 1)));
        }

        if (!opened)
        {
            if (logger != nullptr)
            {
                logger->Warn("打开剪贴板失败: " + LastErrorMessage());
            }
            return std::nullopt;
        }

        struct ClipboardGuard
        {
            ~ClipboardGuard()
            {
                CloseClipboard();
            }
        } guard;

        const auto unicode_text = ReadUnicodeText(logger, max_clipboard_bytes);
        if (html_format != 0 && IsClipboardFormatAvailable(html_format))
        {
            const auto html_text = ReadHtmlText(html_format, logger, max_clipboard_bytes);
            if (html_text.has_value())
            {
                if (!HasEmptyMarkdownCodeFenceArtifact(*html_text))
                {
                    return html_text;
                }
                if (logger != nullptr)
                {
                    logger->Warn("HTML 剪贴板转换结果包含空代码块围栏，已回退到纯文本");
                }
            }
        }

        return unicode_text;
    }

private:
    std::optional<std::string> ReadHtmlText(UINT html_format, Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        HANDLE data = GetClipboardData(html_format);
        if (data == nullptr)
        {
            return std::nullopt;
        }

        const SIZE_T raw_bytes = GlobalSize(data);
        const std::uint64_t html_limit = std::min<std::uint64_t>(4ull * 1024ull * 1024ull, max_clipboard_bytes * 8ull);
        if (raw_bytes == 0 || raw_bytes > static_cast<SIZE_T>(html_limit))
        {
            if (logger != nullptr && raw_bytes > 0)
            {
                logger->Warn("HTML 剪贴板片段过大，回退到纯文本，bytes=" + std::to_string(raw_bytes));
            }
            return std::nullopt;
        }

        const char *raw = static_cast<const char *>(GlobalLock(data));
        if (raw == nullptr)
        {
            return std::nullopt;
        }
        std::string html(raw, raw + raw_bytes);
        GlobalUnlock(data);

        const auto fragment = ExtractCfHtmlFragment(html);
        if (!fragment.has_value())
        {
            return std::nullopt;
        }

        std::string converted = HtmlFragmentToMarkdown(*fragment);
        converted = Trim(converted);
        if (converted.empty())
        {
            return std::nullopt;
        }
        if (converted.size() > max_clipboard_bytes)
        {
            if (logger != nullptr)
            {
                logger->Warn("HTML 剪贴板转换后文本过大，已跳过，utf8_bytes=" + std::to_string(converted.size()));
            }
            return std::nullopt;
        }
        if (logger != nullptr)
        {
            logger->Info("已读取 HTML 剪贴板片段并转换为 Markdown-like 文本，bytes=" +
                         std::to_string(converted.size()));
        }
        return converted;
    }

    std::optional<std::string> ReadUnicodeText(Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data == nullptr)
        {
            return std::nullopt;
        }

        const SIZE_T raw_bytes = GlobalSize(data);
        if (raw_bytes > 0 && raw_bytes > static_cast<SIZE_T>(max_clipboard_bytes * 2))
        {
            if (logger != nullptr)
            {
                logger->Warn("剪贴板原始文本过大，已跳过，utf16_bytes=" + std::to_string(raw_bytes));
            }
            return std::nullopt;
        }

        const wchar_t *raw = static_cast<const wchar_t *>(GlobalLock(data));
        if (raw == nullptr)
        {
            return std::nullopt;
        }

        std::wstring wide(raw);
        GlobalUnlock(data);
        std::string utf8 = NormalizeLineEndings(WideToUtf8(wide));
        utf8 = Trim(utf8);
        if (utf8.empty())
        {
            return std::nullopt;
        }
        if (utf8.size() > max_clipboard_bytes)
        {
            if (logger != nullptr)
            {
                logger->Warn("剪贴板文本过大，已跳过，utf8_bytes=" + std::to_string(utf8.size()));
            }
            return std::nullopt;
        }
        return utf8;
    }
};

class TrayApplication
{
public:
    TrayApplication(const AppConfig *config, fs::path config_path, PersistentQueue *queue, UploadWorker *worker,
                    Logger *logger)
        : config_(config),
          config_path_(std::move(config_path)),
          queue_(queue),
          worker_(worker),
          logger_(logger),
          hotkey_spec_(ParseHotkeyOrThrow(config->hotkey)),
          hotkey_enabled_(config->enable_hotkey),
          notifications_enabled_(config->tray_notifications),
          clipboard_listener_enabled_(config->enable_clipboard_listener)
    {
    }

    int Run()
    {
        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        app_icon_ = CreateGeneratedAppIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        tray_icon_ = CreateGeneratedAppIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TrayApplication::WindowProc;
        wc.hInstance = instance;
        wc.hIcon = app_icon_ != nullptr ? app_icon_ : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = tray_icon_ != nullptr ? tray_icon_ : wc.hIcon;
        wc.lpszClassName = L"NotionClipboardWinTrayWindow";

        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, kAppDisplayName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr)
        {
            throw std::runtime_error("创建后台窗口失败: " + LastErrorMessage());
        }
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

        AddTrayIcon();
        if (hotkey_enabled_)
        {
            hotkey_enabled_ = RegisterUploadHotkey();
        }
        if (clipboard_listener_enabled_)
        {
            EnableClipboardListener(true);
        }

        if (config_->upload_initial_clipboard)
        {
            ProcessClipboard("启动读取", false);
        }

        if (logger_ != nullptr)
        {
            logger_->Info("托盘进程已启动，hotkey=" + hotkey_spec_.display +
                          "，clipboard_listener=" + (clipboard_listener_registered_ ? "on" : "off"));
        }
        ShowNotification(L"Notion Clipboard Win", L"后台进程已启动，按 " + Utf8ToWide(hotkey_spec_.display) + L" 上传剪贴板。");

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Cleanup();
        if (logger_ != nullptr)
        {
            logger_->Info("托盘进程已停止");
        }
        return 0;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        TrayApplication *self = nullptr;
        if (message == WM_NCCREATE)
        {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            self = static_cast<TrayApplication *>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<TrayApplication *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self == nullptr)
        {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return self->HandleMessage(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wparam, LPARAM lparam)
    {
        TrayApplication *self = recording_instance_;
        if (code == HC_ACTION && self != nullptr && self->recording_hotkey_ &&
            (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN))
        {
            const auto *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lparam);
            const UINT vk = static_cast<UINT>(info->vkCode);
            if (vk == VK_ESCAPE)
            {
                self->CancelHotkeyRecording();
                return 1;
            }
            if (IsModifierVirtualKey(vk))
            {
                return CallNextHookEx(self->keyboard_hook_, code, wparam, lparam);
            }

            const auto spec = HotkeySpecFromRecordedKey(CurrentHotkeyModifiers(), vk);
            if (spec.has_value())
            {
                self->ApplyRecordedHotkey(*spec);
                return 1;
            }

            self->ShowNotification(L"Notion Clipboard Win", L"热键需要包含 Ctrl/Alt/Shift/Win 和支持的主按键。");
            return 1;
        }
        return CallNextHookEx(self == nullptr ? nullptr : self->keyboard_hook_, code, wparam, lparam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == taskbar_created_message_)
        {
            tray_icon_added_ = false;
            AddTrayIcon();
            return 0;
        }

        switch (message)
        {
        case WM_HOTKEY:
            if (wparam == kUploadHotkeyId)
            {
                ProcessClipboard("热键", true);
            }
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wparam));
            return 0;
        case kTrayCallbackMessage:
            if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP)
            {
                ShowContextMenu();
            }
            else if (LOWORD(lparam) == WM_LBUTTONDBLCLK)
            {
                ProcessClipboard("托盘双击", true);
            }
            return 0;
        case WM_CLIPBOARDUPDATE:
            if (clipboard_listener_registered_)
            {
                SetTimer(hwnd, kClipboardDebounceTimer, static_cast<UINT>(config_->debounce_ms), nullptr);
            }
            return 0;
        case WM_TIMER:
            if (wparam == kClipboardDebounceTimer)
            {
                KillTimer(hwnd, kClipboardDebounceTimer);
                ProcessClipboard("剪贴板事件", false);
            }
            return 0;
        case WM_CLOSE:
            Cleanup();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    void AddTrayIcon()
    {
        if (hwnd_ == nullptr)
        {
            return;
        }

        nid_ = {};
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = kTrayIconId;
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid_.uCallbackMessage = kTrayCallbackMessage;
        nid_.hIcon =
            tray_icon_ != nullptr ? tray_icon_ : (app_icon_ != nullptr ? app_icon_ : LoadIconW(nullptr, IDI_APPLICATION));
        wcsncpy_s(nid_.szTip, kAppDisplayName, _TRUNCATE);

        const DWORD action = tray_icon_added_ ? NIM_MODIFY : NIM_ADD;
        if (!Shell_NotifyIconW(action, &nid_))
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("托盘图标更新失败: " + LastErrorMessage());
            }
            return;
        }

        if (!tray_icon_added_)
        {
            nid_.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid_);
        }
        tray_icon_added_ = true;
    }

    void ShowNotification(const std::wstring &title, const std::wstring &message)
    {
        if (!notifications_enabled_ || !tray_icon_added_)
        {
            return;
        }

        NOTIFYICONDATAW notify = nid_;
        notify.uFlags = NIF_INFO;
        notify.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(notify.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(notify.szInfo, message.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &notify);
    }

    bool RegisterUploadHotkey()
    {
        if (hwnd_ == nullptr)
        {
            return false;
        }
        if (hotkey_registered_)
        {
            UnregisterHotKey(hwnd_, kUploadHotkeyId);
            hotkey_registered_ = false;
        }

        const UINT modifiers = hotkey_spec_.modifiers | MOD_NOREPEAT;
        if (!RegisterHotKey(hwnd_, kUploadHotkeyId, modifiers, hotkey_spec_.vk))
        {
            if (logger_ != nullptr)
            {
                logger_->Error("注册全局热键失败: " + hotkey_spec_.display + "，原因: " + LastErrorMessage());
            }
            ShowNotification(L"Notion Clipboard Win", L"注册全局热键失败，可能已被其他程序占用。");
            return false;
        }

        hotkey_registered_ = true;
        if (logger_ != nullptr)
        {
            logger_->Info("全局热键已注册: " + hotkey_spec_.display);
        }
        return true;
    }

    void EnableClipboardListener(bool enabled)
    {
        if (hwnd_ == nullptr)
        {
            return;
        }

        if (enabled && !clipboard_listener_registered_)
        {
            if (!AddClipboardFormatListener(hwnd_))
            {
                if (logger_ != nullptr)
                {
                    logger_->Error("注册剪贴板监听失败: " + LastErrorMessage());
                }
                ShowNotification(L"Notion Clipboard Win", L"注册剪贴板监听失败。");
                clipboard_listener_enabled_ = false;
                return;
            }
            clipboard_listener_registered_ = true;
            clipboard_listener_enabled_ = true;
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板监听已启动");
            }
        }
        else if (!enabled && clipboard_listener_registered_)
        {
            RemoveClipboardFormatListener(hwnd_);
            clipboard_listener_registered_ = false;
            clipboard_listener_enabled_ = false;
            KillTimer(hwnd_, kClipboardDebounceTimer);
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板监听已停止");
            }
        }
        else
        {
            clipboard_listener_enabled_ = enabled;
        }
    }

    void ShowContextMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const std::wstring hotkey_label = L"热键: " + Utf8ToWide(hotkey_spec_.display);
        AppendMenuW(menu, MF_STRING, kMenuUploadNow, L"上传当前剪贴板");
        AppendMenuW(menu, MF_GRAYED, kMenuHotkeyStatus, hotkey_label.c_str());
        AppendMenuW(menu, MF_STRING | (hotkey_enabled_ ? MF_CHECKED : MF_UNCHECKED), kMenuToggleHotkey, L"启用热键");
        AppendMenuW(menu, MF_STRING | (recording_hotkey_ ? MF_GRAYED : MF_ENABLED), kMenuRecordHotkey, L"录制热键...");
        AppendMenuW(menu, MF_STRING | (notifications_enabled_ ? MF_CHECKED : MF_UNCHECKED), kMenuToggleNotifications,
                    L"显示通知");
        AppendMenuW(menu, MF_STRING | (clipboard_listener_enabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kMenuToggleClipboardListener, L"自动监听剪贴板");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuOpenConfig, L"打开配置");
        AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"查看日志");
        AppendMenuW(menu, MF_STRING, kMenuOpenStateDir, L"打开状态目录");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

        POINT cursor;
        GetCursorPos(&cursor);
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void HandleCommand(UINT command)
    {
        switch (command)
        {
        case kMenuUploadNow:
            ProcessClipboard("托盘菜单", true);
            break;
        case kMenuToggleHotkey:
            ToggleHotkey();
            break;
        case kMenuRecordHotkey:
            StartHotkeyRecording();
            break;
        case kMenuToggleNotifications:
            ToggleNotifications();
            break;
        case kMenuToggleClipboardListener:
            EnableClipboardListener(!clipboard_listener_enabled_);
            break;
        case kMenuOpenConfig:
            OpenConfig();
            break;
        case kMenuOpenLog:
            OpenPath(config_->state_dir / L"notion-clipboard-win.log");
            break;
        case kMenuOpenStateDir:
            OpenPath(config_->state_dir);
            break;
        case kMenuExit:
            Cleanup();
            DestroyWindow(hwnd_);
            break;
        default:
            break;
        }
    }

    void ToggleHotkey()
    {
        if (hotkey_enabled_)
        {
            if (hotkey_registered_)
            {
                UnregisterHotKey(hwnd_, kUploadHotkeyId);
                hotkey_registered_ = false;
            }
            hotkey_enabled_ = false;
            if (logger_ != nullptr)
            {
                logger_->Info("全局热键已暂停");
            }
            return;
        }

        hotkey_enabled_ = RegisterUploadHotkey();
    }

    bool PersistConfigValue(const std::string &key, const std::string &value)
    {
        try
        {
            UpsertConfigValue(config_path_, key, value);
            return true;
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("写入配置失败: " + key + "=" + value + "，原因: " + std::string(ex.what()));
            }
            return false;
        }
    }

    void StopHotkeyRecordingHook()
    {
        if (keyboard_hook_ != nullptr)
        {
            UnhookWindowsHookEx(keyboard_hook_);
            keyboard_hook_ = nullptr;
        }
        if (recording_instance_ == this)
        {
            recording_instance_ = nullptr;
        }
        recording_hotkey_ = false;
    }

    void StartHotkeyRecording()
    {
        if (recording_hotkey_)
        {
            return;
        }

        const int result = MessageBoxW(hwnd_,
                                       L"点击“确定”后按下新的全局热键。\n\n"
                                       L"要求：Ctrl/Alt/Shift/Win 至少一个修饰键 + 一个主按键。\n"
                                       L"按 Esc 取消录制。",
                                       L"录制热键", MB_OKCANCEL | MB_ICONINFORMATION | MB_TOPMOST);
        if (result != IDOK)
        {
            return;
        }

        restore_hotkey_enabled_after_recording_ = hotkey_enabled_;
        if (hotkey_registered_)
        {
            UnregisterHotKey(hwnd_, kUploadHotkeyId);
            hotkey_registered_ = false;
        }

        recording_hotkey_ = true;
        recording_instance_ = this;
        keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &TrayApplication::KeyboardHookProc,
                                           GetModuleHandleW(nullptr), 0);
        if (keyboard_hook_ == nullptr)
        {
            const std::string error = LastErrorMessage();
            StopHotkeyRecordingHook();
            if (restore_hotkey_enabled_after_recording_)
            {
                hotkey_enabled_ = RegisterUploadHotkey();
            }
            if (logger_ != nullptr)
            {
                logger_->Error("启动热键录制失败: " + error);
            }
            MessageBoxW(hwnd_, L"启动热键录制失败，请查看日志。", L"Notion Clipboard Win",
                        MB_OK | MB_ICONERROR | MB_TOPMOST);
            return;
        }

        if (logger_ != nullptr)
        {
            logger_->Info("开始录制全局热键");
        }
        ShowNotification(L"Notion Clipboard Win", L"正在录制热键，按 Esc 取消。");
    }

    void CancelHotkeyRecording()
    {
        StopHotkeyRecordingHook();
        if (restore_hotkey_enabled_after_recording_)
        {
            hotkey_enabled_ = RegisterUploadHotkey();
        }
        if (logger_ != nullptr)
        {
            logger_->Info("热键录制已取消");
        }
        ShowNotification(L"Notion Clipboard Win", L"热键录制已取消。");
    }

    void ApplyRecordedHotkey(const HotkeySpec &new_spec)
    {
        const HotkeySpec previous_spec = hotkey_spec_;
        const bool previous_enabled = restore_hotkey_enabled_after_recording_;
        StopHotkeyRecordingHook();

        hotkey_spec_ = new_spec;
        hotkey_enabled_ = true;
        if (!RegisterUploadHotkey())
        {
            hotkey_spec_ = previous_spec;
            hotkey_enabled_ = previous_enabled;
            if (previous_enabled)
            {
                hotkey_enabled_ = RegisterUploadHotkey();
            }
            MessageBoxW(hwnd_, L"新热键注册失败，可能已被其他程序占用；已恢复原热键。", L"Notion Clipboard Win",
                        MB_OK | MB_ICONWARNING | MB_TOPMOST);
            return;
        }

        PersistConfigValue("hotkey", hotkey_spec_.display);
        PersistConfigValue("enable_hotkey", "true");
        if (logger_ != nullptr)
        {
            logger_->Info("全局热键已更新: " + hotkey_spec_.display);
        }
        std::wstring message = L"热键已更新为 ";
        message += Utf8ToWide(hotkey_spec_.display);
        message += L"。";
        ShowNotification(L"Notion Clipboard Win", message);
    }

    void ToggleNotifications()
    {
        notifications_enabled_ = !notifications_enabled_;
        PersistConfigValue("tray_notifications", notifications_enabled_ ? "true" : "false");
        if (logger_ != nullptr)
        {
            logger_->Info(std::string("托盘通知已") + (notifications_enabled_ ? "启用" : "关闭"));
        }
        if (notifications_enabled_)
        {
            ShowNotification(L"Notion Clipboard Win", L"托盘通知已启用。");
        }
    }

    void OpenConfig()
    {
        if (!fs::exists(config_path_))
        {
            try
            {
                const fs::path parent = config_path_.parent_path();
                if (!parent.empty())
                {
                    fs::create_directories(parent);
                }
                AtomicWriteFile(config_path_, "notion_token=\n"
                                             "data_source_id=\n"
                                             "database_id=\n"
                                             "hotkey=Ctrl+Shift+B\n"
                                             "enable_hotkey=true\n"
                                             "enable_clipboard_listener=false\n"
                                             "tray_notifications=true\n");
            }
            catch (const std::exception &ex)
            {
                if (logger_ != nullptr)
                {
                    logger_->Warn("创建配置文件失败: " + std::string(ex.what()));
                }
            }
        }
        OpenPath(config_path_);
    }

    void OpenPath(const fs::path &path)
    {
        const std::wstring target = path.wstring();
        HINSTANCE result = ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("打开路径失败: " + WideToUtf8(target));
            }
            ShowNotification(L"Notion Clipboard Win", L"打开路径失败。");
        }
    }

    void ProcessClipboard(const char *trigger, bool user_initiated)
    {
        const auto text = reader_.ReadText(logger_, config_->max_clipboard_bytes);
        if (!text.has_value())
        {
            if (user_initiated)
            {
                ShowNotification(L"Notion Clipboard Win", L"当前剪贴板没有可上传的文本。");
            }
            return;
        }

        UploadJob job = MakeUploadJob(*text);
        const std::uint64_t now_ms = NowUnixMs();
        if (config_->duplicate_suppression_ms > 0 && job.hash == last_hash_ &&
            now_ms - last_hash_at_ms_ <= static_cast<std::uint64_t>(config_->duplicate_suppression_ms))
        {
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板内容重复，已忽略");
            }
            if (user_initiated)
            {
                ShowNotification(L"Notion Clipboard Win", L"短时间内相同内容已忽略。");
            }
            return;
        }

        queue_->Enqueue(job);
        last_hash_ = job.hash;
        last_hash_at_ms_ = now_ms;
        worker_->Notify();
        if (logger_ != nullptr)
        {
            logger_->Info(std::string(trigger) + "已入队剪贴板内容: " + job.id +
                          "，bytes=" + std::to_string(text->size()));
        }
        if (user_initiated)
        {
            ShowNotification(L"Notion Clipboard Win", L"剪贴板内容已加入上传队列。");
        }
    }

    void Cleanup()
    {
        if (cleaned_up_)
        {
            return;
        }
        cleaned_up_ = true;
        StopHotkeyRecordingHook();

        if (hwnd_ != nullptr)
        {
            if (clipboard_listener_registered_)
            {
                RemoveClipboardFormatListener(hwnd_);
                clipboard_listener_registered_ = false;
            }
            if (hotkey_registered_)
            {
                UnregisterHotKey(hwnd_, kUploadHotkeyId);
                hotkey_registered_ = false;
            }
        }

        if (tray_icon_added_)
        {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            tray_icon_added_ = false;
        }
        if (tray_icon_ != nullptr)
        {
            DestroyIcon(tray_icon_);
            tray_icon_ = nullptr;
        }
        if (app_icon_ != nullptr)
        {
            DestroyIcon(app_icon_);
            app_icon_ = nullptr;
        }
    }

    const AppConfig *config_ = nullptr;
    fs::path config_path_;
    PersistentQueue *queue_ = nullptr;
    UploadWorker *worker_ = nullptr;
    Logger *logger_ = nullptr;
    ClipboardReader reader_;
    HotkeySpec hotkey_spec_;
    std::string last_hash_;
    std::uint64_t last_hash_at_ms_ = 0;
    HWND hwnd_ = nullptr;
    HICON app_icon_ = nullptr;
    HICON tray_icon_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    UINT taskbar_created_message_ = 0;
    bool hotkey_enabled_ = true;
    bool hotkey_registered_ = false;
    bool notifications_enabled_ = true;
    bool clipboard_listener_enabled_ = false;
    bool clipboard_listener_registered_ = false;
    bool tray_icon_added_ = false;
    bool cleaned_up_ = false;
    bool recording_hotkey_ = false;
    bool restore_hotkey_enabled_after_recording_ = false;
    HHOOK keyboard_hook_ = nullptr;
    static TrayApplication *recording_instance_;
};

TrayApplication *TrayApplication::recording_instance_ = nullptr;

DWORD g_main_thread_id = 0;

BOOL WINAPI ConsoleCtrlHandler(DWORD control_type)
{
    switch (control_type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_main_thread_id != 0)
        {
            PostThreadMessageW(g_main_thread_id, WM_QUIT, 0, 0);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

void ValidateConfigOrThrow(const AppConfig &config)
{
    if (config.notion_token.empty())
    {
        throw std::runtime_error("缺少 Notion token，请在配置里设置 notion_token 或设置 NOTION_TOKEN");
    }
    if (config.data_source_id.empty() && config.database_id.empty())
    {
        throw std::runtime_error("缺少 data_source_id 或 database_id");
    }
}

int RunOnce(const AppConfig &config, PersistentQueue *queue, NotionClient *notion, Logger *logger, bool dry_run)
{
    ClipboardReader reader;
    const auto text = reader.ReadText(logger, config.max_clipboard_bytes);
    if (!text.has_value())
    {
        throw std::runtime_error("当前剪贴板没有可上传的文本");
    }

    UploadJob job = MakeUploadJob(*text);
    if (dry_run)
    {
        const std::vector<std::string> blocks = BuildTextBlocks(job.content);
        const auto equation_count = std::count_if(blocks.begin(), blocks.end(), [](const std::string &block)
                                                  { return block.find("\"type\":\"equation\"") != std::string::npos; });
        const auto code_count = std::count_if(blocks.begin(), blocks.end(), [](const std::string &block)
                                              { return block.find("\"type\":\"code\"") != std::string::npos; });
        logger->Info("dry-run: title=" + job.title + "，bytes=" + std::to_string(text->size()) +
                     "，blocks=" + std::to_string(blocks.size()) +
                     "，equations=" + std::to_string(static_cast<std::size_t>(equation_count)) +
                     "，code_blocks=" + std::to_string(static_cast<std::size_t>(code_count)));
        return 0;
    }

    try
    {
        notion->ProcessJob(&job, [] {});
        logger->Info("上传成功: " + job.id + (job.page_url.empty() ? "" : " -> " + job.page_url));
        return 0;
    }
    catch (const UploadFailure &ex)
    {
        job.last_error = ex.what();
        queue->Enqueue(job);
        logger->Error("单次上传失败，任务已保存到队列: " + std::string(ex.what()));
        return ex.retryable() ? 2 : 3;
    }
}

int AppMain(int argc, wchar_t **argv)
{
#ifndef NOTION_CLIPBOARD_WIN_GUI
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    g_main_thread_id = GetCurrentThreadId();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    MSG bootstrap_msg;
    PeekMessageW(&bootstrap_msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const CliOptions cli = ParseCli(argc, argv);
    if (cli.help)
    {
        PrintHelp();
        return 0;
    }
    if (cli.self_test)
    {
        return RunSelfTest();
    }
    if (!cli.dry_run_file_path.empty())
    {
        return RunDryRunText(ReadWholeFile(cli.dry_run_file_path));
    }

    AppConfig config = LoadConfig(cli.config_path);
    ValidateConfigOrThrow(config);
    fs::create_directories(config.state_dir);

#ifdef NOTION_CLIPBOARD_WIN_GUI
    const bool mirror_console = false;
#else
    const bool mirror_console = true;
#endif
    Logger logger(config.state_dir / L"notion-clipboard-win.log", mirror_console);
    logger.Info("程序启动，config=" + WideToUtf8(cli.config_path.wstring()));

    PersistentQueue queue(config.state_dir, config.max_retry_attempts);
    NotionClient notion(config, &logger);

    if (cli.validate_config)
    {
        notion.Validate();
        logger.Info("配置验证通过");
        return 0;
    }

    if (cli.once)
    {
        return RunOnce(config, &queue, &notion, &logger, cli.dry_run);
    }

    UploadWorker worker(&queue, &notion, &logger);
    worker.Start();
    int code = 0;
    try
    {
        TrayApplication app(&config, cli.config_path, &queue, &worker, &logger);
        code = app.Run();
    }
    catch (...)
    {
        worker.Stop();
        throw;
    }
    worker.Stop();
    return code;
}
} // 命名空间

#ifdef NOTION_CLIPBOARD_WIN_GUI
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try
    {
        return AppMain(argc, argv);
    }
    catch (const std::exception &ex)
    {
        MessageBoxW(nullptr, Utf8ToWide(ex.what()).c_str(), L"Notion Clipboard Win", MB_ICONERROR | MB_OK);
        return 1;
    }
}
#else
int wmain(int argc, wchar_t **argv)
{
    try
    {
        return AppMain(argc, argv);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "错误: " << ex.what() << "\n";
        return 1;
    }
}
#endif
