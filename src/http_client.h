#pragma once

#include <windows.h>
#include <winhttp.h>

#include <string>

namespace ncw
{
struct HttpResponse
{
    long status_code = 0;
    std::string body;
    int retry_after_seconds = 0;
};

class ScopedInternetHandle
{
public:
    ScopedInternetHandle() = default;
    explicit ScopedInternetHandle(HINTERNET handle);
    ~ScopedInternetHandle();

    ScopedInternetHandle(const ScopedInternetHandle &) = delete;
    ScopedInternetHandle &operator=(const ScopedInternetHandle &) = delete;

    HINTERNET get() const;
    void Reset(HINTERNET handle);

private:
    HINTERNET handle_ = nullptr;
};

class WinHttpClient
{
public:
    WinHttpClient();

    HttpResponse Request(const std::wstring &method, const std::wstring &path, const std::string &body) const;
    void SetToken(std::string token);

private:
    static int ReadRetryAfterSeconds(HINTERNET request);

    ScopedInternetHandle session_;
    std::string notion_token_;
};
}
