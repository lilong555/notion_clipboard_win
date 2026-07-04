#include "upload_target.h"

#include "config.h"
#include "converter.h"
#include "http_client.h"
#include "json.h"
#include "logger.h"
#include "obsidian.h"
#include "queue.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
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

std::vector<std::string> ParseObsidianTags(const std::string &value)
{
    std::vector<std::string> tags;
    std::string token;
    auto flush = [&]
    {
        token = Trim(token);
        while (!token.empty() && token.front() == '#')
        {
            token.erase(token.begin());
            token = Trim(token);
        }
        std::replace(token.begin(), token.end(), '\\', '/');
        while (!token.empty() && token.front() == '/')
        {
            token.erase(token.begin());
        }
        while (!token.empty() && token.back() == '/')
        {
            token.pop_back();
        }
        if (!token.empty() && std::find(tags.begin(), tags.end(), token) == tags.end())
        {
            tags.push_back(token);
        }
        token.clear();
    };

    for (unsigned char ch : value)
    {
        if (ch == ',' || ch == ';' || std::isspace(ch))
        {
            flush();
        }
        else
        {
            token.push_back(static_cast<char>(ch));
        }
    }
    flush();
    return tags;
}

std::string YamlDoubleQuotedString(const std::string &value)
{
    std::string output = "\"";
    for (unsigned char ch : value)
    {
        if (ch == '\\' || ch == '"')
        {
            output.push_back('\\');
            output.push_back(static_cast<char>(ch));
        }
        else if (ch == '\r' || ch == '\n' || ch == '\t')
        {
            output.push_back(' ');
        }
        else
        {
            output.push_back(static_cast<char>(ch));
        }
    }
    output.push_back('"');
    return output;
}

std::string BuildObsidianFrontMatter(const std::vector<std::string> &tags)
{
    if (tags.empty())
    {
        return "";
    }

    std::ostringstream content;
    content << "---\n"
            << "tags:\n";
    for (const std::string &tag : tags)
    {
        content << "  - " << YamlDoubleQuotedString(tag) << "\n";
    }
    content << "---\n\n";
    return content.str();
}

std::string BuildMarkdownDocument(const UploadJob &job, bool normalize_for_obsidian = false,
                                  bool include_source_comment = true,
                                  const std::vector<std::string> &obsidian_tags = {})
{
    std::ostringstream content;
    content << BuildObsidianFrontMatter(obsidian_tags);
    content << "# " << job.title << "\n\n";
    if (include_source_comment)
    {
        content << "<!-- source: notion_clipboard_win; job_id: " << job.id << "; created_at: "
                << IsoUtcTimestampFromUnixMs(job.created_at_ms) << " -->\n\n";
    }
    const std::string body =
        normalize_for_obsidian ? NormalizeMarkdownForObsidian(job.content) : NormalizeLineEndings(job.content);
    content << body;
    if (body.empty() || body.back() != '\n')
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

fs::path MakeUniqueMarkdownPath(const fs::path &path)
{
    if (!fs::exists(path))
    {
        return path;
    }

    const fs::path parent = path.parent_path();
    const std::wstring stem = path.stem().wstring();
    const fs::path extension = path.extension();
    for (int suffix = 2; suffix < 1000; ++suffix)
    {
        fs::path candidate = parent / (stem + L" " + std::to_wstring(suffix) + extension.wstring());
        if (!fs::exists(candidate))
        {
            return candidate;
        }
    }
    return parent / (stem + L" " + std::to_wstring(NowUnixMs()) + extension.wstring());
}

fs::path BuildObsidianMarkdownPath(const fs::path &root, const std::string &folder, const UploadJob &job)
{
    const std::wstring title = SanitizeMarkdownFileStem(Utf8ToWide(job.title));
    return MakeUniqueMarkdownPath(root / SafeRelativePathFromUtf8(folder) / (title + L".md"));
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

        const fs::path output_path = BuildObsidianMarkdownPath(config_.obsidian_vault_dir, config_.obsidian_folder, *job);
        fs::create_directories(output_path.parent_path());
        AtomicWriteFile(output_path, BuildMarkdownDocument(*job, true, false, ParseObsidianTags(config_.obsidian_tags)));
        const std::string output_path_utf8 = WideToUtf8(output_path.wstring());
        const std::string open_uri = BuildObsidianOpenUri(config_.obsidian_vault_dir, output_path);
        job->remote_id = output_path_utf8;
        job->remote_url = open_uri.empty() ? output_path_utf8 : open_uri;
        job->remote_progress = 1;
        checkpoint();
        if (logger_ != nullptr)
        {
            logger_->Info("Obsidian 写入成功: " + output_path_utf8 +
                          (open_uri.empty() ? "" : (" -> " + open_uri)));
        }
    }

private:
    AppConfig config_;
    Logger *logger_ = nullptr;
};

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

