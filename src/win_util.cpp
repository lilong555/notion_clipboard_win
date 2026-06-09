#include "win_util.h"

#include <stdexcept>

namespace ncw
{
std::wstring Utf8ToWide(const std::string &input)
{
    if (input.empty())
    {
        return L"";
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
                                         static_cast<int>(input.size()), nullptr, 0);
    if (size <= 0)
    {
        throw std::runtime_error("UTF-8 转 UTF-16 失败");
    }
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(),
                        size);
    return output;
}

std::string WideToUtf8(const std::wstring &input)
{
    if (input.empty())
    {
        return "";
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr,
                                         nullptr);
    if (size <= 0)
    {
        throw std::runtime_error("UTF-16 转 UTF-8 失败");
    }
    std::string output(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), output.data(), size, nullptr,
                        nullptr);
    return output;
}

std::string LastErrorMessage(DWORD error)
{
    LPWSTR raw = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
    if (size == 0 || raw == nullptr)
    {
        return "Windows error " + std::to_string(error);
    }
    std::wstring wide(raw, raw + size);
    LocalFree(raw);
    while (!wide.empty() && (wide.back() == L'\r' || wide.back() == L'\n' || wide.back() == L'.'))
    {
        wide.pop_back();
    }
    return WideToUtf8(wide);
}
}
