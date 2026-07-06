#include "upload_center.h"

#include "config.h"
#include "json.h"
#include "queue.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace ncw
{
namespace
{
struct RecentRecord
{
    std::string timestamp;
    std::string status;
    std::string target;
    std::string title;
    std::string job_id;
    std::string notion_url;
    std::string notion_page_id;
    std::string obsidian_file;
    std::string local_file_uri;
    std::string obsidian_uri;
    std::string location;
    std::string error;
};

struct QueueRecord
{
    std::string state;
    fs::path path;
    UploadJob job;
    std::string load_error;
};

fs::path RecentUploadResultsPath(const AppConfig &config)
{
    return config.state_dir / L"recent-upload-results.md";
}

fs::path UploadCenterPath(const AppConfig &config)
{
    return config.state_dir / L"upload-center.html";
}

std::string PercentEncodeQueryValue(const std::string &value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size());
    for (unsigned char ch : value)
    {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == '~')
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

std::string ProtocolUrl(const std::string &action, const fs::path &config_path)
{
    return "notion-clipboard-win:/" + action + "/?path=" +
           PercentEncodeQueryValue(WideToUtf8(config_path.wstring()));
}

std::string ProtocolUrlWithParam(const std::string &action, const fs::path &config_path, const std::string &key,
                                 const std::string &value)
{
    return ProtocolUrl(action, config_path) + "&" + PercentEncodeQueryValue(key) + "=" + PercentEncodeQueryValue(value);
}

std::string HtmlEscape(const std::string &text)
{
    std::string output;
    output.reserve(text.size());
    for (char ch : text)
    {
        switch (ch)
        {
        case '&':
            output += "&amp;";
            break;
        case '<':
            output += "&lt;";
            break;
        case '>':
            output += "&gt;";
            break;
        case '"':
            output += "&quot;";
            break;
        default:
            output.push_back(ch);
            break;
        }
    }
    return output;
}

std::string StripAngleLink(std::string value)
{
    value = Trim(std::move(value));
    if (value.size() >= 2 && value.front() == '<' && value.back() == '>')
    {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::vector<std::string> SplitByDelimiter(const std::string &text, const std::string &delimiter)
{
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const std::size_t end = text.find(delimiter, begin);
        parts.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + delimiter.size();
    }
    return parts;
}

void ApplyRecentField(RecentRecord *record, std::string key, std::string value)
{
    key = ToLowerAscii(Trim(std::move(key)));
    value = StripAngleLink(std::move(value));
    if (key == "title")
    {
        record->title = value;
    }
    else if (key == "target")
    {
        record->target = value;
    }
    else if (key == "job")
    {
        record->job_id = value;
    }
    else if (key == "notion url")
    {
        record->notion_url = value;
    }
    else if (key == "notion page id")
    {
        record->notion_page_id = value;
    }
    else if (key == "obsidian file")
    {
        record->obsidian_file = value;
    }
    else if (key == "local file uri")
    {
        record->local_file_uri = value;
    }
    else if (key == "obsidian uri")
    {
        record->obsidian_uri = value;
    }
    else if (key == "location")
    {
        record->location = value;
    }
    else if (key == "error")
    {
        record->error = value;
    }
}

std::vector<RecentRecord> ParseRecentRecords(const fs::path &path)
{
    std::vector<RecentRecord> records;
    if (!fs::exists(path))
    {
        return records;
    }

    std::istringstream input(ReadWholeFile(path));
    std::string line;
    RecentRecord current;
    bool has_current = false;

    auto flush = [&]()
    {
        if (has_current)
        {
            records.push_back(current);
            current = RecentRecord{};
            has_current = false;
        }
    };

    while (std::getline(input, line))
    {
        line = Trim(line);
        if (line.rfind("## ", 0) == 0)
        {
            flush();
            has_current = true;
            const std::vector<std::string> parts = SplitByDelimiter(line.substr(3), " - ");
            if (!parts.empty())
            {
                current.timestamp = Trim(parts[0]);
            }
            if (parts.size() >= 2)
            {
                current.status = Trim(parts[1]);
            }
            if (parts.size() >= 3)
            {
                current.target = Trim(parts[2]);
            }
            continue;
        }

        if (has_current && line.rfind("- ", 0) == 0)
        {
            const std::size_t colon = line.find(':', 2);
            if (colon != std::string::npos)
            {
                ApplyRecentField(&current, line.substr(2, colon - 2), line.substr(colon + 1));
            }
        }
    }
    flush();

    constexpr std::size_t kMaxRecentRows = 80;
    if (records.size() > kMaxRecentRows)
    {
        records.resize(kMaxRecentRows);
    }
    return records;
}

std::string PercentEncodeFileUriPath(const std::string &value)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(value.size());
    for (unsigned char ch : value)
    {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == '~' || ch == '/' || ch == ':')
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

std::string BuildFileUriFromUtf8Path(const std::string &path_text)
{
    if (Trim(path_text).empty())
    {
        return "";
    }

    std::error_code ec;
    fs::path path = fs::path(Utf8ToWide(path_text));
    if (!path.is_absolute())
    {
        path = fs::absolute(path, ec);
        if (ec)
        {
            return "";
        }
    }

    std::string generic = WideToUtf8(path.lexically_normal().generic_wstring());
    if (generic.empty())
    {
        return "";
    }
    if (generic.rfind("//", 0) == 0)
    {
        return "file:" + PercentEncodeFileUriPath(generic);
    }
    if (generic.size() >= 2 && generic[1] == ':')
    {
        return "file:///" + PercentEncodeFileUriPath(generic);
    }
    if (generic.front() == '/')
    {
        return "file://" + PercentEncodeFileUriPath(generic);
    }
    return "file:///" + PercentEncodeFileUriPath(generic);
}

bool LooksLikePath(const std::string &value)
{
    return value.size() >= 3 && ((value[1] == ':' && (value[2] == '\\' || value[2] == '/')) ||
                                 value.rfind("\\\\", 0) == 0 || value.front() == '/');
}

std::string PreferredOpenUrl(const RecentRecord &record)
{
    if (!record.notion_url.empty())
    {
        return record.notion_url;
    }
    if (!record.obsidian_uri.empty())
    {
        return record.obsidian_uri;
    }
    if (!record.local_file_uri.empty())
    {
        return record.local_file_uri;
    }
    if (!record.location.empty())
    {
        if (record.location.rfind("http://", 0) == 0 || record.location.rfind("https://", 0) == 0 ||
            record.location.rfind("obsidian://", 0) == 0 || record.location.rfind("file://", 0) == 0)
        {
            return record.location;
        }
        if (LooksLikePath(record.location))
        {
            return BuildFileUriFromUtf8Path(record.location);
        }
    }
    if (!record.obsidian_file.empty())
    {
        return BuildFileUriFromUtf8Path(record.obsidian_file);
    }
    return "";
}

std::string PreferredCopyValue(const RecentRecord &record)
{
    const std::string url = PreferredOpenUrl(record);
    if (!url.empty())
    {
        return url;
    }
    if (!record.obsidian_file.empty())
    {
        return record.obsidian_file;
    }
    if (!record.notion_page_id.empty())
    {
        return record.notion_page_id;
    }
    return "";
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

UploadJob JobFromJsonForCenter(const std::string &text)
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
    job.target = JsonStringOrEmpty(json.find("target"));
    job.hash = JsonStringOrEmpty(json.find("hash"));
    job.title = JsonStringOrEmpty(json.find("title"));
    job.content = JsonStringOrEmpty(json.find("content"));
    job.remote_id = JsonStringOrEmpty(json.find("remote_id"));
    job.remote_url = JsonStringOrEmpty(json.find("remote_url"));
    job.remote_progress = JsonNumberAsSize(json.find("remote_progress"), 0);
    job.last_error = JsonStringOrEmpty(json.find("last_error"));
    if (job.id.empty())
    {
        throw std::runtime_error("任务文件缺少 id");
    }
    return job;
}

std::string JobToJsonForRetry(const UploadJob &job)
{
    std::ostringstream json;
    json << "{\n"
         << "  \"id\":\"" << EscapeJson(job.id) << "\",\n"
         << "  \"created_at_ms\":" << job.created_at_ms << ",\n"
         << "  \"not_before_ms\":0,\n"
         << "  \"attempts\":0,\n"
         << "  \"target\":\"" << EscapeJson(job.target) << "\",\n"
         << "  \"hash\":\"" << EscapeJson(job.hash) << "\",\n"
         << "  \"title\":\"" << EscapeJson(job.title) << "\",\n"
         << "  \"content\":\"" << EscapeJson(job.content) << "\",\n"
         << "  \"remote_id\":\"" << EscapeJson(job.remote_id) << "\",\n"
         << "  \"remote_url\":\"" << EscapeJson(job.remote_url) << "\",\n"
         << "  \"remote_progress\":" << static_cast<unsigned long long>(job.remote_progress) << ",\n"
         << "  \"last_error\":\"\"\n"
         << "}\n";
    return json.str();
}

std::vector<fs::path> ListJobFiles(const fs::path &dir)
{
    std::vector<fs::path> files;
    if (!fs::exists(dir))
    {
        return files;
    }
    for (const fs::directory_entry &entry : fs::directory_iterator(dir))
    {
        if (entry.is_regular_file() && entry.path().extension() == L".job")
        {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::vector<QueueRecord> LoadQueueRecords(const AppConfig &config, const wchar_t *folder, const std::string &state)
{
    std::vector<QueueRecord> records;
    const fs::path dir = config.state_dir / folder;
    for (const fs::path &path : ListJobFiles(dir))
    {
        QueueRecord record;
        record.state = state;
        record.path = path;
        try
        {
            record.job = JobFromJsonForCenter(ReadWholeFile(path));
        }
        catch (const std::exception &ex)
        {
            record.job.id = WideToUtf8(path.filename().wstring());
            record.job.title = "(无法读取任务)";
            record.load_error = ex.what();
        }
        records.push_back(std::move(record));
    }

    std::sort(records.begin(), records.end(), [](const QueueRecord &a, const QueueRecord &b)
              {
                  if (a.job.created_at_ms != b.job.created_at_ms)
                  {
                      return a.job.created_at_ms > b.job.created_at_ms;
                  }
                  return a.path.wstring() < b.path.wstring();
              });
    constexpr std::size_t kMaxQueueRows = 120;
    if (records.size() > kMaxQueueRows)
    {
        records.resize(kMaxQueueRows);
    }
    return records;
}

fs::path UniqueQueuePath(const fs::path &queue_dir, const fs::path &source_path, const std::string &job_id)
{
    fs::path candidate = queue_dir / source_path.filename();
    if (!fs::exists(candidate))
    {
        return candidate;
    }

    const std::wstring stem = source_path.stem().wstring();
    for (int i = 1; i <= 1000; ++i)
    {
        candidate = queue_dir / (stem + L"-retry-" + std::to_wstring(NowUnixMs()) + L"-" + std::to_wstring(i) + L".job");
        if (!fs::exists(candidate))
        {
            return candidate;
        }
    }
    return queue_dir / (Utf8ToWide(job_id) + L"-retry.job");
}

bool IsSafeJobFilename(const std::string &filename)
{
    if (Trim(filename).empty())
    {
        return false;
    }
    const fs::path path = fs::path(Utf8ToWide(filename));
    return path.filename() == path && path.extension() == L".job";
}

std::string DisplayTime(std::uint64_t unix_ms)
{
    return unix_ms == 0 ? "" : IsoUtcTimestampFromUnixMs(unix_ms);
}

std::string QueueLocation(const UploadJob &job)
{
    if (job.target == "obsidian" && !job.remote_id.empty())
    {
        return job.remote_id;
    }
    return job.remote_url.empty() ? job.remote_id : job.remote_url;
}

std::string DisplayTargetName(const std::string &target)
{
    if (target == "notion")
    {
        return "Notion";
    }
    if (target == "obsidian")
    {
        return "Obsidian";
    }
    if (target == "markdown_file")
    {
        return "Markdown 文件";
    }
    if (target == "webhook")
    {
        return "Webhook";
    }
    if (target == "yuque")
    {
        return "语雀";
    }
    if (target == "feishu_doc")
    {
        return "飞书文档";
    }
    if (target == "configuration")
    {
        return "配置测试";
    }
    return target;
}

std::string DisplayRecentStatus(const std::string &status)
{
    const std::string normalized = ToLowerAscii(Trim(status));
    if (normalized == "success")
    {
        return "成功";
    }
    if (normalized == "failed")
    {
        return "失败";
    }
    return status.empty() ? "未知" : status;
}

void AppendCopyButton(std::ostringstream *html, const std::string &value, const std::string &label)
{
    if (Trim(value).empty())
    {
        return;
    }
    *html << "<button type=\"button\" data-copy=\"" << HtmlEscape(value) << "\">" << HtmlEscape(label) << "</button>";
}

void AppendOpenLink(std::ostringstream *html, const std::string &url, const std::string &label)
{
    if (Trim(url).empty())
    {
        return;
    }
    *html << "<a class=\"button\" href=\"" << HtmlEscape(url) << "\">" << HtmlEscape(label) << "</a>";
}

void AppendConfirmLink(std::ostringstream *html, const std::string &url, const std::string &label,
                       const std::string &message)
{
    if (Trim(url).empty())
    {
        return;
    }
    *html << "<a class=\"button\" href=\"" << HtmlEscape(url) << "\" onclick=\"return confirm('"
          << HtmlEscape(message) << "')\">" << HtmlEscape(label) << "</a>";
}

void AppendRecentTable(std::ostringstream *html, const std::vector<RecentRecord> &records)
{
    *html << "<section><div class=\"section-head\"><h2>最近保存</h2><span>" << records.size()
          << " 条</span></div>";
    if (records.empty())
    {
        *html << "<p class=\"empty\">还没有保存记录。</p></section>";
        return;
    }

    *html << "<div class=\"table-wrap\"><table><thead><tr><th>状态</th><th>时间</th><th>目标</th>"
             "<th>标题</th><th>位置</th><th>操作</th></tr></thead><tbody>";
    for (const RecentRecord &record : records)
    {
        const std::string open_url = PreferredOpenUrl(record);
        const std::string copy_value = PreferredCopyValue(record);
        const std::string location = !record.notion_url.empty()       ? record.notion_url
                                     : !record.obsidian_file.empty()  ? record.obsidian_file
                                     : !record.location.empty()       ? record.location
                                     : !record.notion_page_id.empty() ? record.notion_page_id
                                                                      : "";
        const std::string row_class = ToLowerAscii(record.status) == "success" ? "ok" : "bad";
        *html << "<tr><td><span class=\"pill " << row_class << "\">" << HtmlEscape(DisplayRecentStatus(record.status))
              << "</span>";
        if (!record.error.empty())
        {
            *html << "<div class=\"error\">" << HtmlEscape(record.error) << "</div>";
        }
        *html << "</td><td>" << HtmlEscape(record.timestamp) << "</td><td>" << HtmlEscape(DisplayTargetName(record.target))
              << "</td><td>" << HtmlEscape(record.title) << "</td><td class=\"location\">" << HtmlEscape(location)
              << "</td><td class=\"actions\">";
        AppendOpenLink(html, open_url, "打开");
        AppendCopyButton(html, copy_value, "复制");
        *html << "</td></tr>";
    }
    *html << "</tbody></table></div></section>";
}

void AppendQueueTable(std::ostringstream *html, const std::vector<QueueRecord> &records, const fs::path &config_path)
{
    *html << "<section><div class=\"section-head\"><h2>队列</h2><span>" << records.size() << " 条</span></div>";
    if (records.empty())
    {
        *html << "<p class=\"empty\">当前没有等待重试或最终失败的任务。</p></section>";
        return;
    }

    *html << "<div class=\"table-wrap\"><table><thead><tr><th>状态</th><th>下次重试</th><th>目标</th>"
             "<th>标题</th><th>已重试</th><th>错误</th><th>操作</th></tr></thead><tbody>";
    for (const QueueRecord &record : records)
    {
        const std::string location = QueueLocation(record.job);
        const std::string error = record.load_error.empty() ? record.job.last_error : record.load_error;
        *html << "<tr><td><span class=\"pill queue\">" << HtmlEscape(record.state) << "</span></td><td>"
              << HtmlEscape(DisplayTime(record.job.not_before_ms)) << "</td><td>"
              << HtmlEscape(DisplayTargetName(record.job.target)) << "</td><td>" << HtmlEscape(record.job.title);
        if (!location.empty())
        {
            *html << "<div class=\"muted\">" << HtmlEscape(location) << "</div>";
        }
        *html << "</td><td>" << record.job.attempts << "</td><td class=\"error\">" << HtmlEscape(error)
              << "</td><td class=\"actions\">";
        if (record.state == "最终失败" && record.load_error.empty())
        {
            const std::string file_name = WideToUtf8(record.path.filename().wstring());
            AppendConfirmLink(html, ProtocolUrlWithParam("retry-failed-job", config_path, "file", file_name),
                              "重试此项", "将这个任务移回等待队列并立即重试。继续吗？");
        }
        else
        {
            *html << "<span class=\"muted\">"
                  << (record.load_error.empty() ? "等待自动重试" : "无法重试") << "</span>";
        }
        *html << "</td></tr>";
    }
    *html << "</tbody></table></div></section>";
}
}

std::size_t RetryFailedUploads(const AppConfig &config)
{
    const fs::path failed_dir = config.state_dir / L"failed";
    const fs::path queue_dir = config.state_dir / L"queue";
    fs::create_directories(queue_dir);

    std::size_t retried = 0;
    for (const fs::path &path : ListJobFiles(failed_dir))
    {
        try
        {
            UploadJob job = JobFromJsonForCenter(ReadWholeFile(path));
            const fs::path queue_path = UniqueQueuePath(queue_dir, path, job.id);
            AtomicWriteFile(queue_path, JobToJsonForRetry(job));
            std::error_code ignored;
            fs::remove(path, ignored);
            ++retried;
        }
        catch (...)
        {
            // Corrupt failed jobs stay in failed/ so the upload center can still show them.
        }
    }
    return retried;
}

std::size_t RetryFailedUpload(const AppConfig &config, const std::string &failed_job_filename)
{
    if (!IsSafeJobFilename(failed_job_filename))
    {
        return 0;
    }

    const fs::path failed_path = config.state_dir / L"failed" / fs::path(Utf8ToWide(failed_job_filename));
    if (!fs::exists(failed_path) || !fs::is_regular_file(failed_path))
    {
        return 0;
    }

    fs::create_directories(config.state_dir / L"queue");
    UploadJob job = JobFromJsonForCenter(ReadWholeFile(failed_path));
    const fs::path queue_path = UniqueQueuePath(config.state_dir / L"queue", failed_path, job.id);
    AtomicWriteFile(queue_path, JobToJsonForRetry(job));
    std::error_code ignored;
    fs::remove(failed_path, ignored);
    return 1;
}

std::filesystem::path WriteUploadCenterPage(const AppConfig &config, const std::filesystem::path &config_path)
{
    std::filesystem::create_directories(config.state_dir);
    const fs::path output_path = UploadCenterPath(config);
    const fs::path recent_path = RecentUploadResultsPath(config);
    const std::vector<RecentRecord> recent_records = ParseRecentRecords(recent_path);
    std::vector<QueueRecord> queue_records = LoadQueueRecords(config, L"queue", "等待重试");
    std::vector<QueueRecord> failed_records = LoadQueueRecords(config, L"failed", "最终失败");
    queue_records.insert(queue_records.end(), failed_records.begin(), failed_records.end());

    std::sort(queue_records.begin(), queue_records.end(), [](const QueueRecord &a, const QueueRecord &b)
              {
                  if (a.state != b.state)
                  {
                      return a.state < b.state;
                  }
                  if (a.job.created_at_ms != b.job.created_at_ms)
                  {
                      return a.job.created_at_ms > b.job.created_at_ms;
                  }
                  return a.job.id < b.job.id;
              });

    const std::string refresh_url = ProtocolUrl("open-upload-center", config_path);
    const std::string retry_failed_url = ProtocolUrl("retry-failed-uploads", config_path);
    const std::size_t success_count = std::count_if(recent_records.begin(), recent_records.end(), [](const RecentRecord &record)
                                                    { return ToLowerAscii(record.status) == "success"; });
    const std::size_t failed_count = std::count_if(recent_records.begin(), recent_records.end(), [](const RecentRecord &record)
                                                   { return ToLowerAscii(record.status) == "failed"; });

    std::ostringstream html;
    html << R"(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Notion Clipboard Win 保存记录</title>
<style>
:root{color-scheme:light dark;--bg:#f5f6f8;--panel:#fff;--text:#172033;--muted:#667085;--line:#d9dee8;--accent:#1f6feb;--ok:#047857;--bad:#b42318;--queue:#7c3aed}
@media (prefers-color-scheme:dark){:root{--bg:#111827;--panel:#182233;--text:#edf2f7;--muted:#9aa8bd;--line:#324055;--accent:#5aa2ff;--ok:#34d399;--bad:#f87171;--queue:#c084fc}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 "Segoe UI",system-ui,sans-serif}
header{border-bottom:1px solid var(--line);background:var(--panel)}.bar{max-width:1180px;margin:auto;padding:16px 20px;display:flex;justify-content:space-between;gap:16px;align-items:center}
h1{font-size:20px;margin:0}.path{font-size:12px;color:var(--muted);word-break:break-all}.wrap{max-width:1180px;margin:0 auto;padding:20px}
.toolbar,.metrics{display:flex;gap:8px;flex-wrap:wrap;align-items:center}.metrics{margin-bottom:14px}.metric{border:1px solid var(--line);background:var(--panel);border-radius:8px;padding:10px 12px;min-width:120px}.metric strong{display:block;font-size:20px}.metric span{color:var(--muted);font-size:12px}
section{background:var(--panel);border:1px solid var(--line);border-radius:8px;margin:0 0 16px;padding:14px}.section-head{display:flex;justify-content:space-between;gap:12px;align-items:center;margin-bottom:10px}.section-head h2{font-size:15px;margin:0}.section-head span,.muted{color:var(--muted);font-size:12px}
.button,button{border:0;border-radius:6px;background:var(--accent);color:#fff;padding:7px 10px;font-weight:600;text-decoration:none;cursor:pointer;display:inline-block}.actions{display:flex;gap:6px;flex-wrap:wrap}
.table-wrap{overflow:auto}table{width:100%;border-collapse:collapse;min-width:860px}th,td{border-top:1px solid var(--line);padding:9px 8px;text-align:left;vertical-align:top}th{font-size:12px;color:var(--muted);font-weight:700}td.location{max-width:340px;word-break:break-all}.pill{display:inline-block;border-radius:999px;padding:2px 8px;font-size:12px;font-weight:700;background:var(--line);color:var(--text)}.pill.ok{background:color-mix(in srgb,var(--ok) 14%,transparent);color:var(--ok)}.pill.bad{background:color-mix(in srgb,var(--bad) 14%,transparent);color:var(--bad)}.pill.queue{background:color-mix(in srgb,var(--queue) 14%,transparent);color:var(--queue)}
.error{color:var(--bad);font-size:12px;word-break:break-word}.empty{color:var(--muted);margin:0}.notice{color:var(--muted);font-size:12px;margin:0 0 14px}
@media (max-width:760px){.bar{align-items:flex-start;flex-direction:column}.metric{flex:1 1 40%}}
</style>
</head>
<body>
<header><div class="bar"><div><h1>保存记录</h1><div class="path">本地保存记录和重试队列</div></div><div class="toolbar"><a class="button" href=")"
         << HtmlEscape(refresh_url) << R"(">刷新状态</a><a class="button" href=")"
         << HtmlEscape(retry_failed_url)
         << R"HTML(" onclick="return confirm('将失败任务移回等待队列并立即重试。继续吗？')">重试失败任务</a></div></div></header>
<main class="wrap">
<div class="metrics"><div class="metric"><strong>)HTML"
         << recent_records.size() << R"(</strong><span>最近记录</span></div><div class="metric"><strong>)"
         << success_count << R"(</strong><span>成功</span></div><div class="metric"><strong>)"
         << failed_count << R"(</strong><span>失败记录</span></div><div class="metric"><strong>)"
         << queue_records.size() << R"(</strong><span>队列任务</span></div></div>
<p class="notice">页面是打开时生成的本地快照，不会在后台轮询；点击“刷新状态”可重新生成最新页面。</p>
)";

    AppendRecentTable(&html, recent_records);
    AppendQueueTable(&html, queue_records, config_path);

    html << R"(
</main>
<script>
document.querySelectorAll("[data-copy]").forEach(button=>button.addEventListener("click",async()=>{try{await navigator.clipboard.writeText(button.dataset.copy||""); const old=button.textContent; button.textContent="已复制"; setTimeout(()=>button.textContent=old,1200);}catch(e){button.textContent="复制失败";}}));
</script>
</body></html>
)";

    AtomicWriteFile(output_path, html.str());
    return output_path;
}

int RunUploadCenterSelfTest()
{
    bool ok = true;
    auto fail = [&](const std::string &message)
    {
        std::cout << "[FAIL] upload center self-test: " << message << "\n";
        ok = false;
    };

    const fs::path root = fs::temp_directory_path() / (L"notion-clipboard-win-upload-center-test-" +
                                                       std::to_wstring(NowUnixMs()));
    std::error_code ignored;
    fs::remove_all(root, ignored);

    try
    {
        AppConfig config;
        config.state_dir = root / L"state";
        fs::create_directories(config.state_dir / L"queue");
        fs::create_directories(config.state_dir / L"failed");

        AtomicWriteFile(config.state_dir / L"recent-upload-results.md",
                        "# Recent Upload Results\n\n"
                        "## 2026-07-05T00:00:00Z - SUCCESS - notion\n\n"
                        "- Title: Upload Center Title\n"
                        "- Target: notion\n"
                        "- Job: job-1\n"
                        "- Notion URL: <https://www.notion.so/page>\n\n"
                        "## 2026-07-05T00:01:00Z - FAILED - obsidian\n\n"
                        "- Title: Failed Title\n"
                        "- Target: obsidian\n"
                        "- Job: job-2\n"
                        "- Error: vault missing\n\n");

        AtomicWriteFile(config.state_dir / L"queue" / L"queued.job",
                        "{\n"
                        "  \"id\":\"queued-job\",\n"
                        "  \"created_at_ms\":1783209600000,\n"
                        "  \"not_before_ms\":1783209660000,\n"
                        "  \"attempts\":1,\n"
                        "  \"target\":\"notion\",\n"
                        "  \"title\":\"Queued Title\",\n"
                        "  \"content\":\"Queued Body\",\n"
                        "  \"last_error\":\"temporary error\"\n"
                        "}\n");
        AtomicWriteFile(config.state_dir / L"failed" / L"failed.job",
                        "{\n"
                        "  \"id\":\"failed-job\",\n"
                        "  \"created_at_ms\":1783209500000,\n"
                        "  \"attempts\":3,\n"
                        "  \"target\":\"obsidian\",\n"
                        "  \"title\":\"Failed Queue Title\",\n"
                        "  \"content\":\"Failed Body\",\n"
                        "  \"last_error\":\"permanent error\"\n"
                        "}\n");
        AtomicWriteFile(config.state_dir / L"failed" / L"failed-two.job",
                        "{\n"
                        "  \"id\":\"failed-two-job\",\n"
                        "  \"created_at_ms\":1783209400000,\n"
                        "  \"attempts\":2,\n"
                        "  \"target\":\"notion\",\n"
                        "  \"title\":\"Second Failed Queue Title\",\n"
                        "  \"content\":\"Second Failed Body\",\n"
                        "  \"last_error\":\"another permanent error\"\n"
                        "}\n");

        const fs::path page = WriteUploadCenterPage(config, root / L"notion_clipboard_win.ini");
        const std::string html = ReadWholeFile(page);
        for (const char *needle : {"保存记录", "Upload Center Title", "https://www.notion.so/page", "vault missing",
                                    "Queued Title", "temporary error", "Failed Queue Title", "permanent error",
                                    "页面是打开时生成的本地快照", "data-copy=", "本地保存记录和重试队列",
                                    "open-upload-center", "刷新状态", "retry-failed-uploads", "重试失败任务",
                                    "retry-failed-job", "重试此项", "成功", "失败", ">Notion</td>",
                                    ">Obsidian</td>", "已重试", "等待自动重试",
                                    "将失败任务移回等待队列并立即重试。继续吗？"})
        {
            if (html.find(needle) == std::string::npos)
            {
                fail(std::string("missing expected upload center content: ") + needle);
            }
        }
        for (const char *needle : {"打开状态目录", "原始报告", "上传中心", "最近上传", "还没有上传记录",
                                   "SUCCESS</span>", "FAILED</span>", ">notion</td>", ">obsidian</td>",
                                   "failed 目录", "打开任务", "复制 ID", "queued-job", "failed-two-job"})
        {
            if (html.find(needle) != std::string::npos)
            {
                fail(std::string("found debug-only upload center content: ") + needle);
            }
        }

        if (RetryFailedUpload(config, "../failed.job") != 0)
        {
            fail("retry failed upload accepted unsafe filename");
        }
        const std::size_t single_retried = RetryFailedUpload(config, "failed.job");
        if (single_retried != 1 || !fs::exists(config.state_dir / L"queue" / L"failed.job") ||
            fs::exists(config.state_dir / L"failed" / L"failed.job") ||
            !fs::exists(config.state_dir / L"failed" / L"failed-two.job"))
        {
            fail("retry failed upload did not move one failed job to queue");
        }
        const std::string single_retried_job = ReadWholeFile(config.state_dir / L"queue" / L"failed.job");
        if (single_retried_job.find("\"attempts\":0") == std::string::npos ||
            single_retried_job.find("\"not_before_ms\":0") == std::string::npos ||
            single_retried_job.find("\"last_error\":\"\"") == std::string::npos)
        {
            fail("retry failed upload did not reset retry metadata");
        }

        const std::size_t retried = RetryFailedUploads(config);
        if (retried != 1 || !fs::exists(config.state_dir / L"queue" / L"failed-two.job") ||
            fs::exists(config.state_dir / L"failed" / L"failed-two.job"))
        {
            fail("retry failed uploads did not move remaining failed job to queue");
        }
        const std::string retried_job = ReadWholeFile(config.state_dir / L"queue" / L"failed-two.job");
        if (retried_job.find("\"attempts\":0") == std::string::npos ||
            retried_job.find("\"not_before_ms\":0") == std::string::npos ||
            retried_job.find("\"last_error\":\"\"") == std::string::npos)
        {
            fail("retry failed uploads did not reset retry metadata");
        }
    }
    catch (const std::exception &ex)
    {
        fail(ex.what());
    }

    fs::remove_all(root, ignored);
    if (ok)
    {
        std::cout << "[PASS] upload center page\n";
    }
    return ok ? 0 : 1;
}
}