std::string JoinTargetNames(const std::vector<std::string> &targets)
{
    std::ostringstream output;
    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        if (i > 0)
        {
            output << ",";
        }
        output << targets[i];
    }
    return output.str();
}

std::unique_ptr<UploadTarget> CreateSingleUploadTarget(AppConfig config, Logger *logger)
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
    if (config.upload_target == "webhook")
    {
        return std::make_unique<WebhookTarget>(config, logger);
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

class MultiUploadTarget : public UploadTarget
{
public:
    MultiUploadTarget(const AppConfig &config, Logger *logger, std::vector<std::string> targets)
        : target_names_(std::move(targets)), name_(JoinTargetNames(target_names_))
    {
        for (const std::string &target : target_names_)
        {
            AppConfig target_config = config;
            target_config.upload_target = target;
            targets_.push_back(CreateSingleUploadTarget(target_config, logger));
        }
    }

    std::string Name() const override
    {
        return name_;
    }

    void Validate() override
    {
        for (std::unique_ptr<UploadTarget> &target : targets_)
        {
            target->Validate();
        }
    }

    void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
    {
        if (job->target.empty() || job->target == name_)
        {
            ProcessCompositeJob(job, checkpoint);
            return;
        }

        for (std::size_t i = 0; i < target_names_.size(); ++i)
        {
            if (job->target == target_names_[i])
            {
                targets_[i]->ProcessJob(job, checkpoint);
                return;
            }
        }
        throw UploadFailure("任务目标是 " + job->target + "，当前多后端不包含该目标: " + name_, false, 0);
    }

private:
    void ProcessCompositeJob(UploadJob *job, const std::function<void()> &checkpoint)
    {
        std::vector<std::string> urls;
        for (std::size_t i = 0; i < target_names_.size(); ++i)
        {
            UploadJob child = *job;
            child.target = target_names_[i];
            child.remote_id.clear();
            child.remote_url.clear();
            child.remote_progress = 0;
            try
            {
                targets_[i]->ProcessJob(&child, [] {});
            }
            catch (const UploadFailure &ex)
            {
                throw UploadFailure(target_names_[i] + ": " + std::string(ex.what()), ex.retryable(),
                                    ex.retry_after_seconds());
            }
            urls.push_back(target_names_[i] + "=" + child.remote_url);
        }

        job->target = name_;
        job->remote_id = name_;
        job->remote_url = JoinTargetNames(urls);
        job->remote_progress = target_names_.size();
        checkpoint();
    }

    std::vector<std::string> target_names_;
    std::vector<std::unique_ptr<UploadTarget>> targets_;
    std::string name_;
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
    const std::vector<std::string> targets = ParseUploadTargets(config.upload_target);
    if (targets.empty())
    {
        throw std::runtime_error("upload_target 不能为空");
    }
    if (targets.size() == 1)
    {
        AppConfig target_config = config;
        target_config.upload_target = targets.front();
        return CreateSingleUploadTarget(target_config, logger);
    }
    return std::make_unique<MultiUploadTarget>(config, logger, targets);
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
        obsidian_config.obsidian_tags = "#算法 cpp;daily/note cpp";
        fs::create_directories(obsidian_config.obsidian_vault_dir);
        ValidateConfigOrThrow(obsidian_config);

        ObsidianTarget obsidian_target(obsidian_config, nullptr);
        UploadJob obsidian_job =
            MakeTestUploadJob("Obsidian 标题\n\n"
                              "每列选 (k) 个。\n\n"
                              "[\n"
                              "{1}\\quad \\text{或}\\quad {1,2}\n"
                              "]\n\n"
                              "复杂度：\n\n"
                              "[\n"
                              "O(n\\log n)\n"
                              "]\n\n"
                              "```text\n"
                              "[\n"
                              "not math\n"
                              "]\n"
                              "```\n",
                              obsidian_config.upload_target);
        bool obsidian_checkpointed = false;
        obsidian_target.ProcessJob(&obsidian_job, [&]
                                   { obsidian_checkpointed = true; });
        const fs::path obsidian_output_path = fs::path(Utf8ToWide(obsidian_job.remote_id));
        if (!obsidian_checkpointed || obsidian_job.remote_progress != 1 || !fs::exists(obsidian_output_path) ||
            obsidian_job.remote_url.empty())
        {
            fail("obsidian target did not write markdown file");
        }
        else
        {
            const std::string obsidian_written = ReadWholeFile(obsidian_output_path);
            if (obsidian_output_path.filename().wstring() != L"Obsidian 标题.md")
            {
                fail("obsidian filename should use the clean note title");
            }
            if (obsidian_written.find("---\ntags:\n  - \"算法\"\n  - \"cpp\"\n  - \"daily/note\"\n---\n\n# Obsidian 标题") != 0 ||
                obsidian_written.find("每列选 $k$ 个。") == std::string::npos ||
                obsidian_written.find("$$\n{1}\\quad \\text{或}\\quad {1,2}\n$$") == std::string::npos ||
                obsidian_written.find("$$\nO(n\\log n)\n$$") == std::string::npos ||
                obsidian_written.find("```text\n[\nnot math\n]\n```") == std::string::npos ||
                obsidian_written.find("\n[\nO(n\\log n)\n]") != std::string::npos ||
                obsidian_written.find("source: notion_clipboard_win") != std::string::npos)
            {
                fail("obsidian output did not normalize loose math for markdown");
            }
        }

        AppConfig multi_config;
        multi_config.upload_target = "markdown_file,obsidian";
        multi_config.state_dir = root / L"multi-state";
        multi_config.markdown_output_dir = root / L"multi-markdown";
        multi_config.obsidian_vault_dir = root / L"multi-vault";
        multi_config.obsidian_folder = "Inbox/Clipboard";
        fs::create_directories(multi_config.obsidian_vault_dir);
        ValidateConfigOrThrow(multi_config);

        std::unique_ptr<UploadTarget> multi_target = CreateUploadTarget(multi_config, nullptr);
        multi_target->Validate();
        UploadJob multi_markdown_job = MakeTestUploadJob("多目标标题\n\n正文 $m$。", "markdown_file");
        UploadJob multi_obsidian_job = MakeTestUploadJob("多目标标题\n\n正文 $m$。", "obsidian");
        multi_target->ProcessJob(&multi_markdown_job, [] {});
        multi_target->ProcessJob(&multi_obsidian_job, [] {});
        if (multi_target->Name() != "markdown_file,obsidian" || multi_markdown_job.remote_progress != 1 ||
            multi_obsidian_job.remote_progress != 1 ||
            !fs::exists(fs::path(Utf8ToWide(multi_markdown_job.remote_id))) ||
            !fs::exists(fs::path(Utf8ToWide(multi_obsidian_job.remote_id))))
        {
            fail("multi upload target did not route jobs independently");
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
        std::cout << "[PASS] webhook upload target payload\n";
        std::cout << "[PASS] yuque upload target payload\n";
        std::cout << "[PASS] feishu_doc upload target payload\n";
    }
    return ok ? 0 : 1;
}
}
