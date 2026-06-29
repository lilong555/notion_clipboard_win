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

    HttpResponse RequestJsonUrl(const std::wstring &method, const std::string &url, const std::wstring &extra_headers,
                                const std::string &body, const std::string &connection_name = "URL") const;

private:
    static int ReadRetryAfterSeconds(HINTERNET request);
    HttpResponse SendJsonRequest(const std::wstring &method, const std::wstring &host, INTERNET_PORT port,
                                 const std::wstring &path, bool secure, const std::wstring &extra_headers,
                                 const std::string &body, const std::string &connection_name) const;

    ScopedInternetHandle session_;
};
}
