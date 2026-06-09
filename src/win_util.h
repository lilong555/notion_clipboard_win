#pragma once

#include <windows.h>

#include <string>

namespace ncw
{
std::wstring Utf8ToWide(const std::string &input);
std::string WideToUtf8(const std::wstring &input);
std::string LastErrorMessage(DWORD error = GetLastError());
}
