#include "util.h"

#include "win_util.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ncw
{
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
}
