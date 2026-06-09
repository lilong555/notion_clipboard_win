#include "http_client.h"

#include "win_util.h"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ncw
{
namespace
{
constexpr const char *kNotionVersion = "2026-03-11";
constexpr const wchar_t *kNotionHost = L"api.notion.com";
constexpr int kNotionPort = INTERNET_DEFAULT_HTTPS_PORT;
}

ScopedInternetHandle::ScopedInternetHandle(HINTERNET handle) : handle_(handle) {}

ScopedInternetHandle::~ScopedInternetHandle()
{
    Reset(nullptr);
}

HINTERNET ScopedInternetHandle::get() const
{
    return handle_;
}

void ScopedInternetHandle::Reset(HINTERNET handle)
{
    if (handle_ != nullptr)
    {
        WinHttpCloseHandle(handle_);
    }
    handle_ = handle;
}

WinHttpClient::WinHttpClient()
{
    session_.Reset(WinHttpOpen(L"notion-clipboard-win/0.1",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS,
                               0));
    if (session_.get() == nullptr)
    {
        throw std::runtime_error("WinHTTP 初始化失败: " + LastErrorMessage());
    }
}

HttpResponse WinHttpClient::Request(const std::wstring &method, const std::wstring &path, const std::string &body) const
{
    ScopedInternetHandle connect(WinHttpConnect(session_.get(), kNotionHost, kNotionPort, 0));
    if (connect.get() == nullptr)
    {
        throw std::runtime_error("连接 Notion 失败: " + LastErrorMessage());
    }

    const wchar_t *accept_types[] = {L"application/json", nullptr};
    ScopedInternetHandle request(WinHttpOpenRequest(connect.get(), method.c_str(), path.c_str(), nullptr,
                                                    WINHTTP_NO_REFERER, accept_types, WINHTTP_FLAG_SECURE));
    if (request.get() == nullptr)
    {
        throw std::runtime_error("创建 HTTP 请求失败: " + LastErrorMessage());
    }

    WinHttpSetTimeouts(request.get(), 10000, 15000, 30000, 90000);

    std::wstring headers = L"Authorization: Bearer ";
    headers += Utf8ToWide(notion_token_);
    headers += L"\r\nNotion-Version: ";
    headers += Utf8ToWide(kNotionVersion);
    headers += L"\r\nContent-Type: application/json\r\n";

    LPVOID request_body = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char *>(body.data());
    const DWORD body_size = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(request.get(), headers.c_str(), static_cast<DWORD>(headers.size()), request_body,
                            body_size, body_size, 0))
    {
        throw std::runtime_error("发送 HTTP 请求失败: " + LastErrorMessage());
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr))
    {
        throw std::runtime_error("接收 HTTP 响应失败: " + LastErrorMessage());
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size, WINHTTP_NO_HEADER_INDEX))
    {
        throw std::runtime_error("读取 HTTP 状态码失败: " + LastErrorMessage());
    }

    std::string response_body;
    while (true)
    {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available))
        {
            throw std::runtime_error("读取 HTTP 响应长度失败: " + LastErrorMessage());
        }
        if (available == 0)
        {
            break;
        }
        std::vector<char> buffer(available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), buffer.data(), available, &read))
        {
            throw std::runtime_error("读取 HTTP 响应体失败: " + LastErrorMessage());
        }
        response_body.append(buffer.data(), read);
    }

    return {static_cast<long>(status_code), response_body, ReadRetryAfterSeconds(request.get())};
}

void WinHttpClient::SetToken(std::string token)
{
    notion_token_ = std::move(token);
}

int WinHttpClient::ReadRetryAfterSeconds(HINTERNET request)
{
    DWORD size = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Retry-After", WINHTTP_NO_OUTPUT_BUFFER, &size,
                            WINHTTP_NO_HEADER_INDEX))
    {
        return 0;
    }
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
    {
        return 0;
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Retry-After", value.data(), &size,
                             WINHTTP_NO_HEADER_INDEX))
    {
        return 0;
    }
    while (!value.empty() && value.back() == L'\0')
    {
        value.pop_back();
    }

    try
    {
        return std::max(0, std::stoi(value));
    }
    catch (...)
    {
        return 0;
    }
}
}
