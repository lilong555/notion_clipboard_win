#include "autostart.h"

#include "util.h"
#include "win_util.h"

#include <windows.h>

#include <string>

namespace ncw
{
namespace
{
constexpr const wchar_t *kRunKeyPath = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t *kRunValueName = L"NotionClipboardWin";

std::wstring QuoteWindowsArg(const std::wstring &arg)
{
    std::wstring output = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : arg)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (ch == L'"')
        {
            output.append(backslashes * 2 + 1, L'\\');
            output.push_back(ch);
            backslashes = 0;
            continue;
        }
        output.append(backslashes, L'\\');
        backslashes = 0;
        output.push_back(ch);
    }
    output.append(backslashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

std::wstring ReadRunValue()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
    {
        return L"";
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LONG result = RegQueryValueExW(key, kRunValueName, nullptr, &type, nullptr, &bytes);
    if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0)
    {
        RegCloseKey(key);
        return L"";
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    result = RegQueryValueExW(key, kRunValueName, nullptr, nullptr, reinterpret_cast<LPBYTE>(value.data()), &bytes);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
    {
        return L"";
    }

    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }
    return value;
}
}

std::wstring BuildAutoStartCommand(const std::filesystem::path &config_path)
{
    const std::filesystem::path absolute_config_path = std::filesystem::absolute(config_path);
    return QuoteWindowsArg(ModuleDirectory() / L"notion_clipboard_win.exe") + L" --config " +
           QuoteWindowsArg(absolute_config_path);
}

bool IsAutoStartEnabled(const std::filesystem::path &config_path)
{
    return ReadRunValue() == BuildAutoStartCommand(config_path);
}

bool SetAutoStartEnabled(bool enabled, const std::filesystem::path &config_path, std::string *error)
{
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS)
    {
        if (error != nullptr)
        {
            *error = "打开开机启动注册表项失败: " + LastErrorMessage(static_cast<DWORD>(result));
        }
        return false;
    }

    if (enabled)
    {
        const std::wstring command = BuildAutoStartCommand(config_path);
        result = RegSetValueExW(key, kRunValueName, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        result = RegDeleteValueW(key, kRunValueName);
        if (result == ERROR_FILE_NOT_FOUND)
        {
            result = ERROR_SUCCESS;
        }
    }
    RegCloseKey(key);

    if (result != ERROR_SUCCESS)
    {
        if (error != nullptr)
        {
            *error = std::string(enabled ? "写入开机启动失败: " : "删除开机启动失败: ") +
                     LastErrorMessage(static_cast<DWORD>(result));
        }
        return false;
    }
    return true;
}
}
