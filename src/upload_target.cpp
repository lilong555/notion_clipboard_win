#include "upload_target.h"

#include "config.h"
#include "converter.h"
#include "http_client.h"
#include "json.h"
#include "logger.h"
#include "queue.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace ncw
{
namespace
{
constexpr const char *kNotionApiBaseUrl = "https://api.notion.com";
constexpr const char *kNotionVersion = "2026-03-11";

void EnsureJobTarget(UploadJob *job, const std::string &target)
{
    if (job->target.empty())
    {
        job->target = target;
        return;
    }
    if (job->target != target)
    {
        throw UploadFailure("任务目标是 " + job->target + "，当前后端是 " + target, false, 0);
    }
}

std::string BuildMarkdownDocument(const UploadJob &job)
{
    std::ostringstream content;
    content << "# " << job.title << "\n\n";
    content << "<!-- source: notion_clipboard_win; job_id: " << job.id << "; created_at: "
            << IsoUtcTimestampFromUnixMs(job.created_at_ms) << " -->\n\n";
    content << NormalizeLineEndings(job.content);
    if (job.content.empty() || job.content.back() != '\n')
    {
        content << "\n";
    }
    return content.str();
}

bool IsRetryableHttpStatus(long status_code)
{
    return status_code == 408 || status_code == 429 || (status_code >= 500 && status_code <= 599);
}

int ComputeHttpRetryDelayMs(int retry_after_seconds, int attempt)
{
    return retry_after_seconds > 0 ? retry_after_seconds * 1000 : std::min(30000, 500 * (1 << attempt));
}

std::string JsonScalarForMessage(const JsonValue *value)
{
    if (value == nullptr)
    {
        return "";
    }
    if (value->is_string())
    {
        return value->as_string();
    }
    if (value->is_number())
    {
        std::ostringstream stream;
        stream << static_cast<long long>(value->as_number());
        return stream.str();
    }
    if (value->is_bool())
    {
        return value->as_bool() ? "true" : "false";
    }
    return "";
}

std::string FirstJsonStringLike(const JsonValue &json, const std::vector<std::string> &keys)
{
    for (const std::string &key : keys)
    {
        const std::string value = JsonScalarForMessage(json.find(key));
        if (!value.empty())
        {
            return value;
        }
    }
    return "";
}

void EnsureBusinessCodeOk(const std::string &service_name, const std::string &body)
{
    JsonValue json;
    try
    {
        json = ParseJson(body);
    }
    catch (const std::exception &ex)
    {
        throw UploadFailure(service_name + " 响应不是有效 JSON: " + std::string(ex.what()), true, 0);
    }

    if (!json.is_object())
    {
        throw UploadFailure(service_name + " 响应不是 JSON 对象", true, 0);
    }

    const JsonValue *code = json.find("code");
    if (code == nullptr)
    {
        code = json.find("errcode");
    }
    if (code == nullptr)
    {
        return;
    }

    bool ok = false;
    if (code->is_number())
    {
        ok = static_cast<long long>(code->as_number()) == 0;
    }
    else if (code->is_string())
    {
        const std::string normalized = ToLowerAscii(Trim(code->as_string()));
        ok = normalized.empty() || normalized == "0" || normalized == "ok" || normalized == "success";
    }

    if (ok)
    {
        return;
    }

    const std::string message =
        FirstJsonStringLike(json, {"msg", "message", "error", "error_description", "errmsg"});
    std::string error = service_name + " API 返回业务错误 code=" + JsonScalarForMessage(code);
    if (!message.empty())
    {
        error += ": " + message;
    }
    else
    {
        error += ": " + SummarizeForLog(body);
    }
    throw UploadFailure(error, false, 0);
}

class NotionClient : public UploadTarget
{
public:
    NotionClient(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger) {}

    std::string Name() const override
    {
        return "notion";
    }

    void Validate() override
    {
        EnsureMetadata();
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        EnsureMetadata();

        const std::vector<std::string> blocks = BuildTextBlocks(job->content);
        if (job->remote_id.empty())
        {
            const auto created = CreatePage(*job);
            job->remote_id = created.first;
            job->remote_url = created.second;
            checkpoint();
            if (logger_ != nullptr)
            {
                logger_->Info("已创建 Notion 页面: " + job->id);
            }
        }

        if (job->remote_progress > blocks.size())
        {
            job->remote_progress = 0;
        }

        for (std::size_t begin = job->remote_progress; begin < blocks.size();)
        {
            constexpr std::size_t kMaxAppendRequestBytes = 400ull * 1024ull;
            const std::size_t end =
                SelectAppendBatchEnd(blocks, begin, static_cast<std::size_t>(config_.append_batch_size),
                                     kMaxAppendRequestBytes);
            AppendBlocks(job->remote_id, blocks, begin, end);
            job->remote_progress = end;
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
            const std::string content_preview =
                TruncateUtf8(StripNonMathDollarMarkersForPlainText(CollapseWhitespace(job.content)),
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

    void AppendBlocks(const std::string &parent_block_id, const std::vector<std::string> &blocks, std::size_t begin,
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
        RequestWithRetry("PATCH", "/v1/blocks/" + parent_block_id + "/children", body.str());
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
                std::wstring headers;
                headers += L"Authorization: Bearer ";
                headers += Utf8ToWide(config_.notion_token);
                headers += L"\r\nNotion-Version: ";
                headers += Utf8ToWide(kNotionVersion);
                headers += L"\r\n";
                const HttpResponse response =
                    http_.RequestJsonUrl(Utf8ToWide(method), std::string(kNotionApiBaseUrl) + path, headers, body,
                                         "Notion");
                retry_after = response.retry_after_seconds;
                if (response.status_code >= 200 && response.status_code < 300)
                {
                    return response;
                }

                const bool retryable = IsRetryableHttpStatus(response.status_code);
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
                const int delay_ms = ComputeHttpRetryDelayMs(retry_after, attempt);
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

std::wstring SanitizeMarkdownFileStem(std::wstring name)
{
    for (wchar_t &ch : name)
    {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'/' || ch == L'\\' ||
            ch == L'|' || ch == L'?' || ch == L'*')
        {
            ch = L'_';
        }
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
    {
        name.pop_back();
    }
    if (name.empty())
    {
        name = L"clipboard";
    }
    constexpr std::size_t kMaxStemChars = 80;
    if (name.size() > kMaxStemChars)
    {
        name.resize(kMaxStemChars);
        while (!name.empty() && (name.back() == L'.' || name.back() == L' '))
        {
            name.pop_back();
        }
    }
    return name.empty() ? L"clipboard" : name;
}

class MarkdownFileTarget : public UploadTarget
{
public:
    MarkdownFileTarget(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger)
    {
        if (config_.markdown_output_dir.empty())
        {
            config_.markdown_output_dir = config_.state_dir / L"markdown";
        }
    }

    std::string Name() const override
    {
        return "markdown_file";
    }

    void Validate() override
    {
        fs::create_directories(config_.markdown_output_dir);
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        if (job->remote_progress > 0 && !job->remote_url.empty())
        {
            return;
        }

        fs::create_directories(config_.markdown_output_dir);
        const fs::path output_path = BuildOutputPath(*job);
        AtomicWriteFile(output_path, BuildMarkdownDocument(*job));
        job->remote_id = WideToUtf8(output_path.wstring());
        job->remote_url = job->remote_id;
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("已写入 Markdown 文件: " + job->remote_url);
        }
    }

private:
    fs::path BuildOutputPath(const UploadJob &job) const
    {
        const std::wstring stem = SanitizeMarkdownFileStem(Utf8ToWide(job.title));
        const std::wstring suffix = Utf8ToWide(job.id);
        return config_.markdown_output_dir / (suffix + L"-" + stem + L".md");
    }

    AppConfig config_;
    Logger *logger_ = nullptr;
};

std::string BuildWebhookPayload(const UploadJob &job)
{
    std::ostringstream body;
    body << "{"
         << "\"source\":\"notion_clipboard_win\","
         << "\"target\":\"webhook\","
         << "\"id\":\"" << EscapeJson(job.id) << "\","
         << "\"title\":\"" << EscapeJson(job.title) << "\","
         << "\"hash\":\"" << EscapeJson(job.hash) << "\","
         << "\"created_at_ms\":" << job.created_at_ms << ","
         << "\"created_at\":\"" << IsoUtcTimestampFromUnixMs(job.created_at_ms) << "\","
         << "\"content_format\":\"markdown\","
         << "\"content\":\"" << EscapeJson(NormalizeLineEndings(job.content)) << "\""
         << "}";
    return body.str();
}

class WebhookTarget : public UploadTarget
{
public:
    WebhookTarget(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger) {}

    std::string Name() const override
    {
        return "webhook";
    }

    void Validate() override
    {
        ValidateConfigOrThrow(config_);
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        if (job->remote_progress > 0 && !job->remote_url.empty())
        {
            return;
        }

        const HttpResponse response = PostWithRetry(BuildWebhookPayload(*job));
        std::pair<std::string, std::string> ids = ExtractWebhookResponseIds(response.body);
        job->remote_id = ids.first.empty() ? job->id : ids.first;
        job->remote_url = ids.second.empty() ? config_.webhook_url : ids.second;
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("Webhook 上传成功: " + job->id + " -> " + job->remote_url);
        }
    }

private:
    HttpResponse PostWithRetry(const std::string &body)
    {
        std::string last_error;
        int retry_after = 0;
        for (int attempt = 0; attempt <= config_.http_retry_attempts; ++attempt)
        {
            try
            {
                Throttle();
                std::wstring headers;
                if (!config_.webhook_bearer_token.empty())
                {
                    headers += L"Authorization: Bearer ";
                    headers += Utf8ToWide(config_.webhook_bearer_token);
                    headers += L"\r\n";
                }
                const HttpResponse response =
                    http_.RequestJsonUrl(L"POST", config_.webhook_url, headers, body, "webhook");
                retry_after = response.retry_after_seconds;
                if (response.status_code >= 200 && response.status_code < 300)
                {
                    return response;
                }

                const bool retryable = IsRetryableHttpStatus(response.status_code);
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
                const int delay_ms = ComputeHttpRetryDelayMs(retry_after, attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        throw UploadFailure(last_error.empty() ? "Webhook 请求失败" : last_error, true, retry_after);
    }

    static std::pair<std::string, std::string> ExtractWebhookResponseIds(const std::string &body)
    {
        try
        {
            const JsonValue json = ParseJson(body);
            if (!json.is_object())
            {
                return {};
            }
            const JsonValue *id = json.find("id");
            const JsonValue *url = json.find("url");
            return {(id != nullptr && id->is_string()) ? id->as_string() : "",
                    (url != nullptr && url->is_string()) ? url->as_string() : ""};
        }
        catch (...)
        {
            return {};
        }
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
    std::mutex throttle_mutex_;
    std::chrono::steady_clock::time_point last_request_at_;
};

std::string SanitizePortableFileStem(const std::string &name)
{
    std::string output;
    output.reserve(name.size());
    bool previous_dash = false;
    for (unsigned char ch : name)
    {
        if (std::isalnum(ch) || ch == '_' || ch == '.')
        {
            output.push_back(static_cast<char>(ch));
            previous_dash = false;
        }
        else if (ch == '-' || std::isspace(ch))
        {
            if (!previous_dash && !output.empty())
            {
                output.push_back('-');
                previous_dash = true;
            }
        }
        else if (!previous_dash && !output.empty())
        {
            output.push_back('-');
            previous_dash = true;
        }
    }
    while (!output.empty() && (output.back() == '-' || output.back() == '.' || output.back() == ' '))
    {
        output.pop_back();
    }
    while (!output.empty() && (output.front() == '-' || output.front() == '.' || output.front() == ' '))
    {
        output.erase(output.begin());
    }
    if (output.empty())
    {
        output = "clipboard";
    }
    constexpr std::size_t kMaxStemChars = 64;
    if (output.size() > kMaxStemChars)
    {
        output.resize(kMaxStemChars);
        while (!output.empty() && (output.back() == '-' || output.back() == '.' || output.back() == ' '))
        {
            output.pop_back();
        }
    }
    return output.empty() ? "clipboard" : output;
}

fs::path SafeRelativePathFromUtf8(const std::string &relative)
{
    std::string normalized = relative;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    fs::path output;
    std::size_t begin = 0;
    while (begin <= normalized.size())
    {
        const std::size_t slash = normalized.find('/', begin);
        const std::string segment =
            Trim(normalized.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin));
        if (!segment.empty() && segment != ".")
        {
            if (segment == ".." || segment.find(':') != std::string::npos)
            {
                throw UploadFailure("相对目录不能包含 .. 或盘符: " + relative, false, 0);
            }
            output /= Utf8ToWide(segment);
        }
        if (slash == std::string::npos)
        {
            break;
        }
        begin = slash + 1;
    }
    return output;
}

fs::path BuildLocalMarkdownPath(const fs::path &root, const std::string &folder, const std::string &prefix,
                                const UploadJob &job)
{
    const std::string safe_prefix = SanitizePortableFileStem(prefix);
    const std::wstring title = SanitizeMarkdownFileStem(Utf8ToWide(job.title));
    const std::wstring filename =
        Utf8ToWide(safe_prefix + "-" + std::to_string(job.created_at_ms) + "-" + job.hash.substr(0, 12)) + L"-" +
        title + L".md";
    return root / SafeRelativePathFromUtf8(folder) / filename;
}

std::wstring QuoteWindowsArg(const std::wstring &arg)
{
    std::wstring quoted = L"\"";
    for (wchar_t ch : arg)
    {
        if (ch == L'"')
        {
            quoted += L"\\\"";
        }
        else
        {
            quoted.push_back(ch);
        }
    }
    quoted += L"\"";
    return quoted;
}

std::string RunSystemCommand(const std::wstring &command, const std::string &description)
{
    const int exit_code = _wsystem(command.c_str());
    if (exit_code != 0)
    {
        throw UploadFailure(description + " 失败，exit_code=" + std::to_string(exit_code), true, 0);
    }
    return std::to_string(exit_code);
}

class ObsidianTarget : public UploadTarget
{
public:
    ObsidianTarget(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger) {}

    std::string Name() const override
    {
        return "obsidian";
    }

    void Validate() override
    {
        ValidateConfigOrThrow(config_);
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        if (job->remote_progress > 0 && !job->remote_url.empty())
        {
            return;
        }

        const fs::path output_path =
            BuildLocalMarkdownPath(config_.obsidian_vault_dir, config_.obsidian_folder, config_.obsidian_filename_prefix,
                                   *job);
        fs::create_directories(output_path.parent_path());
        AtomicWriteFile(output_path, BuildMarkdownDocument(*job));
        job->remote_id = WideToUtf8(output_path.wstring());
        job->remote_url = job->remote_id;
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("Obsidian 写入成功: " + job->remote_url);
        }
    }

private:
    AppConfig config_;
    Logger *logger_ = nullptr;
};

class LocalGitTarget : public UploadTarget
{
public:
    LocalGitTarget(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger) {}

    std::string Name() const override
    {
        return "local_git";
    }

    void Validate() override
    {
        ValidateConfigOrThrow(config_);
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        if (job->remote_progress > 0 && !job->remote_url.empty())
        {
            return;
        }

        const fs::path output_path = BuildLocalMarkdownPath(config_.local_git_repo_dir, config_.local_git_directory,
                                                            config_.local_git_filename_prefix, *job);
        fs::create_directories(output_path.parent_path());
        AtomicWriteFile(output_path, BuildMarkdownDocument(*job));

        if (config_.local_git_auto_commit)
        {
            CommitFile(output_path, *job);
        }

        job->remote_id = WideToUtf8(output_path.lexically_relative(config_.local_git_repo_dir).wstring());
        job->remote_url = WideToUtf8(output_path.wstring());
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("本地 Git 写入成功: " + job->remote_url);
        }
    }

private:
    void CommitFile(const fs::path &output_path, const UploadJob &job)
    {
        const fs::path relative = output_path.lexically_relative(config_.local_git_repo_dir);
        const std::wstring repo = std::filesystem::absolute(config_.local_git_repo_dir).wstring();
        const std::wstring rel = relative.wstring();
        RunSystemCommand(L"git -C " + QuoteWindowsArg(repo) + L" add -- " + QuoteWindowsArg(rel), "git add");
        RunSystemCommand(L"git -C " + QuoteWindowsArg(repo) + L" commit -m " +
                             QuoteWindowsArg(Utf8ToWide("Add clipboard note: " + job.title)) + L" -- " +
                             QuoteWindowsArg(rel),
                         "git commit");
    }

    AppConfig config_;
    Logger *logger_ = nullptr;
};

std::string Base64Encode(const std::string &input)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= input.size())
    {
        const unsigned int a = static_cast<unsigned char>(input[i++]);
        const unsigned int b = static_cast<unsigned char>(input[i++]);
        const unsigned int c = static_cast<unsigned char>(input[i++]);
        output.push_back(kAlphabet[(a >> 2) & 0x3f]);
        output.push_back(kAlphabet[((a & 0x03) << 4) | ((b >> 4) & 0x0f)]);
        output.push_back(kAlphabet[((b & 0x0f) << 2) | ((c >> 6) & 0x03)]);
        output.push_back(kAlphabet[c & 0x3f]);
    }
    const std::size_t remaining = input.size() - i;
    if (remaining == 1)
    {
        const unsigned int a = static_cast<unsigned char>(input[i]);
        output.push_back(kAlphabet[(a >> 2) & 0x3f]);
        output.push_back(kAlphabet[(a & 0x03) << 4]);
        output.push_back('=');
        output.push_back('=');
    }
    else if (remaining == 2)
    {
        const unsigned int a = static_cast<unsigned char>(input[i]);
        const unsigned int b = static_cast<unsigned char>(input[i + 1]);
        output.push_back(kAlphabet[(a >> 2) & 0x3f]);
        output.push_back(kAlphabet[((a & 0x03) << 4) | ((b >> 4) & 0x0f)]);
        output.push_back(kAlphabet[(b & 0x0f) << 2]);
        output.push_back('=');
    }
    return output;
}

std::string PercentEncodePathSegment(const std::string &segment)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char ch : segment)
    {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
        {
            output.push_back(static_cast<char>(ch));
            continue;
        }
        output.push_back('%');
        output.push_back(kHex[(ch >> 4) & 0x0f]);
        output.push_back(kHex[ch & 0x0f]);
    }
    return output;
}

std::string NormalizeGitHubRepoDirectory(const std::string &directory)
{
    std::string normalized = directory;
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    std::vector<std::string> segments;
    std::size_t begin = 0;
    while (begin <= normalized.size())
    {
        const std::size_t slash = normalized.find('/', begin);
        const std::string raw_segment =
            Trim(normalized.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin));
        if (!raw_segment.empty() && raw_segment != "." && raw_segment != "..")
        {
            segments.push_back(SanitizePortableFileStem(raw_segment));
        }
        if (slash == std::string::npos)
        {
            break;
        }
        begin = slash + 1;
    }

    std::string output;
    for (const std::string &segment : segments)
    {
        if (!output.empty())
        {
            output += "/";
        }
        output += segment;
    }
    return output;
}

std::string JoinGitHubRepoPath(const std::string &directory, const std::string &filename)
{
    const std::string normalized_directory = NormalizeGitHubRepoDirectory(directory);
    if (normalized_directory.empty())
    {
        return filename;
    }
    return normalized_directory + "/" + filename;
}

std::string EncodeGitHubContentPath(const std::string &path)
{
    std::string output;
    std::size_t begin = 0;
    while (begin <= path.size())
    {
        const std::size_t slash = path.find('/', begin);
        const std::string segment = path.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
        if (!output.empty())
        {
            output += "/";
        }
        output += PercentEncodePathSegment(segment);
        if (slash == std::string::npos)
        {
            break;
        }
        begin = slash + 1;
    }
    return output;
}

std::string BuildGitHubGistPayload(const UploadJob &job, const AppConfig &config)
{
    const std::string stem = SanitizePortableFileStem(config.github_gist_filename_prefix);
    const std::string filename = stem + "-" + job.hash.substr(0, 12) + ".md";

    std::ostringstream body;
    body << "{"
         << "\"description\":\"" << EscapeJson(job.title) << "\","
         << "\"public\":" << (config.github_gist_public ? "true" : "false") << ","
         << "\"files\":{\"" << EscapeJson(filename) << "\":{\"content\":\""
         << EscapeJson(BuildMarkdownDocument(job)) << "\"}}"
         << "}";
    return body.str();
}

std::string BuildGitHubRepoFilePath(const UploadJob &job, const AppConfig &config)
{
    const std::string stem = SanitizePortableFileStem(config.github_repo_filename_prefix);
    const std::string filename = stem + "-" + std::to_string(job.created_at_ms) + "-" + job.hash.substr(0, 12) + ".md";
    return JoinGitHubRepoPath(config.github_repo_directory, filename);
}

std::string BuildGitHubRepoApiUrl(const UploadJob &job, const AppConfig &config)
{
    return "https://api.github.com/repos/" + PercentEncodePathSegment(config.github_repo_owner) + "/" +
           PercentEncodePathSegment(config.github_repo_name) + "/contents/" +
           EncodeGitHubContentPath(BuildGitHubRepoFilePath(job, config));
}

std::string BuildGitHubRepoPayload(const UploadJob &job, const AppConfig &config)
{
    const std::string title = Trim(job.title).empty() ? "clipboard note" : job.title;
    std::ostringstream body;
    body << "{"
         << "\"message\":\"" << EscapeJson("Add clipboard note: " + title) << "\","
         << "\"content\":\"" << Base64Encode(BuildMarkdownDocument(job)) << "\"";
    if (!Trim(config.github_repo_branch).empty())
    {
        body << ",\"branch\":\"" << EscapeJson(Trim(config.github_repo_branch)) << "\"";
    }
    body << "}";
    return body.str();
}

std::string EncodeSlashSeparatedPath(const std::string &path)
{
    std::string output;
    std::size_t begin = 0;
    while (begin <= path.size())
    {
        const std::size_t slash = path.find('/', begin);
        const std::string segment = path.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
        if (!segment.empty())
        {
            if (!output.empty())
            {
                output += "/";
            }
            output += PercentEncodePathSegment(segment);
        }
        if (slash == std::string::npos)
        {
            break;
        }
        begin = slash + 1;
    }
    return output;
}

std::string BuildYuqueSlug(const UploadJob &job, const AppConfig &config)
{
    return SanitizePortableFileStem(config.yuque_slug_prefix) + "-" + std::to_string(job.created_at_ms) + "-" +
           job.hash.substr(0, 12);
}

std::string BuildYuqueApiUrl(const AppConfig &config)
{
    return "https://www.yuque.com/api/v2/repos/" + EncodeSlashSeparatedPath(config.yuque_namespace) + "/docs";
}

std::string BuildYuquePayload(const UploadJob &job, const AppConfig &config)
{
    std::ostringstream body;
    body << "{"
         << "\"title\":\"" << EscapeJson(job.title) << "\","
         << "\"slug\":\"" << BuildYuqueSlug(job, config) << "\","
         << "\"format\":\"markdown\","
         << "\"body\":\"" << EscapeJson(BuildMarkdownDocument(job)) << "\""
         << "}";
    return body.str();
}

std::string BuildFeishuAuthPayload(const AppConfig &config)
{
    return "{\"app_id\":\"" + EscapeJson(config.feishu_app_id) + "\",\"app_secret\":\"" +
           EscapeJson(config.feishu_app_secret) + "\"}";
}

std::string BuildFeishuDocumentPayload(const UploadJob &job, const AppConfig &config)
{
    std::ostringstream body;
    body << "{\"title\":\"" << EscapeJson(job.title) << "\"";
    if (!Trim(config.feishu_folder_token).empty())
    {
        body << ",\"folder_token\":\"" << EscapeJson(Trim(config.feishu_folder_token)) << "\"";
    }
    body << "}";
    return body.str();
}

std::vector<std::string> SplitForFeishuTextBlocks(const std::string &text)
{
    constexpr std::size_t kMaxChunkBytes = 1600;
    std::vector<std::string> chunks;
    std::size_t begin = 0;
    while (begin < text.size())
    {
        std::size_t end = std::min(text.size(), begin + kMaxChunkBytes);
        while (end > begin && (static_cast<unsigned char>(text[end - 1]) & 0xC0) == 0x80)
        {
            --end;
        }
        if (end == begin)
        {
            end = std::min(text.size(), begin + kMaxChunkBytes);
        }
        chunks.push_back(text.substr(begin, end - begin));
        begin = end;
    }
    if (chunks.empty())
    {
        chunks.push_back("");
    }
    return chunks;
}

std::string BuildFeishuChildrenPayload(const UploadJob &job)
{
    const std::string content = BuildMarkdownDocument(job);
    std::ostringstream body;
    body << "{\"index\":0,\"children\":[";
    bool first = true;
    for (const std::string &chunk : SplitForFeishuTextBlocks(content))
    {
        if (!first)
        {
            body << ",";
        }
        body << "{\"block_type\":2,\"text\":{\"elements\":[{\"text_run\":{\"content\":\"" << EscapeJson(chunk)
             << "\"}}]}}";
        first = false;
    }
    body << "]}";
    return body.str();
}

class GitHubGistTarget : public UploadTarget
{
public:
    GitHubGistTarget(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger) {}

    std::string Name() const override
    {
        return "github_gist";
    }

    void Validate() override
    {
        ValidateConfigOrThrow(config_);
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        if (job->remote_progress > 0 && !job->remote_url.empty())
        {
            return;
        }

        const HttpResponse response = CreateGistWithRetry(BuildGitHubGistPayload(*job, config_));
        const auto ids = ExtractGistResponseIds(response.body);
        if (ids.first.empty())
        {
            throw UploadFailure("GitHub Gist 创建响应缺少 id", true, 0);
        }
        job->remote_id = ids.first;
        job->remote_url = ids.second.empty() ? ("https://gist.github.com/" + ids.first) : ids.second;
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("GitHub Gist 上传成功: " + job->id + " -> " + job->remote_url);
        }
    }

private:
    HttpResponse CreateGistWithRetry(const std::string &body)
    {
        std::string last_error;
        int retry_after = 0;
        for (int attempt = 0; attempt <= config_.http_retry_attempts; ++attempt)
        {
            try
            {
                Throttle();
                std::wstring headers;
                headers += L"Authorization: Bearer ";
                headers += Utf8ToWide(config_.github_token);
                headers += L"\r\nAccept: application/vnd.github+json\r\n";
                headers += L"X-GitHub-Api-Version: 2026-03-10\r\n";
                headers += L"User-Agent: notion-clipboard-win\r\n";

                const HttpResponse response =
                    http_.RequestJsonUrl(L"POST", "https://api.github.com/gists", headers, body, "GitHub Gist");
                retry_after = response.retry_after_seconds;
                if (response.status_code >= 200 && response.status_code < 300)
                {
                    return response;
                }

                const bool retryable = IsRetryableHttpStatus(response.status_code);
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
                const int delay_ms = ComputeHttpRetryDelayMs(retry_after, attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        throw UploadFailure(last_error.empty() ? "GitHub Gist 请求失败" : last_error, true, retry_after);
    }

    static std::pair<std::string, std::string> ExtractGistResponseIds(const std::string &body)
    {
        try
        {
            const JsonValue json = ParseJson(body);
            if (!json.is_object())
            {
                return {};
            }
            const JsonValue *id = json.find("id");
            const JsonValue *url = json.find("html_url");
            return {(id != nullptr && id->is_string()) ? id->as_string() : "",
                    (url != nullptr && url->is_string()) ? url->as_string() : ""};
        }
        catch (...)
        {
            return {};
        }
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
    std::mutex throttle_mutex_;
    std::chrono::steady_clock::time_point last_request_at_;
};

class FeishuDocTarget : public UploadTarget
{
public:
    FeishuDocTarget(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger) {}

    std::string Name() const override
    {
        return "feishu_doc";
    }

    void Validate() override
    {
        ValidateConfigOrThrow(config_);
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        if (job->remote_progress > 0 && !job->remote_url.empty())
        {
            return;
        }

        const std::string tenant_token = GetTenantAccessToken();
        const auto created = CreateDocument(tenant_token, *job);
        AppendDocumentText(tenant_token, created.first, *job);

        job->remote_id = created.first;
        job->remote_url = created.second.empty() ? created.first : created.second;
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("飞书文档上传成功: " + job->id + " -> " + job->remote_url);
        }
    }

private:
    std::string GetTenantAccessToken()
    {
        const HttpResponse response =
            RequestWithRetry(L"POST", "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
                             L"", BuildFeishuAuthPayload(config_), "Feishu Auth");
        const JsonValue json = ParseJson(response.body);
        const JsonValue *token = json.find("tenant_access_token");
        if (token == nullptr || !token->is_string())
        {
            const JsonValue *data = json.find("data");
            token = data == nullptr ? nullptr : data->find("tenant_access_token");
        }
        if (token == nullptr || !token->is_string())
        {
            throw UploadFailure("飞书 tenant_access_token 响应缺少 token", true, 0);
        }
        return token->as_string();
    }

    std::pair<std::string, std::string> CreateDocument(const std::string &tenant_token, const UploadJob &job)
    {
        const std::wstring headers = AuthHeaders(tenant_token);
        const HttpResponse response =
            RequestWithRetry(L"POST", "https://open.feishu.cn/open-apis/docx/v1/documents", headers,
                             BuildFeishuDocumentPayload(job, config_), "Feishu Doc");
        const JsonValue json = ParseJson(response.body);
        const JsonValue *data = json.find("data");
        const JsonValue *document = data == nullptr ? nullptr : data->find("document");
        const JsonValue *id = document == nullptr ? nullptr : document->find("document_id");
        if (id == nullptr || !id->is_string())
        {
            id = data == nullptr ? nullptr : data->find("document_id");
        }
        const JsonValue *url = document == nullptr ? nullptr : document->find("url");
        if (url == nullptr && data != nullptr)
        {
            url = data->find("url");
        }
        if (id == nullptr || !id->is_string())
        {
            throw UploadFailure("飞书创建文档响应缺少 document_id", true, 0);
        }
        return {id->as_string(), (url != nullptr && url->is_string()) ? url->as_string() : ""};
    }

    void AppendDocumentText(const std::string &tenant_token, const std::string &document_id, const UploadJob &job)
    {
        const std::string encoded_id = PercentEncodePathSegment(document_id);
        RequestWithRetry(L"POST",
                         "https://open.feishu.cn/open-apis/docx/v1/documents/" + encoded_id + "/blocks/" +
                             encoded_id + "/children",
                         AuthHeaders(tenant_token), BuildFeishuChildrenPayload(job), "Feishu Blocks");
    }

    HttpResponse RequestWithRetry(const std::wstring &method, const std::string &url, const std::wstring &headers,
                                  const std::string &body, const std::string &connection_name)
    {
        std::string last_error;
        int retry_after = 0;
        for (int attempt = 0; attempt <= config_.http_retry_attempts; ++attempt)
        {
            try
            {
                Throttle();
                const HttpResponse response = http_.RequestJsonUrl(method, url, headers, body, connection_name);
                retry_after = response.retry_after_seconds;
                if (response.status_code >= 200 && response.status_code < 300)
                {
                    EnsureBusinessCodeOk(connection_name, response.body);
                    return response;
                }
                const bool retryable = IsRetryableHttpStatus(response.status_code);
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
                std::this_thread::sleep_for(std::chrono::milliseconds(ComputeHttpRetryDelayMs(retry_after, attempt)));
            }
        }
        throw UploadFailure(last_error.empty() ? "飞书请求失败" : last_error, true, retry_after);
    }

    std::wstring AuthHeaders(const std::string &tenant_token) const
    {
        return L"Authorization: Bearer " + Utf8ToWide(tenant_token) + L"\r\n";
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
    std::mutex throttle_mutex_;
    std::chrono::steady_clock::time_point last_request_at_;
};

class YuqueTarget : public UploadTarget
{
public:
    YuqueTarget(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger) {}

    std::string Name() const override
    {
        return "yuque";
    }

    void Validate() override
    {
        ValidateConfigOrThrow(config_);
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        if (job->remote_progress > 0 && !job->remote_url.empty())
        {
            return;
        }

        const HttpResponse response = PostWithRetry(BuildYuquePayload(*job, config_));
        const auto ids = ExtractYuqueResponseIds(response.body, *job);
        if (ids.first.empty())
        {
            throw UploadFailure("语雀创建文档响应缺少 id 或 slug", true, 0);
        }
        job->remote_id = ids.first;
        job->remote_url = ids.second;
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("语雀上传成功: " + job->id + " -> " + job->remote_url);
        }
    }

private:
    HttpResponse PostWithRetry(const std::string &body)
    {
        std::string last_error;
        int retry_after = 0;
        for (int attempt = 0; attempt <= config_.http_retry_attempts; ++attempt)
        {
            try
            {
                Throttle();
                std::wstring headers;
                headers += L"X-Auth-Token: ";
                headers += Utf8ToWide(config_.yuque_token);
                headers += L"\r\nUser-Agent: notion-clipboard-win\r\n";
                const HttpResponse response =
                    http_.RequestJsonUrl(L"POST", BuildYuqueApiUrl(config_), headers, body, "Yuque");
                retry_after = response.retry_after_seconds;
                if (response.status_code >= 200 && response.status_code < 300)
                {
                    EnsureBusinessCodeOk("语雀", response.body);
                    return response;
                }

                const bool retryable = IsRetryableHttpStatus(response.status_code);
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
                const int delay_ms = ComputeHttpRetryDelayMs(retry_after, attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        throw UploadFailure(last_error.empty() ? "语雀请求失败" : last_error, true, retry_after);
    }

    std::pair<std::string, std::string> ExtractYuqueResponseIds(const std::string &body, const UploadJob &job) const
    {
        try
        {
            const JsonValue json = ParseJson(body);
            const JsonValue *data = json.find("data");
            if (data == nullptr || !data->is_object())
            {
                return {};
            }
            const JsonValue *id = data->find("id");
            const JsonValue *slug = data->find("slug");
            const std::string resolved_slug =
                (slug != nullptr && slug->is_string()) ? slug->as_string() : BuildYuqueSlug(job, config_);
            const std::string resolved_id =
                (id != nullptr && id->is_string())
                    ? id->as_string()
                    : ((id != nullptr && id->is_number()) ? std::to_string(static_cast<long long>(id->as_number()))
                                                           : resolved_slug);
            return {resolved_id, "https://www.yuque.com/" + Trim(config_.yuque_namespace) + "/" + resolved_slug};
        }
        catch (...)
        {
            return {};
        }
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
    std::mutex throttle_mutex_;
    std::chrono::steady_clock::time_point last_request_at_;
};

class GitHubRepoTarget : public UploadTarget
{
public:
    GitHubRepoTarget(AppConfig config, Logger *logger) : config_(std::move(config)), logger_(logger) {}

    std::string Name() const override
    {
        return "github_repo";
    }

    void Validate() override
    {
        ValidateConfigOrThrow(config_);
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        EnsureJobTarget(job, Name());
        if (job->remote_progress > 0 && !job->remote_url.empty())
        {
            return;
        }

        const HttpResponse response =
            PutFileWithRetry(BuildGitHubRepoApiUrl(*job, config_), BuildGitHubRepoPayload(*job, config_));
        const auto ids = ExtractGitHubRepoResponseIds(response.body);
        if (ids.first.empty())
        {
            throw UploadFailure("GitHub 仓库文件创建响应缺少 sha", true, 0);
        }
        job->remote_id = ids.first;
        job->remote_url = ids.second.empty() ? ("https://github.com/" + config_.github_repo_owner + "/" +
                                                config_.github_repo_name)
                                             : ids.second;
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("GitHub 仓库上传成功: " + job->id + " -> " + job->remote_url);
        }
    }

private:
    HttpResponse PutFileWithRetry(const std::string &url, const std::string &body)
    {
        std::string last_error;
        int retry_after = 0;
        for (int attempt = 0; attempt <= config_.http_retry_attempts; ++attempt)
        {
            try
            {
                Throttle();
                std::wstring headers;
                headers += L"Authorization: Bearer ";
                headers += Utf8ToWide(config_.github_token);
                headers += L"\r\nAccept: application/vnd.github+json\r\n";
                headers += L"X-GitHub-Api-Version: 2026-03-10\r\n";
                headers += L"User-Agent: notion-clipboard-win\r\n";

                const HttpResponse response = http_.RequestJsonUrl(L"PUT", url, headers, body, "GitHub Repo");
                retry_after = response.retry_after_seconds;
                if (response.status_code >= 200 && response.status_code < 300)
                {
                    return response;
                }

                const bool retryable = IsRetryableHttpStatus(response.status_code);
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
                const int delay_ms = ComputeHttpRetryDelayMs(retry_after, attempt);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        throw UploadFailure(last_error.empty() ? "GitHub 仓库请求失败" : last_error, true, retry_after);
    }

    static std::pair<std::string, std::string> ExtractGitHubRepoResponseIds(const std::string &body)
    {
        try
        {
            const JsonValue json = ParseJson(body);
            if (!json.is_object())
            {
                return {};
            }
            const JsonValue *content = json.find("content");
            const JsonValue *commit = json.find("commit");
            const JsonValue *content_sha = content == nullptr ? nullptr : content->find("sha");
            const JsonValue *commit_sha = commit == nullptr ? nullptr : commit->find("sha");
            const JsonValue *content_url = content == nullptr ? nullptr : content->find("html_url");
            const JsonValue *commit_url = commit == nullptr ? nullptr : commit->find("html_url");
            const std::string id = (commit_sha != nullptr && commit_sha->is_string())
                                       ? commit_sha->as_string()
                                       : ((content_sha != nullptr && content_sha->is_string()) ? content_sha->as_string()
                                                                                               : "");
            const std::string url = (content_url != nullptr && content_url->is_string())
                                        ? content_url->as_string()
                                        : ((commit_url != nullptr && commit_url->is_string()) ? commit_url->as_string()
                                                                                              : "");
            return {id, url};
        }
        catch (...)
        {
            return {};
        }
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
    std::mutex throttle_mutex_;
    std::chrono::steady_clock::time_point last_request_at_;
};

UploadJob MakeTestUploadJob(const std::string &content, const std::string &target)
{
    UploadJob job;
    job.created_at_ms = NowUnixMs();
    job.not_before_ms = 0;
    job.target = target;
    job.hash = Hex64(Fnv1a64(content));
    job.title = BuildTitleFromContent(content);
    job.content = content;
    job.id = "test-" + job.hash.substr(0, 12) + "-" + std::to_string(job.created_at_ms);
    return job;
}
}

std::unique_ptr<UploadTarget> CreateUploadTarget(const AppConfig &config, Logger *logger)
{
    if (config.upload_target == "notion")
    {
        return std::make_unique<NotionClient>(config, logger);
    }
    if (config.upload_target == "markdown_file")
    {
        return std::make_unique<MarkdownFileTarget>(config, logger);
    }
    if (config.upload_target == "obsidian")
    {
        return std::make_unique<ObsidianTarget>(config, logger);
    }
    if (config.upload_target == "local_git")
    {
        return std::make_unique<LocalGitTarget>(config, logger);
    }
    if (config.upload_target == "webhook")
    {
        return std::make_unique<WebhookTarget>(config, logger);
    }
    if (config.upload_target == "github_gist")
    {
        return std::make_unique<GitHubGistTarget>(config, logger);
    }
    if (config.upload_target == "github_repo")
    {
        return std::make_unique<GitHubRepoTarget>(config, logger);
    }
    if (config.upload_target == "yuque")
    {
        return std::make_unique<YuqueTarget>(config, logger);
    }
    if (config.upload_target == "feishu_doc")
    {
        return std::make_unique<FeishuDocTarget>(config, logger);
    }
    throw std::runtime_error("未知 upload_target: " + config.upload_target);
}

int RunUploadTargetSelfTest()
{
    bool ok = true;
    auto fail = [&](const std::string &message)
    {
        std::cout << "[FAIL] upload target self-test: " << message << "\n";
        ok = false;
    };

    const fs::path root =
        fs::temp_directory_path() / (L"notion-clipboard-win-target-test-" + std::to_wstring(NowUnixMs()));
    std::error_code ignored;
    fs::remove_all(root, ignored);

    try
    {
        AppConfig config;
        config.upload_target = "markdown_file";
        config.state_dir = root / L"state";
        config.markdown_output_dir = root / L"out";
        ValidateConfigOrThrow(config);

        MarkdownFileTarget target(config, nullptr);
        target.Validate();

        UploadJob job = MakeTestUploadJob("测试标题\n\n正文 $x$。", config.upload_target);
        bool checkpointed = false;
        target.ProcessJob(&job, [&]
                          { checkpointed = true; });

        const fs::path output_path = fs::path(Utf8ToWide(job.remote_url));
        if (!checkpointed)
        {
            fail("markdown_file did not checkpoint progress");
        }
        if (job.target != "markdown_file" || job.remote_url.empty() || job.remote_progress != 1)
        {
            fail("markdown_file did not update generic job progress");
        }
        if (!fs::exists(output_path))
        {
            fail("markdown_file output file missing");
        }
        else
        {
            const std::string written = ReadWholeFile(output_path);
            if (written.find("# 测试标题") == std::string::npos || written.find("正文 $x$。") == std::string::npos ||
                written.find("job_id: " + job.id) == std::string::npos)
            {
                fail("markdown_file output content mismatch");
            }
        }

        PersistentQueue legacy_queue(root / L"legacy-state", 12);
        const fs::path legacy_job_path = root / L"legacy-state" / L"queue" / L"legacy.job";
        AtomicWriteFile(legacy_job_path,
                        "{\n"
                        "  \"id\":\"legacy\",\n"
                        "  \"created_at_ms\":1,\n"
                        "  \"not_before_ms\":0,\n"
                        "  \"attempts\":0,\n"
                        "  \"hash\":\"abc\",\n"
                        "  \"title\":\"Legacy\",\n"
                        "  \"content\":\"body\",\n"
                        "  \"page_id\":\"legacy-page\",\n"
                        "  \"page_url\":\"https://legacy.example/page\",\n"
                        "  \"appended_block_count\":7,\n"
                        "  \"last_error\":\"\"\n"
                        "}\n");
        std::uint64_t next_due_ms = 0;
        const auto legacy_item = legacy_queue.NextDueJob(NowUnixMs(), &next_due_ms, nullptr);
        if (!legacy_item.has_value() || legacy_item->first.remote_id != "legacy-page" ||
            legacy_item->first.remote_url != "https://legacy.example/page" || legacy_item->first.remote_progress != 7)
        {
            fail("legacy queue progress fields were not migrated");
        }
        else
        {
            legacy_queue.Update(legacy_item->second, legacy_item->first);
            const std::string migrated_json = ReadWholeFile(legacy_item->second);
            if (migrated_json.find("\"remote_id\":\"legacy-page\"") == std::string::npos ||
                migrated_json.find("\"remote_url\":\"https://legacy.example/page\"") == std::string::npos ||
                migrated_json.find("\"remote_progress\":7") == std::string::npos)
            {
                fail("legacy queue progress fields were not written with generic names");
            }
        }

        AppConfig webhook_config;
        webhook_config.upload_target = "webhook";
        webhook_config.webhook_url = "https://example.com/hook";
        webhook_config.webhook_bearer_token = "secret_should_not_be_in_payload";
        ValidateConfigOrThrow(webhook_config);

        UploadJob webhook_job = MakeTestUploadJob("Webhook 标题\n\n正文 $y$。", webhook_config.upload_target);
        const std::string payload = BuildWebhookPayload(webhook_job);
        const JsonValue payload_json = ParseJson(payload);
        if (!payload_json.is_object())
        {
            fail("webhook payload is not JSON object");
        }
        const JsonValue *payload_target = payload_json.find("target");
        const JsonValue *title = payload_json.find("title");
        const JsonValue *format = payload_json.find("content_format");
        const JsonValue *content = payload_json.find("content");
        if (payload_target == nullptr || !payload_target->is_string() || payload_target->as_string() != "webhook" ||
            title == nullptr || !title->is_string() || title->as_string() != "Webhook 标题" ||
            format == nullptr || !format->is_string() || format->as_string() != "markdown" ||
            content == nullptr || !content->is_string() || content->as_string().find("正文 $y$。") == std::string::npos)
        {
            fail("webhook payload fields mismatch");
        }
        if (payload.find(webhook_config.webhook_bearer_token) != std::string::npos)
        {
            fail("webhook bearer token leaked into payload");
        }

        AppConfig invalid_webhook_config;
        invalid_webhook_config.upload_target = "webhook";
        bool rejected_missing_url = false;
        try
        {
            ValidateConfigOrThrow(invalid_webhook_config);
        }
        catch (const std::exception &)
        {
            rejected_missing_url = true;
        }
        if (!rejected_missing_url)
        {
            fail("webhook target accepted missing URL");
        }

        AppConfig obsidian_config;
        obsidian_config.upload_target = "obsidian";
        obsidian_config.obsidian_vault_dir = root / L"obsidian-vault";
        obsidian_config.obsidian_folder = "Inbox/Clipboard";
        obsidian_config.obsidian_filename_prefix = "clip";
        fs::create_directories(obsidian_config.obsidian_vault_dir);
        ValidateConfigOrThrow(obsidian_config);

        ObsidianTarget obsidian_target(obsidian_config, nullptr);
        UploadJob obsidian_job = MakeTestUploadJob("Obsidian 标题\n\n正文 $o$。", obsidian_config.upload_target);
        bool obsidian_checkpointed = false;
        obsidian_target.ProcessJob(&obsidian_job, [&]
                                   { obsidian_checkpointed = true; });
        if (!obsidian_checkpointed || obsidian_job.remote_progress != 1 ||
            !fs::exists(fs::path(Utf8ToWide(obsidian_job.remote_url))))
        {
            fail("obsidian target did not write markdown file");
        }

        AppConfig local_git_config;
        local_git_config.upload_target = "local_git";
        local_git_config.local_git_repo_dir = root / L"local-git";
        local_git_config.local_git_directory = "notes/clipboard";
        local_git_config.local_git_filename_prefix = "clip";
        local_git_config.local_git_auto_commit = false;
        fs::create_directories(local_git_config.local_git_repo_dir / L".git");
        ValidateConfigOrThrow(local_git_config);

        LocalGitTarget local_git_target(local_git_config, nullptr);
        UploadJob local_git_job = MakeTestUploadJob("Git 标题\n\n正文 $g$。", local_git_config.upload_target);
        bool local_git_checkpointed = false;
        local_git_target.ProcessJob(&local_git_job, [&]
                                    { local_git_checkpointed = true; });
        if (!local_git_checkpointed || local_git_job.remote_progress != 1 ||
            !fs::exists(fs::path(Utf8ToWide(local_git_job.remote_url))) ||
            local_git_job.remote_id.find("notes") == std::string::npos)
        {
            fail("local_git target did not write markdown file");
        }

        AppConfig gist_config;
        gist_config.upload_target = "github_gist";
        gist_config.github_token = "github_secret_should_not_be_in_payload";
        gist_config.github_gist_public = false;
        gist_config.github_gist_filename_prefix = "Algo Notes";
        ValidateConfigOrThrow(gist_config);

        UploadJob gist_job = MakeTestUploadJob("Gist 标题\n\n正文 $z$。", gist_config.upload_target);
        const std::string gist_payload = BuildGitHubGistPayload(gist_job, gist_config);
        const JsonValue gist_json = ParseJson(gist_payload);
        if (!gist_json.is_object())
        {
            fail("github_gist payload is not JSON object");
        }
        const JsonValue *gist_description = gist_json.find("description");
        const JsonValue *gist_public = gist_json.find("public");
        const JsonValue *gist_files = gist_json.find("files");
        if (gist_description == nullptr || !gist_description->is_string() ||
            gist_description->as_string() != "Gist 标题" || gist_public == nullptr || !gist_public->is_bool() ||
            gist_public->as_bool() || gist_files == nullptr || !gist_files->is_object())
        {
            fail("github_gist payload top-level fields mismatch");
        }
        else
        {
            const JsonValue *file = gist_files->find("Algo-Notes-" + gist_job.hash.substr(0, 12) + ".md");
            const JsonValue *file_content = file == nullptr ? nullptr : file->find("content");
            if (file == nullptr || !file->is_object() || file_content == nullptr || !file_content->is_string() ||
                file_content->as_string().find("正文 $z$。") == std::string::npos ||
                file_content->as_string().find("job_id: " + gist_job.id) == std::string::npos)
            {
                fail("github_gist file content mismatch");
            }
        }
        if (gist_payload.find(gist_config.github_token) != std::string::npos)
        {
            fail("github token leaked into gist payload");
        }

        AppConfig invalid_gist_config;
        invalid_gist_config.upload_target = "github_gist";
        bool rejected_missing_github_token = false;
        try
        {
            ValidateConfigOrThrow(invalid_gist_config);
        }
        catch (const std::exception &)
        {
            rejected_missing_github_token = true;
        }
        if (!rejected_missing_github_token)
        {
            fail("github_gist target accepted missing token");
        }

        AppConfig repo_config;
        repo_config.upload_target = "github_repo";
        repo_config.github_token = "repo_secret_should_not_be_in_payload";
        repo_config.github_repo_owner = "octo-org";
        repo_config.github_repo_name = "notes repo";
        repo_config.github_repo_branch = "main";
        repo_config.github_repo_directory = "AI Notes/Clipboard";
        repo_config.github_repo_filename_prefix = "Daily Clip";
        ValidateConfigOrThrow(repo_config);

        UploadJob repo_job = MakeTestUploadJob("Repo 标题\n\n正文 $w$。", repo_config.upload_target);
        repo_job.created_at_ms = 123456789;
        const std::string repo_path = BuildGitHubRepoFilePath(repo_job, repo_config);
        const std::string repo_url = BuildGitHubRepoApiUrl(repo_job, repo_config);
        const std::string repo_payload = BuildGitHubRepoPayload(repo_job, repo_config);
        const JsonValue repo_json = ParseJson(repo_payload);
        if (repo_path != "AI-Notes/Clipboard/Daily-Clip-123456789-" + repo_job.hash.substr(0, 12) + ".md")
        {
            fail("github_repo file path mismatch");
        }
        if (repo_url.find("https://api.github.com/repos/octo-org/notes%20repo/contents/AI-Notes/Clipboard/") != 0)
        {
            fail("github_repo API URL mismatch");
        }
        if (!repo_json.is_object())
        {
            fail("github_repo payload is not JSON object");
        }
        const JsonValue *repo_message = repo_json.find("message");
        const JsonValue *repo_content = repo_json.find("content");
        const JsonValue *repo_branch = repo_json.find("branch");
        if (repo_message == nullptr || !repo_message->is_string() ||
            repo_message->as_string() != "Add clipboard note: Repo 标题" || repo_content == nullptr ||
            !repo_content->is_string() || repo_content->as_string().find('\n') != std::string::npos ||
            repo_branch == nullptr || !repo_branch->is_string() || repo_branch->as_string() != "main")
        {
            fail("github_repo payload fields mismatch");
        }
        if (repo_payload.find(repo_config.github_token) != std::string::npos)
        {
            fail("github token leaked into repo payload");
        }

        AppConfig invalid_repo_config;
        invalid_repo_config.upload_target = "github_repo";
        invalid_repo_config.github_token = "token";
        invalid_repo_config.github_repo_owner = "owner";
        bool rejected_missing_repo_name = false;
        try
        {
            ValidateConfigOrThrow(invalid_repo_config);
        }
        catch (const std::exception &)
        {
            rejected_missing_repo_name = true;
        }
        if (!rejected_missing_repo_name)
        {
            fail("github_repo target accepted missing repo name");
        }

        AppConfig yuque_config;
        yuque_config.upload_target = "yuque";
        yuque_config.yuque_token = "yuque_secret_should_not_be_in_payload";
        yuque_config.yuque_namespace = "team/knowledge base";
        yuque_config.yuque_slug_prefix = "Daily Note";
        ValidateConfigOrThrow(yuque_config);

        UploadJob yuque_job = MakeTestUploadJob("语雀标题\n\n正文 $q$。", yuque_config.upload_target);
        yuque_job.created_at_ms = 222333444;
        const std::string yuque_payload = BuildYuquePayload(yuque_job, yuque_config);
        const std::string yuque_url = BuildYuqueApiUrl(yuque_config);
        const JsonValue yuque_json = ParseJson(yuque_payload);
        if (yuque_url != "https://www.yuque.com/api/v2/repos/team/knowledge%20base/docs")
        {
            fail("yuque API URL mismatch");
        }
        const JsonValue *yuque_title = yuque_json.find("title");
        const JsonValue *yuque_slug = yuque_json.find("slug");
        const JsonValue *yuque_format = yuque_json.find("format");
        const JsonValue *yuque_body = yuque_json.find("body");
        if (!yuque_json.is_object() || yuque_title == nullptr || !yuque_title->is_string() ||
            yuque_title->as_string() != "语雀标题" || yuque_slug == nullptr || !yuque_slug->is_string() ||
            yuque_slug->as_string() != "Daily-Note-222333444-" + yuque_job.hash.substr(0, 12) ||
            yuque_format == nullptr || !yuque_format->is_string() || yuque_format->as_string() != "markdown" ||
            yuque_body == nullptr || !yuque_body->is_string() ||
            yuque_body->as_string().find("正文 $q$。") == std::string::npos)
        {
            fail("yuque payload fields mismatch");
        }
        if (yuque_payload.find(yuque_config.yuque_token) != std::string::npos)
        {
            fail("yuque token leaked into payload");
        }

        AppConfig feishu_config;
        feishu_config.upload_target = "feishu_doc";
        feishu_config.feishu_app_id = "cli_a";
        feishu_config.feishu_app_secret = "feishu_secret_should_not_be_in_doc_payload";
        feishu_config.feishu_folder_token = "folder_token";
        ValidateConfigOrThrow(feishu_config);

        UploadJob feishu_job = MakeTestUploadJob("飞书标题\n\n正文 $f$。", feishu_config.upload_target);
        const std::string feishu_auth_payload = BuildFeishuAuthPayload(feishu_config);
        const std::string feishu_doc_payload = BuildFeishuDocumentPayload(feishu_job, feishu_config);
        const std::string feishu_children_payload = BuildFeishuChildrenPayload(feishu_job);
        const JsonValue feishu_auth_json = ParseJson(feishu_auth_payload);
        const JsonValue feishu_doc_json = ParseJson(feishu_doc_payload);
        const JsonValue feishu_children_json = ParseJson(feishu_children_payload);
        const JsonValue *feishu_app_id = feishu_auth_json.find("app_id");
        const JsonValue *feishu_title = feishu_doc_json.find("title");
        const JsonValue *feishu_folder = feishu_doc_json.find("folder_token");
        const JsonValue *feishu_children = feishu_children_json.find("children");
        if (!feishu_auth_json.is_object() || feishu_app_id == nullptr || !feishu_app_id->is_string() ||
            feishu_app_id->as_string() != "cli_a" || !feishu_doc_json.is_object() || feishu_title == nullptr ||
            !feishu_title->is_string() || feishu_title->as_string() != "飞书标题" || feishu_folder == nullptr ||
            !feishu_folder->is_string() || feishu_folder->as_string() != "folder_token" ||
            !feishu_children_json.is_object() || feishu_children == nullptr || !feishu_children->is_array() ||
            feishu_children->as_array().empty() ||
            feishu_children_payload.find("正文 $f$。") == std::string::npos)
        {
            fail("feishu_doc payload fields mismatch");
        }
        if (feishu_doc_payload.find(feishu_config.feishu_app_secret) != std::string::npos ||
            feishu_children_payload.find(feishu_config.feishu_app_secret) != std::string::npos)
        {
            fail("feishu secret leaked into document payload");
        }

        EnsureBusinessCodeOk("飞书", "{\"code\":0,\"msg\":\"success\"}");
        EnsureBusinessCodeOk("语雀", "{\"data\":{\"id\":1,\"slug\":\"ok\"}}");
        bool rejected_business_error = false;
        try
        {
            EnsureBusinessCodeOk("飞书", "{\"code\":999,\"msg\":\"bad token\"}");
        }
        catch (const UploadFailure &ex)
        {
            rejected_business_error =
                !ex.retryable() && std::string(ex.what()).find("code=999") != std::string::npos &&
                std::string(ex.what()).find("bad token") != std::string::npos;
        }
        if (!rejected_business_error)
        {
            fail("business code error was not rejected");
        }
    }
    catch (const std::exception &ex)
    {
        fail(ex.what());
    }

    fs::remove_all(root, ignored);
    if (ok)
    {
        std::cout << "[PASS] markdown_file upload target\n";
        std::cout << "[PASS] legacy queue progress migration\n";
        std::cout << "[PASS] obsidian upload target\n";
        std::cout << "[PASS] local_git upload target\n";
        std::cout << "[PASS] webhook upload target payload\n";
        std::cout << "[PASS] github_gist upload target payload\n";
        std::cout << "[PASS] github_repo upload target payload\n";
        std::cout << "[PASS] yuque upload target payload\n";
        std::cout << "[PASS] feishu_doc upload target payload\n";
    }
    return ok ? 0 : 1;
}
}
