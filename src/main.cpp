#include <windows.h>
#include <shellapi.h>

#include "app_icon.h"
#include "autostart.h"
#include "config.h"
#include "config_page.h"
#include "converter.h"
#include "hotkey.h"
#include "logger.h"
#include "obsidian.h"
#include "queue.h"
#include "resource.h"
#include "upload_center.h"
#include "upload_target.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
using ncw::CreateGeneratedAppIcon;
using ncw::LastErrorMessage;
using ncw::AppConfig;
using ncw::AtomicWriteFile;
using ncw::BuildObsidianMarkdownPreview;
using ncw::BuildTextBlocks;
using ncw::BuildTitleFromContent;
using ncw::CliOptions;
using ncw::CreateUploadTarget;
using ncw::CurrentHotkeyModifiers;
using ncw::ExtractCfHtmlFragment;
using ncw::Fnv1a64;
using ncw::HasEmptyMarkdownCodeFenceArtifact;
using ncw::Hex64;
using ncw::HtmlFragmentToMarkdown;
using ncw::HotkeySpec;
using ncw::HotkeySpecFromRecordedKey;
using ncw::IsoUtcTimestampFromUnixMs;
using ncw::IsAutoStartEnabled;
using ncw::IsModifierVirtualKey;
using ncw::Logger;
using ncw::LoadConfig;
using ncw::ModuleDirectory;
using ncw::NowUnixMs;
using ncw::NormalizeLineEndings;
using ncw::ParseHotkeyOrThrow;
using ncw::ParseCli;
using ncw::ParseUploadTargets;
using ncw::PrintHelp;
using ncw::PersistentQueue;
using ncw::ReadWholeFile;
using ncw::RunDryRunText;
using ncw::RunConfigPageSelfTest;
using ncw::RunObsidianSelfTest;
using ncw::RunSelfTest;
using ncw::RunUploadCenterSelfTest;
using ncw::RunUploadTargetSelfTest;
using ncw::RetryFailedUpload;
using ncw::RetryFailedUploads;
using ncw::SetAutoStartEnabled;
using ncw::Trim;
using ncw::ToLowerAscii;
using ncw::UpsertConfigValue;
using ncw::UploadFailure;
using ncw::UploadJob;
using ncw::UploadTarget;
using ncw::Utf8ToWide;
using ncw::ValidateConfigOrThrow;
using ncw::WideToUtf8;
using ncw::WriteConfigPage;
using ncw::WriteUploadCenterPage;

constexpr UINT_PTR kClipboardDebounceTimer = 1001;
constexpr UINT kUploadHotkeyId = 2001;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kUploadResultMessage = WM_APP + 2;
constexpr UINT kWakeUploadWorkerMessage = WM_APP + 3;
constexpr UINT kMenuUploadNow = 3001;
constexpr UINT kMenuHotkeyStatus = 3002;
constexpr UINT kMenuToggleHotkey = 3003;
constexpr UINT kMenuRecordHotkey = 3004;
constexpr UINT kMenuToggleNotifications = 3005;
constexpr UINT kMenuToggleAutoStart = 3006;
constexpr UINT kMenuToggleClipboardListener = 3007;
constexpr UINT kMenuOpenConfig = 3008;
constexpr UINT kMenuOpenLog = 3009;
constexpr UINT kMenuOpenStateDir = 3010;
constexpr UINT kMenuExit = 3011;
constexpr UINT kMenuOpenConfigPage = 3012;
constexpr UINT kMenuOpenRecentUploads = 3013;
constexpr UINT kMenuValidateConfig = 3014;
constexpr UINT kMenuOpenConfigDiagnostics = 3015;
constexpr UINT kMenuOpenLastObsidian = 3016;
constexpr UINT kMenuPreviewObsidian = 3017;
constexpr const wchar_t *kAppDisplayName = L"Notion Clipboard Win";

#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif

struct UploadResultNotice
{
    std::wstring title;
    std::wstring message;
};

std::wstring TruncateWide(std::wstring text, std::size_t max_chars)
{
    if (text.size() <= max_chars)
    {
        return text;
    }
    if (max_chars <= 3)
    {
        return text.substr(0, max_chars);
    }
    return text.substr(0, max_chars - 3) + L"...";
}

std::wstring UploadTargetDisplayName(const std::string &target)
{
    if (target == "notion")
    {
        return L"Notion";
    }
    if (target == "obsidian")
    {
        return L"Obsidian";
    }
    if (target == "markdown_file")
    {
        return L"Markdown 文件";
    }
    if (target == "webhook")
    {
        return L"Webhook";
    }
    if (target == "yuque")
    {
        return L"语雀";
    }
    if (target == "feishu_doc")
    {
        return L"飞书文档";
    }
    return Utf8ToWide(target);
}

std::wstring JoinTargetDisplayNames(const std::vector<std::string> &targets)
{
    std::wstring joined;
    for (const std::string &target : targets)
    {
        if (!joined.empty())
        {
            joined += L"、";
        }
        joined += UploadTargetDisplayName(target);
    }
    return joined;
}

HotkeySpec DefaultHotkeySpec()
{
    return ParseHotkeyOrThrow("Ctrl+Shift+B");
}

std::string UploadJobLocation(const UploadJob &job)
{
    if (job.target == "obsidian" && !job.remote_id.empty())
    {
        return job.remote_id;
    }
    return job.remote_url.empty() ? job.remote_id : job.remote_url;
}

std::string ReportLineValue(std::string value)
{
    value = NormalizeLineEndings(std::move(value));
    std::replace(value.begin(), value.end(), '\n', ' ');
    return Trim(value);
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
    std::filesystem::path path = std::filesystem::path(Utf8ToWide(path_text));
    if (!path.is_absolute())
    {
        path = std::filesystem::absolute(path, ec);
        if (ec)
        {
            return "";
        }
    }

    std::string generic = WideToUtf8(path.generic_wstring());
    std::replace(generic.begin(), generic.end(), '\\', '/');
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

std::filesystem::path RecentUploadResultsPath(const AppConfig &config)
{
    return config.state_dir / L"recent-upload-results.md";
}

std::filesystem::path LastObsidianUploadPath(const AppConfig &config)
{
    return config.state_dir / L"last-obsidian-upload.ini";
}

std::filesystem::path ObsidianPreviewPath(const AppConfig &config)
{
    return config.state_dir / L"obsidian-preview.md";
}

std::filesystem::path ConfigDiagnosticsPath(const AppConfig &config)
{
    return config.state_dir / L"config-diagnostics.md";
}

void AppendUploadResultLocationFields(std::ostringstream *entry, const UploadJob &job)
{
    if (job.target == "notion")
    {
        if (!job.remote_url.empty())
        {
            *entry << "- Notion URL: <" << ReportLineValue(job.remote_url) << ">\n";
        }
        if (!job.remote_id.empty())
        {
            *entry << "- Notion Page ID: " << ReportLineValue(job.remote_id) << "\n";
        }
        return;
    }

    if (job.target == "obsidian")
    {
        if (!job.remote_id.empty())
        {
            *entry << "- Obsidian File: " << ReportLineValue(job.remote_id) << "\n";
            const std::string file_uri = BuildFileUriFromUtf8Path(job.remote_id);
            if (!file_uri.empty())
            {
                *entry << "- Local File URI: <" << file_uri << ">\n";
            }
        }
        if (!job.remote_url.empty() && job.remote_url.rfind("obsidian://", 0) == 0)
        {
            *entry << "- Obsidian URI: <" << ReportLineValue(job.remote_url) << ">\n";
        }
        else if (!job.remote_url.empty() && job.remote_url != job.remote_id)
        {
            *entry << "- Location: " << ReportLineValue(job.remote_url) << "\n";
        }
        return;
    }

    const std::string location = UploadJobLocation(job);
    if (!location.empty())
    {
        *entry << "- Location: " << ReportLineValue(location) << "\n";
    }
}

void WriteRecentUploadResultReport(const std::filesystem::path &path, const UploadJob &job, bool success,
                                   const std::string &detail)
{
    std::ostringstream entry;
    entry << "## " << IsoUtcTimestampFromUnixMs(NowUnixMs()) << " - " << (success ? "SUCCESS" : "FAILED") << " - "
          << ReportLineValue(job.target) << "\n\n"
          << "- Title: " << ReportLineValue(job.title) << "\n"
          << "- Target: " << ReportLineValue(job.target) << "\n"
          << "- Job: " << ReportLineValue(job.id) << "\n";

    AppendUploadResultLocationFields(&entry, job);
    if (!success && !detail.empty())
    {
        entry << "- Error: " << ReportLineValue(detail) << "\n";
    }
    entry << "\n";

    std::string previous;
    try
    {
        if (fs::exists(path))
        {
            previous = ReadWholeFile(path);
        }
    }
    catch (...)
    {
        previous.clear();
    }

    constexpr std::size_t kMaxRecentUploadReportBytes = 128ull * 1024ull;
    const std::string heading = "# Recent Upload Results\n\n";
    if (previous.rfind(heading, 0) == 0)
    {
        previous.erase(0, heading.size());
    }
    std::string content = heading + entry.str() + previous;
    if (content.size() > kMaxRecentUploadReportBytes)
    {
        content.resize(kMaxRecentUploadReportBytes);
    }
    AtomicWriteFile(path, content);
}

void WriteLastObsidianUploadState(const std::filesystem::path &path, const UploadJob &job)
{
    if (job.target != "obsidian" || job.remote_id.empty())
    {
        return;
    }

    std::ostringstream content;
    content << "updated=" << IsoUtcTimestampFromUnixMs(NowUnixMs()) << "\n"
            << "title=" << ReportLineValue(job.title) << "\n"
            << "file=" << ReportLineValue(job.remote_id) << "\n";
    if (!job.remote_url.empty() && job.remote_url.rfind("obsidian://", 0) == 0)
    {
        content << "uri=" << ReportLineValue(job.remote_url) << "\n";
    }
    AtomicWriteFile(path, content.str());
}

std::optional<std::string> ReadStateValue(const std::filesystem::path &path, const std::string &key)
{
    if (!std::filesystem::exists(path))
    {
        return std::nullopt;
    }

    const std::string normalized_key = ToLowerAscii(Trim(key));
    std::istringstream input(ReadWholeFile(path));
    std::string line;
    while (std::getline(input, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
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
            return Trim(line.substr(eq + 1));
        }
    }
    return std::nullopt;
}

void AppendTargetConfigSummary(std::ostringstream *report, const AppConfig &config)
{
    if (config.upload_target == "notion")
    {
        *report << "- Token: " << (config.notion_token.empty() ? "missing" : "present") << "\n"
                << "- Data Source ID: " << (config.data_source_id.empty() ? "(empty)" : ReportLineValue(config.data_source_id))
                << "\n"
                << "- Database ID: " << (config.database_id.empty() ? "(empty)" : ReportLineValue(config.database_id))
                << "\n";
        return;
    }
    if (config.upload_target == "obsidian")
    {
        *report << "- Vault: " << ReportLineValue(WideToUtf8(config.obsidian_vault_dir.wstring())) << "\n"
                << "- Folder: " << (config.obsidian_folder.empty() ? "(vault root)" : ReportLineValue(config.obsidian_folder))
                << "\n"
                << "- Tags: " << (Trim(config.obsidian_tags).empty() ? "(none)" : ReportLineValue(config.obsidian_tags))
                << "\n";
        return;
    }
}

bool WriteConfigDiagnosticsReport(const std::filesystem::path &path, const AppConfig &config, Logger *logger)
{
    const std::vector<std::string> targets = ParseUploadTargets(config.upload_target);
    bool all_ok = !targets.empty();

    std::ostringstream report;
    report << "# Configuration Diagnostics\n\n"
           << "- Generated: " << IsoUtcTimestampFromUnixMs(NowUnixMs()) << "\n"
           << "- Upload target: " << ReportLineValue(config.upload_target) << "\n"
           << "- State dir: " << ReportLineValue(WideToUtf8(config.state_dir.wstring())) << "\n\n";

    if (targets.empty())
    {
        report << "## Overall\n\n- Status: FAILED\n- Error: upload_target is empty\n\n";
        AtomicWriteFile(path, report.str());
        return false;
    }

    for (const std::string &target : targets)
    {
        AppConfig target_config = config;
        target_config.upload_target = target;

        report << "## " << ReportLineValue(target) << "\n\n";
        AppendTargetConfigSummary(&report, target_config);

        try
        {
            ValidateConfigOrThrow(target_config);
            std::unique_ptr<UploadTarget> upload_target = CreateUploadTarget(target_config, logger);
            upload_target->Validate();
            report << "- Status: OK\n\n";
        }
        catch (const std::exception &ex)
        {
            all_ok = false;
            report << "- Status: FAILED\n"
                   << "- Error: " << ReportLineValue(ex.what()) << "\n\n";
        }
    }

    AtomicWriteFile(path, report.str());
    return all_ok;
}

HICON LoadApplicationIcon(HINSTANCE instance, int width, int height)
{
    HICON icon = reinterpret_cast<HICON>(
        LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, width, height, LR_DEFAULTCOLOR));
    if (icon != nullptr)
    {
        return icon;
    }
    return CreateGeneratedAppIcon(width, height);
}

std::atomic<std::uint64_t> g_job_counter{0};

UploadJob MakeUploadJob(const std::string &content, const std::string &target)
{
    UploadJob job;
    job.created_at_ms = NowUnixMs();
    job.not_before_ms = 0;
    job.target = target;
    job.hash = Hex64(Fnv1a64(content));
    job.title = BuildTitleFromContent(content);
    job.content = content;

    std::ostringstream id;
    id << job.created_at_ms << "-" << job.hash.substr(0, 12) << "-" << GetCurrentProcessId() << "-"
       << g_job_counter.fetch_add(1);
    job.id = id.str();
    return job;
}

std::string BuildConfigTestUploadContent(const AppConfig &config)
{
    std::ostringstream content;
    content << "Notion Clipboard Win 测试上传\n\n"
            << "这是一条由配置页面发出的测试内容，用来确认当前上传目标可以正常写入。\n\n"
            << "- 时间: " << IsoUtcTimestampFromUnixMs(NowUnixMs()) << "\n"
            << "- 上传目标: " << ReportLineValue(config.upload_target) << "\n\n"
            << "行内公式测试：$a+b=c$。\n\n"
            << "```cpp\n"
            << "int main() {\n"
            << "    return 0;\n"
            << "}\n"
            << "```\n";
    return content.str();
}

bool RunConfigTestUpload(const AppConfig &config, Logger *logger)
{
    const std::vector<std::string> targets = ParseUploadTargets(config.upload_target);
    const std::filesystem::path recent_path = RecentUploadResultsPath(config);
    std::filesystem::create_directories(recent_path.parent_path());

    if (targets.empty())
    {
        UploadJob job = MakeUploadJob(BuildConfigTestUploadContent(config), "configuration");
        job.id = "test-" + job.id;
        WriteRecentUploadResultReport(recent_path, job, false, "upload_target 不能为空");
        return false;
    }

    bool all_ok = true;
    const std::string content = BuildConfigTestUploadContent(config);
    for (const std::string &target_name : targets)
    {
        UploadJob job = MakeUploadJob(content, target_name);
        job.id = "test-" + job.id;
        try
        {
            AppConfig target_config = config;
            target_config.upload_target = target_name;
            ValidateConfigOrThrow(target_config);
            std::unique_ptr<UploadTarget> upload_target = CreateUploadTarget(target_config, logger);
            upload_target->Validate();
            upload_target->ProcessJob(&job, [] {});
            WriteRecentUploadResultReport(recent_path, job, true, "");
            WriteLastObsidianUploadState(LastObsidianUploadPath(config), job);
            if (logger != nullptr)
            {
                logger->Info("测试上传成功: [" + target_name + "] " + job.id +
                             (job.remote_url.empty() ? "" : " -> " + job.remote_url));
            }
        }
        catch (const UploadFailure &ex)
        {
            all_ok = false;
            job.last_error = ex.what();
            WriteRecentUploadResultReport(recent_path, job, false, ex.what());
            if (logger != nullptr)
            {
                logger->Warn("测试上传失败: [" + target_name + "] " + std::string(ex.what()));
            }
        }
        catch (const std::exception &ex)
        {
            all_ok = false;
            job.last_error = ex.what();
            WriteRecentUploadResultReport(recent_path, job, false, ex.what());
            if (logger != nullptr)
            {
                logger->Warn("测试上传失败: [" + target_name + "] " + std::string(ex.what()));
            }
        }
    }

    return all_ok;
}

class UploadWorker
{
public:
    using ResultCallback = std::function<void(const UploadJob &, bool, const std::string &)>;

    UploadWorker(PersistentQueue *queue, UploadTarget *target, Logger *logger)
        : queue_(queue), target_(target), logger_(logger)
    {
    }

    void Start()
    {
        stop_.store(false);
        thread_ = std::thread([this]
                              { Run(); });
    }

    void Stop()
    {
        stop_.store(true);
        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            wake_requested_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable())
        {
            thread_.join();
        }
    }

    void Notify()
    {
        {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            wake_requested_ = true;
        }
        cv_.notify_all();
    }

    void SetResultCallback(ResultCallback callback)
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        result_callback_ = std::move(callback);
    }

private:
    void EmitResult(const UploadJob &job, bool success, const std::string &detail)
    {
        ResultCallback callback;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback = result_callback_;
        }
        if (callback)
        {
            callback(job, success, detail);
        }
    }

    void Run()
    {
        if (logger_ != nullptr)
        {
            logger_->Info("上传线程已启动");
        }

        while (!stop_.load())
        {
            std::uint64_t next_due = 0;
            auto item = queue_->NextDueJob(NowUnixMs(), &next_due, logger_);
            if (!item.has_value())
            {
                WaitForNextJob(next_due);
                continue;
            }

            UploadJob job = item->first;
            const fs::path path = item->second;
            try
            {
                target_->ProcessJob(&job, [&]
                                    { queue_->Update(path, job); });
                queue_->MarkSuccess(path);
                if (logger_ != nullptr)
                {
                    logger_->Info("上传成功: " + job.id + (job.remote_url.empty() ? "" : " -> " + job.remote_url));
                }
                EmitResult(job, true, "");
            }
            catch (const UploadFailure &ex)
            {
                queue_->MarkFailure(path, job, ex.what(), ex.retryable(), ex.retry_after_seconds(), logger_);
                EmitResult(job, false, ex.what());
            }
            catch (const std::exception &ex)
            {
                queue_->MarkFailure(path, job, ex.what(), true, 0, logger_);
                EmitResult(job, false, ex.what());
            }
        }

        if (logger_ != nullptr)
        {
            logger_->Info("上传线程已停止");
        }
    }

    void WaitForNextJob(std::uint64_t next_due)
    {
        std::unique_lock<std::mutex> lock(wait_mutex_);
        if (next_due == 0)
        {
            cv_.wait(lock, [this]
                     { return stop_.load() || wake_requested_; });
            wake_requested_ = false;
            return;
        }

        const std::uint64_t now = NowUnixMs();
        const std::uint64_t delay = next_due > now ? next_due - now : 0;
        cv_.wait_for(lock, std::chrono::milliseconds(delay), [this]
                     { return stop_.load() || wake_requested_; });
        wake_requested_ = false;
    }

    PersistentQueue *queue_ = nullptr;
    UploadTarget *target_ = nullptr;
    Logger *logger_ = nullptr;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::condition_variable cv_;
    std::mutex wait_mutex_;
    bool wake_requested_ = false;
    std::mutex callback_mutex_;
    ResultCallback result_callback_;
};

class DisabledUploadTarget : public UploadTarget
{
public:
    explicit DisabledUploadTarget(std::string reason) : reason_(std::move(reason)) {}

    std::string Name() const override
    {
        return "disabled";
    }

    void Validate() override
    {
        throw UploadFailure(reason_, false, 0);
    }

    void ProcessJob(UploadJob *, const std::function<void()> &) override
    {
        throw UploadFailure(reason_, false, 0);
    }

private:
    std::string reason_;
};

class ClipboardReader
{
public:
    std::optional<std::string> ReadText(Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        const UINT html_format = RegisterClipboardFormatW(L"HTML Format");
        if (!IsClipboardFormatAvailable(CF_UNICODETEXT) &&
            (html_format == 0 || !IsClipboardFormatAvailable(html_format)))
        {
            return std::nullopt;
        }

        bool opened = false;
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            if (OpenClipboard(nullptr))
            {
                opened = true;
                break;
            }
            Sleep(static_cast<DWORD>(20 * (attempt + 1)));
        }

        if (!opened)
        {
            if (logger != nullptr)
            {
                logger->Warn("打开剪贴板失败: " + LastErrorMessage());
            }
            return std::nullopt;
        }

        struct ClipboardGuard
        {
            ~ClipboardGuard()
            {
                CloseClipboard();
            }
        } guard;

        const auto unicode_text = ReadUnicodeText(logger, max_clipboard_bytes);
        if (html_format != 0 && IsClipboardFormatAvailable(html_format))
        {
            const auto html_text = ReadHtmlText(html_format, logger, max_clipboard_bytes);
            if (html_text.has_value())
            {
                if (!HasEmptyMarkdownCodeFenceArtifact(*html_text))
                {
                    return html_text;
                }
                if (logger != nullptr)
                {
                    logger->Warn("HTML 剪贴板转换结果包含空代码块围栏，已回退到纯文本");
                }
            }
        }

        return unicode_text;
    }

private:
    std::optional<std::string> ReadHtmlText(UINT html_format, Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        HANDLE data = GetClipboardData(html_format);
        if (data == nullptr)
        {
            return std::nullopt;
        }

        const SIZE_T raw_bytes = GlobalSize(data);
        const std::uint64_t html_limit = std::min<std::uint64_t>(4ull * 1024ull * 1024ull, max_clipboard_bytes * 8ull);
        if (raw_bytes == 0 || raw_bytes > static_cast<SIZE_T>(html_limit))
        {
            if (logger != nullptr && raw_bytes > 0)
            {
                logger->Warn("HTML 剪贴板片段过大，回退到纯文本，bytes=" + std::to_string(raw_bytes));
            }
            return std::nullopt;
        }

        const char *raw = static_cast<const char *>(GlobalLock(data));
        if (raw == nullptr)
        {
            return std::nullopt;
        }
        std::string html(raw, raw + raw_bytes);
        GlobalUnlock(data);

        const auto fragment = ExtractCfHtmlFragment(html);
        if (!fragment.has_value())
        {
            return std::nullopt;
        }

        std::string converted = HtmlFragmentToMarkdown(*fragment);
        converted = Trim(converted);
        if (converted.empty())
        {
            return std::nullopt;
        }
        if (converted.size() > max_clipboard_bytes)
        {
            if (logger != nullptr)
            {
                logger->Warn("HTML 剪贴板转换后文本过大，已跳过，utf8_bytes=" + std::to_string(converted.size()));
            }
            return std::nullopt;
        }
        if (logger != nullptr)
        {
            logger->Info("已读取 HTML 剪贴板片段并转换为 Markdown-like 文本，bytes=" +
                         std::to_string(converted.size()));
        }
        return converted;
    }

    std::optional<std::string> ReadUnicodeText(Logger *logger, std::uint64_t max_clipboard_bytes) const
    {
        HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data == nullptr)
        {
            return std::nullopt;
        }

        const SIZE_T raw_bytes = GlobalSize(data);
        if (raw_bytes > 0 && raw_bytes > static_cast<SIZE_T>(max_clipboard_bytes * 2))
        {
            if (logger != nullptr)
            {
                logger->Warn("剪贴板原始文本过大，已跳过，utf16_bytes=" + std::to_string(raw_bytes));
            }
            return std::nullopt;
        }

        const wchar_t *raw = static_cast<const wchar_t *>(GlobalLock(data));
        if (raw == nullptr)
        {
            return std::nullopt;
        }

        std::wstring wide(raw);
        GlobalUnlock(data);
        std::string utf8 = NormalizeLineEndings(WideToUtf8(wide));
        utf8 = Trim(utf8);
        if (utf8.empty())
        {
            return std::nullopt;
        }
        if (utf8.size() > max_clipboard_bytes)
        {
            if (logger != nullptr)
            {
                logger->Warn("剪贴板文本过大，已跳过，utf8_bytes=" + std::to_string(utf8.size()));
            }
            return std::nullopt;
        }
        return utf8;
    }
};

std::filesystem::path WriteObsidianPreviewFromText(const AppConfig &config, std::string text)
{
    const std::string input = Trim(NormalizeLineEndings(std::move(text)));
    if (input.empty())
    {
        throw std::runtime_error("当前剪贴板没有可预览的文本");
    }

    const fs::path preview_path = ObsidianPreviewPath(config);
    AtomicWriteFile(preview_path, BuildObsidianMarkdownPreview(input, config.obsidian_tags));
    return preview_path;
}

std::optional<std::filesystem::path> WriteObsidianPreviewFromClipboard(const AppConfig &config,
                                                                       const ClipboardReader &reader,
                                                                       Logger *logger)
{
    const auto text = reader.ReadText(logger, config.max_clipboard_bytes);
    if (!text.has_value())
    {
        return std::nullopt;
    }
    return WriteObsidianPreviewFromText(config, *text);
}

class TrayApplication
{
public:
    TrayApplication(const AppConfig *config, fs::path config_path, PersistentQueue *queue, UploadWorker *worker,
                    Logger *logger, std::string startup_config_error, bool open_config_page_on_start)
        : config_(config),
          config_path_(std::move(config_path)),
          queue_(queue),
          worker_(worker),
          logger_(logger),
          startup_config_error_(std::move(startup_config_error)),
          open_config_page_on_start_(open_config_page_on_start),
          hotkey_spec_(DefaultHotkeySpec()),
          hotkey_enabled_(config->enable_hotkey),
          notifications_enabled_(config->tray_notifications),
          auto_start_enabled_(config->start_with_windows),
          auto_start_configured_(config->start_with_windows_configured),
          clipboard_listener_enabled_(config->enable_clipboard_listener)
    {
        try
        {
            hotkey_spec_ = ParseHotkeyOrThrow(config->hotkey);
        }
        catch (const std::exception &ex)
        {
            hotkey_enabled_ = false;
            if (startup_config_error_.empty())
            {
                startup_config_error_ = ex.what();
            }
            else
            {
                startup_config_error_ += "\n";
                startup_config_error_ += ex.what();
            }
            if (logger_ != nullptr)
            {
                logger_->Warn("热键配置无效，已临时禁用热键并使用默认显示值: " + std::string(ex.what()));
            }
        }
    }

    int Run()
    {
        taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        app_icon_ = LoadApplicationIcon(instance, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
        tray_icon_ = LoadApplicationIcon(instance, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));

        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TrayApplication::WindowProc;
        wc.hInstance = instance;
        wc.hIcon = app_icon_ != nullptr ? app_icon_ : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = tray_icon_ != nullptr ? tray_icon_ : wc.hIcon;
        wc.lpszClassName = L"NotionClipboardWinTrayWindow";

        RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, kAppDisplayName, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, 0, 0, nullptr, nullptr, instance, this);
        if (hwnd_ == nullptr)
        {
            throw std::runtime_error("创建后台窗口失败: " + LastErrorMessage());
        }
        if (worker_ != nullptr)
        {
            worker_->SetResultCallback([this](const UploadJob &job, bool success, const std::string &detail)
                                       { PostUploadResultNotice(job, success, detail); });
        }
        SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(wc.hIcon));
        SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(wc.hIconSm));

        AddTrayIcon();
        SyncAutoStartSetting();
        if (hotkey_enabled_)
        {
            hotkey_enabled_ = RegisterUploadHotkey();
        }
        if (clipboard_listener_enabled_)
        {
            EnableClipboardListener(true);
        }

        if (startup_config_error_.empty() && config_->upload_initial_clipboard)
        {
            ProcessClipboard("启动读取", false);
        }

        if (logger_ != nullptr)
        {
            logger_->Info("托盘进程已启动，hotkey=" + hotkey_spec_.display +
                          "，clipboard_listener=" + (clipboard_listener_registered_ ? "on" : "off"));
        }
        ShowNotification(L"Notion Clipboard Win", L"后台进程已启动，按 " + Utf8ToWide(hotkey_spec_.display) + L" 上传剪贴板。");
        const bool opened_config_page_on_start = MaybeOpenConfigPageOnStart();
        MaybeShowStartupConfigError(opened_config_page_on_start);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Cleanup();
        if (logger_ != nullptr)
        {
            logger_->Info("托盘进程已停止");
        }
        return 0;
    }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        TrayApplication *self = nullptr;
        if (message == WM_NCCREATE)
        {
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lparam);
            self = static_cast<TrayApplication *>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<TrayApplication *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self == nullptr)
        {
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
        return self->HandleMessage(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wparam, LPARAM lparam)
    {
        TrayApplication *self = recording_instance_;
        if (code == HC_ACTION && self != nullptr && self->recording_hotkey_ &&
            (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN))
        {
            const auto *info = reinterpret_cast<KBDLLHOOKSTRUCT *>(lparam);
            const UINT vk = static_cast<UINT>(info->vkCode);
            if (vk == VK_ESCAPE)
            {
                self->CancelHotkeyRecording();
                return 1;
            }
            if (IsModifierVirtualKey(vk))
            {
                return CallNextHookEx(self->keyboard_hook_, code, wparam, lparam);
            }

            const auto spec = HotkeySpecFromRecordedKey(CurrentHotkeyModifiers(), vk);
            if (spec.has_value())
            {
                self->ApplyRecordedHotkey(*spec);
                return 1;
            }

            self->ShowNotification(L"Notion Clipboard Win", L"热键需要包含 Ctrl/Alt/Shift/Win 和支持的主按键。");
            return 1;
        }
        return CallNextHookEx(self == nullptr ? nullptr : self->keyboard_hook_, code, wparam, lparam);
    }

    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == taskbar_created_message_)
        {
            tray_icon_added_ = false;
            AddTrayIcon();
            return 0;
        }

        switch (message)
        {
        case WM_HOTKEY:
            if (wparam == kUploadHotkeyId)
            {
                ProcessClipboard("热键", true);
            }
            return 0;
        case WM_COMMAND:
            HandleCommand(LOWORD(wparam));
            return 0;
        case kTrayCallbackMessage:
            if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP)
            {
                ShowContextMenu();
            }
            else if (LOWORD(lparam) == WM_LBUTTONDBLCLK)
            {
                ProcessClipboard("托盘双击", true);
            }
            return 0;
        case kUploadResultMessage:
        {
            std::unique_ptr<UploadResultNotice> notice(reinterpret_cast<UploadResultNotice *>(lparam));
            if (notice)
            {
                ShowNotification(notice->title, notice->message);
            }
            return 0;
        }
        case kWakeUploadWorkerMessage:
            if (worker_ != nullptr)
            {
                worker_->Notify();
            }
            return 0;
        case WM_CLIPBOARDUPDATE:
            if (clipboard_listener_registered_)
            {
                SetTimer(hwnd, kClipboardDebounceTimer, static_cast<UINT>(config_->debounce_ms), nullptr);
            }
            return 0;
        case WM_TIMER:
            if (wparam == kClipboardDebounceTimer)
            {
                KillTimer(hwnd, kClipboardDebounceTimer);
                ProcessClipboard("剪贴板事件", false);
            }
            return 0;
        case WM_CLOSE:
            Cleanup();
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    }

    void AddTrayIcon()
    {
        if (hwnd_ == nullptr)
        {
            return;
        }

        nid_ = {};
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = kTrayIconId;
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid_.uCallbackMessage = kTrayCallbackMessage;
        nid_.hIcon =
            tray_icon_ != nullptr ? tray_icon_ : (app_icon_ != nullptr ? app_icon_ : LoadIconW(nullptr, IDI_APPLICATION));
        wcsncpy_s(nid_.szTip, kAppDisplayName, _TRUNCATE);

        const DWORD action = tray_icon_added_ ? NIM_MODIFY : NIM_ADD;
        if (!Shell_NotifyIconW(action, &nid_))
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("托盘图标更新失败: " + LastErrorMessage());
            }
            return;
        }

        if (!tray_icon_added_)
        {
            nid_.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &nid_);
        }
        tray_icon_added_ = true;
    }

    void ShowNotification(const std::wstring &title, const std::wstring &message)
    {
        if (!notifications_enabled_ || !tray_icon_added_)
        {
            return;
        }

        NOTIFYICONDATAW notify = nid_;
        notify.uFlags = NIF_INFO;
        notify.dwInfoFlags = NIIF_INFO;
        wcsncpy_s(notify.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(notify.szInfo, message.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &notify);
    }

    void PostUploadResultNotice(const UploadJob &job, bool success, const std::string &detail)
    {
        if (cleaned_up_.load() || hwnd_ == nullptr)
        {
            return;
        }

        try
        {
            WriteRecentUploadResultReport(RecentUploadResultsPath(*config_), job, success, detail);
            if (success && job.target == "obsidian")
            {
                WriteLastObsidianUploadState(LastObsidianUploadPath(*config_), job);
            }
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("写入最近上传结果失败: " + std::string(ex.what()));
            }
        }

        auto notice = std::make_unique<UploadResultNotice>();
        notice->title = L"Notion Clipboard Win";
        const std::wstring target = UploadTargetDisplayName(job.target);
        const std::wstring note_title = TruncateWide(Utf8ToWide(job.title), 64);
        if (success)
        {
            notice->message = target + L" 上传成功";
            if (!note_title.empty())
            {
                notice->message += L"：" + note_title;
            }
            const std::string location = UploadJobLocation(job);
            if (!location.empty())
            {
                notice->message += L"\n" + TruncateWide(Utf8ToWide(location), 120);
            }
        }
        else
        {
            notice->message = target + L" 上传失败";
            if (!note_title.empty())
            {
                notice->message += L"：" + note_title;
            }
            if (!detail.empty())
            {
                notice->message += L"\n" + TruncateWide(Utf8ToWide(detail), 120);
            }
        }

        if (!PostMessageW(hwnd_, kUploadResultMessage, 0, reinterpret_cast<LPARAM>(notice.get())))
        {
            return;
        }
        notice.release();
    }

    bool RegisterUploadHotkey()
    {
        if (hwnd_ == nullptr)
        {
            return false;
        }
        if (hotkey_registered_)
        {
            UnregisterHotKey(hwnd_, kUploadHotkeyId);
            hotkey_registered_ = false;
        }

        const UINT modifiers = hotkey_spec_.modifiers | MOD_NOREPEAT;
        if (!RegisterHotKey(hwnd_, kUploadHotkeyId, modifiers, hotkey_spec_.vk))
        {
            if (logger_ != nullptr)
            {
                logger_->Error("注册全局热键失败: " + hotkey_spec_.display + "，原因: " + LastErrorMessage());
            }
            ShowNotification(L"Notion Clipboard Win", L"注册全局热键失败，可能已被其他程序占用。");
            return false;
        }

        hotkey_registered_ = true;
        if (logger_ != nullptr)
        {
            logger_->Info("全局热键已注册: " + hotkey_spec_.display);
        }
        return true;
    }

    void EnableClipboardListener(bool enabled)
    {
        if (hwnd_ == nullptr)
        {
            return;
        }

        if (enabled && !clipboard_listener_registered_)
        {
            if (!AddClipboardFormatListener(hwnd_))
            {
                if (logger_ != nullptr)
                {
                    logger_->Error("注册剪贴板监听失败: " + LastErrorMessage());
                }
                ShowNotification(L"Notion Clipboard Win", L"注册剪贴板监听失败。");
                clipboard_listener_enabled_ = false;
                return;
            }
            clipboard_listener_registered_ = true;
            clipboard_listener_enabled_ = true;
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板监听已启动");
            }
        }
        else if (!enabled && clipboard_listener_registered_)
        {
            RemoveClipboardFormatListener(hwnd_);
            clipboard_listener_registered_ = false;
            clipboard_listener_enabled_ = false;
            KillTimer(hwnd_, kClipboardDebounceTimer);
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板监听已停止");
            }
        }
        else
        {
            clipboard_listener_enabled_ = enabled;
        }
    }

    void ShowContextMenu()
    {
        HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
        {
            return;
        }

        const std::wstring hotkey_label = L"热键: " + Utf8ToWide(hotkey_spec_.display);
        AppendMenuW(menu, MF_STRING, kMenuUploadNow, L"上传当前剪贴板");
        AppendMenuW(menu, MF_STRING, kMenuPreviewObsidian, L"预览 Obsidian Markdown");
        AppendMenuW(menu, MF_GRAYED, kMenuHotkeyStatus, hotkey_label.c_str());
        AppendMenuW(menu, MF_STRING | (hotkey_enabled_ ? MF_CHECKED : MF_UNCHECKED), kMenuToggleHotkey, L"启用热键");
        AppendMenuW(menu, MF_STRING | (recording_hotkey_ ? MF_GRAYED : MF_ENABLED), kMenuRecordHotkey, L"录制热键...");
        AppendMenuW(menu, MF_STRING | (notifications_enabled_ ? MF_CHECKED : MF_UNCHECKED), kMenuToggleNotifications,
                    L"显示通知");
        AppendMenuW(menu, MF_STRING | (auto_start_enabled_ ? MF_CHECKED : MF_UNCHECKED), kMenuToggleAutoStart,
                    L"开机自动启动");
        AppendMenuW(menu, MF_STRING | (clipboard_listener_enabled_ ? MF_CHECKED : MF_UNCHECKED),
                    kMenuToggleClipboardListener, L"自动监听剪贴板");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuOpenConfigPage, L"打开配置页面");
        AppendMenuW(menu, MF_STRING, kMenuOpenRecentUploads, L"打开上传中心");
        AppendMenuW(menu, MF_STRING | (fs::exists(LastObsidianUploadPath(*config_)) ? MF_ENABLED : MF_GRAYED),
                    kMenuOpenLastObsidian, L"打开最近 Obsidian 笔记");
        AppendMenuW(menu, MF_STRING, kMenuOpenLog, L"查看日志");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuExit, L"退出");

        POINT cursor;
        GetCursorPos(&cursor);
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void HandleCommand(UINT command)
    {
        switch (command)
        {
        case kMenuUploadNow:
            ProcessClipboard("托盘菜单", true);
            break;
        case kMenuPreviewObsidian:
            PreviewClipboardForObsidian();
            break;
        case kMenuToggleHotkey:
            ToggleHotkey();
            break;
        case kMenuRecordHotkey:
            StartHotkeyRecording();
            break;
        case kMenuToggleNotifications:
            ToggleNotifications();
            break;
        case kMenuToggleAutoStart:
            ToggleAutoStart();
            break;
        case kMenuToggleClipboardListener:
            EnableClipboardListener(!clipboard_listener_enabled_);
            break;
        case kMenuOpenConfig:
            OpenConfig();
            break;
        case kMenuOpenConfigPage:
            OpenConfigPage();
            break;
        case kMenuValidateConfig:
            StartConfigDiagnostics();
            break;
        case kMenuOpenConfigDiagnostics:
            OpenConfigDiagnostics();
            break;
        case kMenuOpenRecentUploads:
            OpenUploadCenter();
            break;
        case kMenuOpenLastObsidian:
            OpenLastObsidianUpload();
            break;
        case kMenuOpenLog:
            OpenPath(config_->state_dir / L"notion-clipboard-win.log");
            break;
        case kMenuOpenStateDir:
            OpenPath(config_->state_dir);
            break;
        case kMenuExit:
            Cleanup();
            DestroyWindow(hwnd_);
            break;
        default:
            break;
        }
    }

    void ToggleHotkey()
    {
        if (hotkey_enabled_)
        {
            if (hotkey_registered_)
            {
                UnregisterHotKey(hwnd_, kUploadHotkeyId);
                hotkey_registered_ = false;
            }
            hotkey_enabled_ = false;
            if (logger_ != nullptr)
            {
                logger_->Info("全局热键已暂停");
            }
            return;
        }

        hotkey_enabled_ = RegisterUploadHotkey();
    }

    bool PersistConfigValue(const std::string &key, const std::string &value)
    {
        try
        {
            UpsertConfigValue(config_path_, key, value);
            return true;
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("写入配置失败: " + key + "=" + value + "，原因: " + std::string(ex.what()));
            }
            return false;
        }
    }

    void SyncAutoStartSetting()
    {
        if (!auto_start_configured_)
        {
            auto_start_enabled_ = IsAutoStartEnabled(config_path_);
            return;
        }

        std::string error;
        if (!SetAutoStartEnabled(auto_start_enabled_, config_path_, &error))
        {
            auto_start_enabled_ = IsAutoStartEnabled(config_path_);
            if (logger_ != nullptr)
            {
                logger_->Warn(error);
            }
            ShowNotification(L"Notion Clipboard Win", L"同步开机自动启动设置失败，请查看日志。");
            return;
        }

        auto_start_enabled_ = IsAutoStartEnabled(config_path_);
        if (logger_ != nullptr)
        {
            logger_->Info(std::string("开机自动启动已同步: ") + (auto_start_enabled_ ? "on" : "off"));
        }
    }

    void StopHotkeyRecordingHook()
    {
        if (keyboard_hook_ != nullptr)
        {
            UnhookWindowsHookEx(keyboard_hook_);
            keyboard_hook_ = nullptr;
        }
        if (recording_instance_ == this)
        {
            recording_instance_ = nullptr;
        }
        recording_hotkey_ = false;
    }

    void StartHotkeyRecording()
    {
        if (recording_hotkey_)
        {
            return;
        }

        const int result = MessageBoxW(hwnd_,
                                       L"点击“确定”后按下新的全局热键。\n\n"
                                       L"要求：Ctrl/Alt/Shift/Win 至少一个修饰键 + 一个主按键。\n"
                                       L"按 Esc 取消录制。",
                                       L"录制热键", MB_OKCANCEL | MB_ICONINFORMATION | MB_TOPMOST);
        if (result != IDOK)
        {
            return;
        }

        restore_hotkey_enabled_after_recording_ = hotkey_enabled_;
        if (hotkey_registered_)
        {
            UnregisterHotKey(hwnd_, kUploadHotkeyId);
            hotkey_registered_ = false;
        }

        recording_hotkey_ = true;
        recording_instance_ = this;
        keyboard_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &TrayApplication::KeyboardHookProc,
                                           GetModuleHandleW(nullptr), 0);
        if (keyboard_hook_ == nullptr)
        {
            const std::string error = LastErrorMessage();
            StopHotkeyRecordingHook();
            if (restore_hotkey_enabled_after_recording_)
            {
                hotkey_enabled_ = RegisterUploadHotkey();
            }
            if (logger_ != nullptr)
            {
                logger_->Error("启动热键录制失败: " + error);
            }
            MessageBoxW(hwnd_, L"启动热键录制失败，请查看日志。", L"Notion Clipboard Win",
                        MB_OK | MB_ICONERROR | MB_TOPMOST);
            return;
        }

        if (logger_ != nullptr)
        {
            logger_->Info("开始录制全局热键");
        }
        ShowNotification(L"Notion Clipboard Win", L"正在录制热键，按 Esc 取消。");
    }

    void CancelHotkeyRecording()
    {
        StopHotkeyRecordingHook();
        if (restore_hotkey_enabled_after_recording_)
        {
            hotkey_enabled_ = RegisterUploadHotkey();
        }
        if (logger_ != nullptr)
        {
            logger_->Info("热键录制已取消");
        }
        ShowNotification(L"Notion Clipboard Win", L"热键录制已取消。");
    }

    void ApplyRecordedHotkey(const HotkeySpec &new_spec)
    {
        const HotkeySpec previous_spec = hotkey_spec_;
        const bool previous_enabled = restore_hotkey_enabled_after_recording_;
        StopHotkeyRecordingHook();

        hotkey_spec_ = new_spec;
        hotkey_enabled_ = true;
        if (!RegisterUploadHotkey())
        {
            hotkey_spec_ = previous_spec;
            hotkey_enabled_ = previous_enabled;
            if (previous_enabled)
            {
                hotkey_enabled_ = RegisterUploadHotkey();
            }
            MessageBoxW(hwnd_, L"新热键注册失败，可能已被其他程序占用；已恢复原热键。", L"Notion Clipboard Win",
                        MB_OK | MB_ICONWARNING | MB_TOPMOST);
            return;
        }

        PersistConfigValue("hotkey", hotkey_spec_.display);
        PersistConfigValue("enable_hotkey", "true");
        if (logger_ != nullptr)
        {
            logger_->Info("全局热键已更新: " + hotkey_spec_.display);
        }
        std::wstring message = L"热键已更新为 ";
        message += Utf8ToWide(hotkey_spec_.display);
        message += L"。";
        ShowNotification(L"Notion Clipboard Win", message);
    }

    void ToggleNotifications()
    {
        notifications_enabled_ = !notifications_enabled_;
        PersistConfigValue("tray_notifications", notifications_enabled_ ? "true" : "false");
        if (logger_ != nullptr)
        {
            logger_->Info(std::string("托盘通知已") + (notifications_enabled_ ? "启用" : "关闭"));
        }
        if (notifications_enabled_)
        {
            ShowNotification(L"Notion Clipboard Win", L"托盘通知已启用。");
        }
    }

    void ToggleAutoStart()
    {
        const bool next_enabled = !auto_start_enabled_;
        std::string error;
        if (!SetAutoStartEnabled(next_enabled, config_path_, &error))
        {
            if (logger_ != nullptr)
            {
                logger_->Error(error);
            }
            ShowNotification(L"Notion Clipboard Win", L"更新开机自动启动失败，请查看日志。");
            return;
        }

        auto_start_enabled_ = next_enabled;
        auto_start_configured_ = true;
        PersistConfigValue("start_with_windows", auto_start_enabled_ ? "true" : "false");
        if (logger_ != nullptr)
        {
            logger_->Info(std::string("开机自动启动已") + (auto_start_enabled_ ? "启用" : "关闭"));
        }
        ShowNotification(L"Notion Clipboard Win",
                         auto_start_enabled_ ? L"开机自动启动已启用。" : L"开机自动启动已关闭。");
    }

    void OpenConfig()
    {
        if (!fs::exists(config_path_))
        {
            try
            {
                const fs::path parent = config_path_.parent_path();
                if (!parent.empty())
                {
                    fs::create_directories(parent);
                }
                AtomicWriteFile(config_path_, "upload_target=notion\n"
                                             "obsidian_vault_dir=\n"
                                             "obsidian_folder=Clipboard\n"
                                             "notion_token=\n"
                                             "data_source_id=\n"
                                             "database_id=\n"
                                             "hotkey=Ctrl+Shift+B\n"
                                             "enable_hotkey=true\n"
                                             "enable_clipboard_listener=false\n"
                                             "tray_notifications=true\n"
                                             "start_with_windows=false\n");
            }
            catch (const std::exception &ex)
            {
                if (logger_ != nullptr)
                {
                    logger_->Warn("创建配置文件失败: " + std::string(ex.what()));
                }
            }
        }
        OpenPath(config_path_);
    }

    void OpenConfigPage()
    {
        try
        {
            OpenPath(WriteConfigPage(*config_, config_path_));
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("创建配置页面失败: " + std::string(ex.what()));
            }
            ShowNotification(L"Notion Clipboard Win", L"创建配置页面失败，请查看日志。");
        }
    }

    void OpenRecentUploadResults()
    {
        const fs::path path = RecentUploadResultsPath(*config_);
        if (!fs::exists(path))
        {
            try
            {
                AtomicWriteFile(path, "# Recent Upload Results\n\n还没有上传结果。上传成功或失败后会在这里记录 Notion URL 和 Obsidian 文件路径。\n");
            }
            catch (const std::exception &ex)
            {
                if (logger_ != nullptr)
                {
                    logger_->Warn("创建最近上传结果文件失败: " + std::string(ex.what()));
                }
            }
        }
        OpenPath(path);
    }

    void OpenUploadCenter()
    {
        try
        {
            OpenPath(WriteUploadCenterPage(*config_, config_path_));
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("创建上传中心失败: " + std::string(ex.what()));
            }
            ShowNotification(L"Notion Clipboard Win", L"创建上传中心失败，请查看日志。");
        }
    }

    bool TryOpenShellTarget(const std::wstring &target)
    {
        HINSTANCE result = ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    }

    void OpenLastObsidianUpload()
    {
        const fs::path state_path = LastObsidianUploadPath(*config_);
        try
        {
            const std::optional<std::string> uri = ReadStateValue(state_path, "uri");
            const std::optional<std::string> file = ReadStateValue(state_path, "file");
            bool opened = false;
            if (uri.has_value() && uri->rfind("obsidian://", 0) == 0)
            {
                opened = TryOpenShellTarget(Utf8ToWide(*uri));
            }
            if (!opened && file.has_value() && !file->empty())
            {
                const fs::path file_path = fs::path(Utf8ToWide(*file));
                if (fs::exists(file_path))
                {
                    opened = TryOpenShellTarget(file_path.wstring());
                }
            }
            if (!opened)
            {
                ShowNotification(L"Notion Clipboard Win", L"无法打开最近 Obsidian 笔记，请查看最近上传结果。");
                OpenRecentUploadResults();
            }
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("打开最近 Obsidian 笔记失败: " + std::string(ex.what()));
            }
            ShowNotification(L"Notion Clipboard Win", L"无法打开最近 Obsidian 笔记，请查看最近上传结果。");
            OpenRecentUploadResults();
        }
    }

    void OpenConfigDiagnostics()
    {
        const fs::path path = ConfigDiagnosticsPath(*config_);
        if (!fs::exists(path))
        {
            try
            {
                AtomicWriteFile(path, "# Configuration Diagnostics\n\n还没有配置诊断结果。请先从托盘菜单选择“验证当前配置”。\n");
            }
            catch (const std::exception &ex)
            {
                if (logger_ != nullptr)
                {
                    logger_->Warn("创建配置诊断文件失败: " + std::string(ex.what()));
                }
            }
        }
        OpenPath(path);
    }

    void StartConfigDiagnostics()
    {
        if (diagnostics_running_.load())
        {
            ShowNotification(L"Notion Clipboard Win", L"配置验证正在进行。");
            return;
        }
        if (diagnostics_thread_.joinable())
        {
            diagnostics_thread_.join();
        }

        diagnostics_running_.store(true);
        const AppConfig config = *config_;
        const fs::path diagnostics_path = ConfigDiagnosticsPath(config);
        if (logger_ != nullptr)
        {
            logger_->Info("开始验证当前配置");
        }
        ShowNotification(L"Notion Clipboard Win", L"正在验证当前配置...");

        diagnostics_thread_ = std::thread([this, config, diagnostics_path]
                                          {
                                              bool ok = false;
                                              std::string error;
                                              try
                                              {
                                                  ok = WriteConfigDiagnosticsReport(diagnostics_path, config, logger_);
                                              }
                                              catch (const std::exception &ex)
                                              {
                                                  error = ex.what();
                                                  try
                                                  {
                                                      std::ostringstream report;
                                                      report << "# Configuration Diagnostics\n\n"
                                                             << "- Generated: " << IsoUtcTimestampFromUnixMs(NowUnixMs()) << "\n"
                                                             << "- Status: FAILED\n"
                                                             << "- Error: " << ReportLineValue(error) << "\n";
                                                      AtomicWriteFile(diagnostics_path, report.str());
                                                  }
                                                  catch (...)
                                                  {
                                                  }
                                              }
                                              diagnostics_running_.store(false);

                                              if (logger_ != nullptr)
                                              {
                                                  logger_->Info(std::string("配置验证完成: ") + (ok ? "OK" : "FAILED"));
                                              }

                                              auto notice = std::make_unique<UploadResultNotice>();
                                              notice->title = L"Notion Clipboard Win";
                                              notice->message = ok ? L"配置验证通过。报告已更新。" : L"配置验证发现问题。请查看配置诊断。";
                                              if (!error.empty())
                                              {
                                                  notice->message += L"\n" + TruncateWide(Utf8ToWide(error), 120);
                                              }
                                              if (!cleaned_up_.load() && hwnd_ != nullptr &&
                                                  PostMessageW(hwnd_, kUploadResultMessage, 0,
                                                               reinterpret_cast<LPARAM>(notice.get())))
                                              {
                                                  notice.release();
                                              }
                                          });
    }

    bool MaybeOpenConfigPageOnStart()
    {
        if (!open_config_page_on_start_)
        {
            return false;
        }
        if (logger_ != nullptr)
        {
            logger_->Info("按启动参数打开配置页面");
        }
        OpenConfigPage();
        return true;
    }

    void MaybeShowStartupConfigError(bool config_page_already_opened)
    {
        if (startup_config_error_.empty())
        {
            return;
        }
        if (logger_ != nullptr)
        {
            logger_->Warn("配置尚未可用: " + startup_config_error_);
        }
        ShowNotification(L"Notion Clipboard Win",
                         config_page_already_opened ? L"配置尚未完成，已打开配置页面。"
                                                    : L"配置尚未完成，请从托盘菜单打开配置页面。");
        MessageBoxW(hwnd_, (L"配置尚未完成，上传功能暂不可用。\n\n" + Utf8ToWide(startup_config_error_) +
                                (config_page_already_opened
                                     ? L"\n\n请在已打开的配置页面填写必要项，然后点击“应用并重启”。"
                                     : L"\n\n请从托盘菜单打开配置页面，填写必要项后点击“应用并重启”。"))
                               .c_str(),
                    L"Notion Clipboard Win", MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
    }

    void OpenPath(const fs::path &path)
    {
        const std::wstring target = path.wstring();
        HINSTANCE result = ShellExecuteW(hwnd_, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
        {
            if (logger_ != nullptr)
            {
                logger_->Warn("打开路径失败: " + WideToUtf8(target));
            }
            ShowNotification(L"Notion Clipboard Win", L"打开路径失败。");
        }
    }

    void ProcessClipboard(const char *trigger, bool user_initiated)
    {
        if (!startup_config_error_.empty())
        {
            if (logger_ != nullptr)
            {
                logger_->Warn(std::string(trigger) + "上传被跳过，配置尚未完成: " + startup_config_error_);
            }
            if (user_initiated)
            {
                ShowNotification(L"Notion Clipboard Win", L"配置尚未完成，请从托盘菜单打开配置页面。");
            }
            return;
        }

        const auto text = reader_.ReadText(logger_, config_->max_clipboard_bytes);
        if (!text.has_value())
        {
            if (user_initiated)
            {
                ShowNotification(L"Notion Clipboard Win", L"当前剪贴板没有可上传的文本。");
            }
            return;
        }

        const std::vector<std::string> targets = ParseUploadTargets(config_->upload_target);
        if (targets.empty())
        {
            if (logger_ != nullptr)
            {
                logger_->Warn(std::string(trigger) + "上传被跳过，upload_target 为空");
            }
            return;
        }

        UploadJob first_job = MakeUploadJob(*text, targets.front());
        const std::uint64_t now_ms = NowUnixMs();
        if (config_->duplicate_suppression_ms > 0 && first_job.hash == last_hash_ &&
            now_ms - last_hash_at_ms_ <= static_cast<std::uint64_t>(config_->duplicate_suppression_ms))
        {
            if (logger_ != nullptr)
            {
                logger_->Info("剪贴板内容重复，已忽略");
            }
            if (user_initiated)
            {
                ShowNotification(L"Notion Clipboard Win", L"短时间内相同内容已忽略。");
            }
            return;
        }

        std::vector<std::string> job_ids;
        job_ids.reserve(targets.size());
        job_ids.push_back(first_job.id);
        queue_->Enqueue(first_job);
        for (std::size_t i = 1; i < targets.size(); ++i)
        {
            UploadJob job = MakeUploadJob(*text, targets[i]);
            job_ids.push_back(job.id);
            queue_->Enqueue(job);
        }

        last_hash_ = first_job.hash;
        last_hash_at_ms_ = now_ms;
        worker_->Notify();
        if (logger_ != nullptr)
        {
            logger_->Info(std::string(trigger) + "已入队剪贴板内容: targets=" + config_->upload_target +
                          "，jobs=" + std::to_string(job_ids.size()) + "，first=" + job_ids.front() +
                          "，bytes=" + std::to_string(text->size()));
        }
        if (user_initiated)
        {
            ShowNotification(L"Notion Clipboard Win", L"剪贴板内容已加入上传队列：" + JoinTargetDisplayNames(targets) + L"。");
        }
    }

    void PreviewClipboardForObsidian()
    {
        try
        {
            const std::optional<fs::path> preview_path =
                WriteObsidianPreviewFromClipboard(*config_, reader_, logger_);
            if (!preview_path.has_value())
            {
                ShowNotification(L"Notion Clipboard Win", L"当前剪贴板没有可预览的文本。");
                return;
            }

            if (logger_ != nullptr)
            {
                logger_->Info("已生成 Obsidian Markdown 预览: " + WideToUtf8(preview_path->wstring()));
            }
            OpenPath(*preview_path);
        }
        catch (const std::exception &ex)
        {
            if (logger_ != nullptr)
            {
                logger_->Error("生成 Obsidian Markdown 预览失败: " + std::string(ex.what()));
            }
            ShowNotification(L"Notion Clipboard Win", L"生成 Obsidian 预览失败，请查看日志。");
        }
    }

    void Cleanup()
    {
        bool expected = false;
        if (!cleaned_up_.compare_exchange_strong(expected, true))
        {
            return;
        }
        StopHotkeyRecordingHook();
        if (worker_ != nullptr)
        {
            worker_->SetResultCallback({});
        }
        if (diagnostics_thread_.joinable())
        {
            diagnostics_thread_.join();
        }

        if (hwnd_ != nullptr)
        {
            if (clipboard_listener_registered_)
            {
                RemoveClipboardFormatListener(hwnd_);
                clipboard_listener_registered_ = false;
            }
            if (hotkey_registered_)
            {
                UnregisterHotKey(hwnd_, kUploadHotkeyId);
                hotkey_registered_ = false;
            }
        }

        if (tray_icon_added_)
        {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            tray_icon_added_ = false;
        }
        if (tray_icon_ != nullptr)
        {
            DestroyIcon(tray_icon_);
            tray_icon_ = nullptr;
        }
        if (app_icon_ != nullptr)
        {
            DestroyIcon(app_icon_);
            app_icon_ = nullptr;
        }
    }

    const AppConfig *config_ = nullptr;
    fs::path config_path_;
    PersistentQueue *queue_ = nullptr;
    UploadWorker *worker_ = nullptr;
    Logger *logger_ = nullptr;
    std::string startup_config_error_;
    bool open_config_page_on_start_ = false;
    ClipboardReader reader_;
    HotkeySpec hotkey_spec_;
    std::string last_hash_;
    std::uint64_t last_hash_at_ms_ = 0;
    HWND hwnd_ = nullptr;
    HICON app_icon_ = nullptr;
    HICON tray_icon_ = nullptr;
    NOTIFYICONDATAW nid_ = {};
    UINT taskbar_created_message_ = 0;
    bool hotkey_enabled_ = true;
    bool hotkey_registered_ = false;
    bool notifications_enabled_ = true;
    bool auto_start_enabled_ = false;
    bool auto_start_configured_ = false;
    bool clipboard_listener_enabled_ = false;
    bool clipboard_listener_registered_ = false;
    bool tray_icon_added_ = false;
    std::atomic<bool> cleaned_up_{false};
    std::atomic<bool> diagnostics_running_{false};
    std::thread diagnostics_thread_;
    bool recording_hotkey_ = false;
    bool restore_hotkey_enabled_after_recording_ = false;
    HHOOK keyboard_hook_ = nullptr;
    static TrayApplication *recording_instance_;
};

TrayApplication *TrayApplication::recording_instance_ = nullptr;

DWORD g_main_thread_id = 0;

BOOL WINAPI ConsoleCtrlHandler(DWORD control_type)
{
    switch (control_type)
    {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_main_thread_id != 0)
        {
            PostThreadMessageW(g_main_thread_id, WM_QUIT, 0, 0);
        }
        return TRUE;
    default:
        return FALSE;
    }
}

int RunOnce(const AppConfig &config, PersistentQueue *queue, UploadTarget *target, Logger *logger, bool dry_run)
{
    ClipboardReader reader;
    const auto text = reader.ReadText(logger, config.max_clipboard_bytes);
    if (!text.has_value())
    {
        throw std::runtime_error("当前剪贴板没有可上传的文本");
    }

    UploadJob job = MakeUploadJob(*text, config.upload_target);
    if (dry_run)
    {
        const std::vector<std::string> blocks = BuildTextBlocks(job.content);
        const auto equation_count = std::count_if(blocks.begin(), blocks.end(), [](const std::string &block)
                                                  { return block.find("\"type\":\"equation\"") != std::string::npos; });
        const auto code_count = std::count_if(blocks.begin(), blocks.end(), [](const std::string &block)
                                              { return block.find("\"type\":\"code\"") != std::string::npos; });
        logger->Info("dry-run: title=" + job.title + "，bytes=" + std::to_string(text->size()) +
                     "，blocks=" + std::to_string(blocks.size()) +
                     "，equations=" + std::to_string(static_cast<std::size_t>(equation_count)) +
                     "，code_blocks=" + std::to_string(static_cast<std::size_t>(code_count)));
        return 0;
    }

    const std::vector<std::string> targets = ParseUploadTargets(config.upload_target);
    if (targets.empty())
    {
        throw std::runtime_error("upload_target 不能为空");
    }

    int result = 0;
    for (const std::string &target_name : targets)
    {
        UploadJob target_job = MakeUploadJob(*text, target_name);
        try
        {
            target->ProcessJob(&target_job, [] {});
            logger->Info("上传成功: " + target_job.id + " [" + target_name + "]" +
                         (target_job.remote_url.empty() ? "" : " -> " + target_job.remote_url));
        }
        catch (const UploadFailure &ex)
        {
            target_job.last_error = ex.what();
            queue->Enqueue(target_job);
            logger->Error("单次上传失败，任务已保存到队列: [" + target_name + "] " + std::string(ex.what()));
            result = std::max(result, ex.retryable() ? 2 : 3);
        }
    }
    return result;
}

int HexDigit(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f')
    {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(const std::string &input)
{
    std::string output;
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i)
    {
        if (input[i] == '%' && i + 2 < input.size())
        {
            const int high = HexDigit(input[i + 1]);
            const int low = HexDigit(input[i + 2]);
            if (high >= 0 && low >= 0)
            {
                output.push_back(static_cast<char>((high << 4) | low));
                i += 2;
                continue;
            }
        }
        output.push_back(input[i] == '+' ? ' ' : input[i]);
    }
    return output;
}

std::optional<std::string> QueryValue(const std::string &url, const std::string &key)
{
    const std::size_t question = url.find('?');
    if (question == std::string::npos)
    {
        return std::nullopt;
    }

    std::size_t begin = question + 1;
    while (begin <= url.size())
    {
        const std::size_t end = url.find('&', begin);
        const std::string part = url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        const std::size_t eq = part.find('=');
        const std::string name = UrlDecode(part.substr(0, eq));
        if (name == key)
        {
            return UrlDecode(eq == std::string::npos ? "" : part.substr(eq + 1));
        }
        if (end == std::string::npos)
        {
            break;
        }
        begin = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> IniValue(const std::string &content, const std::string &key)
{
    const std::string normalized_key = ToLowerAscii(Trim(key));
    std::istringstream input(content);
    std::string line;
    while (std::getline(input, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';')
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
            return Trim(line.substr(eq + 1));
        }
    }
    return std::nullopt;
}

void ValidateConfigContentForApply(const std::filesystem::path &config_path, const std::string &content)
{
    const std::optional<std::string> upload_target = IniValue(content, "upload_target");
    if (!upload_target.has_value() || ParseUploadTargets(*upload_target).empty())
    {
        throw std::runtime_error("配置内容缺少有效的 upload_target");
    }

    std::filesystem::path temp_path = config_path;
    temp_path += L".validate.";
    temp_path += std::to_wstring(GetCurrentProcessId());
    temp_path += L".ini";

    std::error_code ignored;
    try
    {
        AtomicWriteFile(temp_path, content);
        const AppConfig candidate = LoadConfig(temp_path);
        ValidateConfigOrThrow(candidate);
    }
    catch (...)
    {
        std::filesystem::remove(temp_path, ignored);
        throw;
    }
    std::filesystem::remove(temp_path, ignored);
}

std::wstring QuoteCommandArg(const std::wstring &arg)
{
    std::wstring output = L"\"";
    for (wchar_t ch : arg)
    {
        if (ch == L'"')
        {
            output += L"\\\"";
        }
        else
        {
            output.push_back(ch);
        }
    }
    output += L"\"";
    return output;
}

void RegisterConfigProtocolHandler(Logger *logger)
{
    const std::filesystem::path exe_path = ModuleDirectory() / L"notion_clipboard_win.exe";
    const std::wstring command = QuoteCommandArg(exe_path.wstring()) + L" \"%1\"";
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\notion-clipboard-win", 0, nullptr, 0,
                                  KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS)
    {
        if (logger != nullptr)
        {
            logger->Warn("注册配置应用协议失败: " + LastErrorMessage(static_cast<DWORD>(result)));
        }
        return;
    }

    const wchar_t *display = L"URL:Notion Clipboard Win";
    RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(display),
                   static_cast<DWORD>((wcslen(display) + 1) * sizeof(wchar_t)));
    const wchar_t *empty = L"";
    RegSetValueExW(key, L"URL Protocol", 0, REG_SZ, reinterpret_cast<const BYTE *>(empty), sizeof(wchar_t));
    RegCloseKey(key);

    result = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\notion-clipboard-win\\shell\\open\\command", 0,
                             nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (result != ERROR_SUCCESS)
    {
        if (logger != nullptr)
        {
            logger->Warn("注册配置应用协议命令失败: " + LastErrorMessage(static_cast<DWORD>(result)));
        }
        return;
    }
    RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                   static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

std::filesystem::path ConfigPathFromProtocolUrl(const std::string &url, const char *action_name)
{
    const std::optional<std::string> path_value = QueryValue(url, "path");
    if (!path_value.has_value())
    {
        throw std::runtime_error(std::string(action_name) + " URL 缺少 path");
    }

    const std::filesystem::path config_path = std::filesystem::absolute(std::filesystem::path(Utf8ToWide(*path_value)));
    if (config_path.filename() != L"notion_clipboard_win.ini")
    {
        throw std::runtime_error("只允许访问 notion_clipboard_win.ini: " + WideToUtf8(config_path.wstring()));
    }
    return config_path;
}

void OpenPathWithShell(const std::filesystem::path &path, const char *action_name)
{
    HINSTANCE opened = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(opened) <= 32)
    {
        throw std::runtime_error(std::string(action_name) + "失败: " + LastErrorMessage());
    }
}

void WakeRunningUploadWorker()
{
    HWND existing = FindWindowW(L"NotionClipboardWinTrayWindow", kAppDisplayName);
    if (existing != nullptr)
    {
        PostMessageW(existing, kWakeUploadWorkerMessage, 0, 0);
    }
}

void WriteFileIfMissing(const std::filesystem::path &path, const std::string &content)
{
    std::filesystem::create_directories(path.parent_path());
    if (!std::filesystem::exists(path))
    {
        AtomicWriteFile(path, content);
    }
}

int ApplyConfigUrlAndRestart(const std::string &url)
{
    std::optional<std::string> content_value = QueryValue(url, "content");
    if (!content_value.has_value())
    {
        content_value = QueryValue(url, "icontent");
    }
    if (!content_value.has_value())
    {
        throw std::runtime_error("配置应用 URL 缺少 content");
    }

    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "配置应用");

    try
    {
        ValidateConfigContentForApply(config_path, *content_value);
    }
    catch (const std::exception &ex)
    {
        throw std::runtime_error(std::string("配置内容未通过校验，未覆盖原配置: ") + ex.what());
    }

    AtomicWriteFile(config_path, *content_value);

    HWND existing = FindWindowW(L"NotionClipboardWinTrayWindow", kAppDisplayName);
    if (existing != nullptr)
    {
        SendMessageW(existing, WM_CLOSE, 0, 0);
        Sleep(500);
    }

    const std::filesystem::path exe_path = ModuleDirectory() / L"notion_clipboard_win.exe";
    const std::wstring args = L"--config " + QuoteCommandArg(config_path.wstring());
    HINSTANCE launched = ShellExecuteW(nullptr, L"open", exe_path.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(launched) <= 32)
    {
        throw std::runtime_error("重启程序失败: " + LastErrorMessage());
    }
    return 0;
}

int OpenConfigPageUrl(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "配置页面");
    const AppConfig config = LoadConfig(config_path);
    const std::filesystem::path page_path = WriteConfigPage(config, config_path);
    OpenPathWithShell(page_path, "打开配置页面");
    return 0;
}

AppConfig LoadConfigFromProtocolUrlOrContent(const std::string &url, const std::filesystem::path &config_path,
                                             std::filesystem::path *temp_path)
{
    std::optional<std::string> content_value = QueryValue(url, "content");
    if (!content_value.has_value())
    {
        content_value = QueryValue(url, "icontent");
    }
    if (!content_value.has_value())
    {
        return LoadConfig(config_path);
    }

    std::filesystem::path candidate_path = config_path;
    candidate_path += L".validate-url.";
    candidate_path += std::to_wstring(GetCurrentProcessId());
    candidate_path += L".ini";
    AtomicWriteFile(candidate_path, *content_value);
    if (temp_path != nullptr)
    {
        *temp_path = candidate_path;
    }
    return LoadConfig(candidate_path);
}

int ValidateConfigUrlAndOpenReport(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "配置验证");
    std::filesystem::path temp_path;
    std::error_code ignored;

    try
    {
        const AppConfig config = LoadConfigFromProtocolUrlOrContent(url, config_path, &temp_path);
        std::filesystem::create_directories(config.state_dir);
        Logger logger(config.state_dir / L"notion-clipboard-win.log", false);
        const std::filesystem::path diagnostics_path = ConfigDiagnosticsPath(config);
        WriteConfigDiagnosticsReport(diagnostics_path, config, &logger);
        std::filesystem::remove(temp_path, ignored);
        OpenPathWithShell(diagnostics_path, "打开配置诊断");
    }
    catch (const std::exception &ex)
    {
        AppConfig fallback_config = LoadConfig(config_path);
        std::filesystem::create_directories(fallback_config.state_dir);
        const std::filesystem::path diagnostics_path = ConfigDiagnosticsPath(fallback_config);
        std::ostringstream report;
        report << "# Configuration Diagnostics\n\n"
               << "- Generated: " << IsoUtcTimestampFromUnixMs(NowUnixMs()) << "\n"
               << "- Status: FAILED\n"
               << "- Error: " << ReportLineValue(ex.what()) << "\n";
        AtomicWriteFile(diagnostics_path, report.str());
        std::filesystem::remove(temp_path, ignored);
        OpenPathWithShell(diagnostics_path, "打开配置诊断");
    }
    return 0;
}

int TestUploadUrlAndOpenReport(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "测试上传");
    std::filesystem::path temp_path;
    std::error_code ignored;

    try
    {
        const AppConfig config = LoadConfigFromProtocolUrlOrContent(url, config_path, &temp_path);
        std::filesystem::create_directories(config.state_dir);
        Logger logger(config.state_dir / L"notion-clipboard-win.log", false);
        RunConfigTestUpload(config, &logger);
        std::filesystem::remove(temp_path, ignored);
        OpenPathWithShell(WriteUploadCenterPage(config, config_path), "打开上传中心");
    }
    catch (const std::exception &ex)
    {
        AppConfig fallback_config;
        try
        {
            fallback_config = LoadConfig(config_path);
        }
        catch (...)
        {
        }
        std::filesystem::create_directories(fallback_config.state_dir);
        const std::filesystem::path recent_path = RecentUploadResultsPath(fallback_config);
        UploadJob job = MakeUploadJob(BuildConfigTestUploadContent(fallback_config), "configuration");
        job.id = "test-" + job.id;
        WriteRecentUploadResultReport(recent_path, job, false, ex.what());
        std::filesystem::remove(temp_path, ignored);
        OpenPathWithShell(WriteUploadCenterPage(fallback_config, config_path), "打开上传中心");
    }
    return 0;
}

int PreviewObsidianClipboardUrlAndOpen(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "Obsidian 预览");
    std::filesystem::path temp_path;
    std::error_code ignored;

    try
    {
        const AppConfig config = LoadConfigFromProtocolUrlOrContent(url, config_path, &temp_path);
        std::filesystem::create_directories(config.state_dir);
        Logger logger(config.state_dir / L"notion-clipboard-win.log", false);
        ClipboardReader reader;
        const std::optional<std::filesystem::path> preview_path =
            WriteObsidianPreviewFromClipboard(config, reader, &logger);
        std::filesystem::remove(temp_path, ignored);
        if (!preview_path.has_value())
        {
            throw std::runtime_error("当前剪贴板没有可预览的文本");
        }
        OpenPathWithShell(*preview_path, "打开 Obsidian Markdown 预览");
    }
    catch (const std::exception &ex)
    {
        std::filesystem::remove(temp_path, ignored);
        const AppConfig fallback_config = LoadConfig(config_path);
        std::filesystem::create_directories(fallback_config.state_dir);
        Logger logger(fallback_config.state_dir / L"notion-clipboard-win.log", false);
        logger.Error("生成 Obsidian Markdown 预览失败: " + std::string(ex.what()));
        throw;
    }
    return 0;
}

int OpenConfigDiagnosticsUrl(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "配置诊断");
    const AppConfig config = LoadConfig(config_path);
    const std::filesystem::path diagnostics_path = ConfigDiagnosticsPath(config);
    WriteFileIfMissing(diagnostics_path,
                       "# Configuration Diagnostics\n\n还没有配置诊断结果。请先点击“验证当前配置”。\n");
    OpenPathWithShell(diagnostics_path, "打开配置诊断");
    return 0;
}

int OpenRecentUploadsUrl(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "最近上传结果");
    const AppConfig config = LoadConfig(config_path);
    const std::filesystem::path recent_path = RecentUploadResultsPath(config);
    WriteFileIfMissing(recent_path,
                       "# Recent Upload Results\n\n还没有上传结果。上传成功或失败后会在这里记录 Notion URL 和 Obsidian 文件路径。\n");
    OpenPathWithShell(recent_path, "打开最近上传结果");
    return 0;
}

int OpenUploadCenterUrl(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "上传中心");
    const AppConfig config = LoadConfig(config_path);
    const std::filesystem::path page_path = WriteUploadCenterPage(config, config_path);
    OpenPathWithShell(page_path, "打开上传中心");
    return 0;
}

int RetryFailedUploadsUrlAndOpenCenter(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "重试失败任务");
    const AppConfig config = LoadConfig(config_path);
    const std::size_t retried = RetryFailedUploads(config);
    if (retried > 0)
    {
        WakeRunningUploadWorker();
    }
    const std::filesystem::path page_path = WriteUploadCenterPage(config, config_path);
    OpenPathWithShell(page_path, "打开上传中心");
    return 0;
}

int RetryFailedJobUrlAndOpenCenter(const std::string &url)
{
    const std::filesystem::path config_path = ConfigPathFromProtocolUrl(url, "重试失败任务");
    const AppConfig config = LoadConfig(config_path);
    const std::optional<std::string> file = QueryValue(url, "file");
    if (!file.has_value())
    {
        throw std::runtime_error("重试失败任务 URL 缺少 file");
    }

    const std::size_t retried = RetryFailedUpload(config, *file);
    if (retried > 0)
    {
        WakeRunningUploadWorker();
    }
    const std::filesystem::path page_path = WriteUploadCenterPage(config, config_path);
    OpenPathWithShell(page_path, "打开上传中心");
    return 0;
}

int RunObsidianDryRunFile(const std::filesystem::path &input_path, const std::filesystem::path &output_path,
                          const std::string &obsidian_tags, bool quiet = false)
{
    const std::string input = Trim(NormalizeLineEndings(ReadWholeFile(input_path)));
    if (input.empty())
    {
        throw std::runtime_error("输入文件没有可转换的文本");
    }

    std::string output = BuildObsidianMarkdownPreview(input, obsidian_tags);
    if (output.empty())
    {
        throw std::runtime_error("Obsidian 预览内容为空");
    }
    if (output.back() != '\n')
    {
        output.push_back('\n');
    }

    AtomicWriteFile(output_path, output);
    if (!quiet)
    {
        std::cout << "obsidian dry-run: title=" << BuildTitleFromContent(input) << "，bytes=" << output.size()
                  << "，output=" << WideToUtf8(output_path.wstring()) << "\n";
    }
    return 0;
}

int RunMainSelfTest()
{
    bool ok = true;
    auto fail = [&](const std::string &message)
    {
        std::cout << "[FAIL] main self-test: " << message << "\n";
        ok = false;
    };

    class SuccessfulTarget : public UploadTarget
    {
    public:
        std::string Name() const override
        {
            return "obsidian";
        }

        void Validate() override {}

        void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) override
        {
            job->remote_id = "E:\\vault\\Inbox\\Worker Callback.md";
            job->remote_url = "obsidian://open?vault=Test&file=Inbox%2FWorker%20Callback.md";
            job->remote_progress = 1;
            checkpoint();
        }
    };

    const fs::path root =
        fs::temp_directory_path() / (L"notion-clipboard-win-main-test-" + std::to_wstring(NowUnixMs()));
    std::error_code ignored;
    fs::remove_all(root, ignored);

    try
    {
        {
            wchar_t exe[] = L"notion_clipboard_win.exe";
            wchar_t flag[] = L"--open-config-page-on-start";
            wchar_t *argv[] = {exe, flag};
            const CliOptions parsed = ParseCli(2, argv);
            if (!parsed.open_config_page_on_start)
            {
                fail("open-config-page-on-start flag was not parsed");
            }
        }
        {
            wchar_t exe[] = L"notion_clipboard_win.exe";
            wchar_t url[] = L"notion-clipboard-win:/test-upload/?path=C%3A%5CTemp%5Cnotion_clipboard_win.ini";
            wchar_t *argv[] = {exe, url};
            const CliOptions parsed = ParseCli(2, argv);
            if (parsed.test_upload_url.empty())
            {
                fail("test-upload protocol URL was not parsed");
            }
        }
        {
            wchar_t exe[] = L"notion_clipboard_win.exe";
            wchar_t url[] =
                L"notion-clipboard-win:/preview-obsidian-clipboard/?path=C%3A%5CTemp%5Cnotion_clipboard_win.ini";
            wchar_t *argv[] = {exe, url};
            const CliOptions parsed = ParseCli(2, argv);
            if (parsed.preview_obsidian_clipboard_url.empty())
            {
                fail("preview-obsidian-clipboard protocol URL was not parsed");
            }
        }
        {
            wchar_t exe[] = L"notion_clipboard_win.exe";
            wchar_t url[] = L"notion-clipboard-win:/open-upload-center/?path=C%3A%5CTemp%5Cnotion_clipboard_win.ini";
            wchar_t *argv[] = {exe, url};
            const CliOptions parsed = ParseCli(2, argv);
            if (parsed.open_upload_center_url.empty())
            {
                fail("open-upload-center protocol URL was not parsed");
            }
        }
        {
            wchar_t exe[] = L"notion_clipboard_win.exe";
            wchar_t url[] = L"notion-clipboard-win:/retry-failed-uploads/?path=C%3A%5CTemp%5Cnotion_clipboard_win.ini";
            wchar_t *argv[] = {exe, url};
            const CliOptions parsed = ParseCli(2, argv);
            if (parsed.retry_failed_uploads_url.empty())
            {
                fail("retry-failed-uploads protocol URL was not parsed");
            }
        }
        {
            wchar_t exe[] = L"notion_clipboard_win.exe";
            wchar_t url[] =
                L"notion-clipboard-win:/retry-failed-job/?path=C%3A%5CTemp%5Cnotion_clipboard_win.ini&file=failed.job";
            wchar_t *argv[] = {exe, url};
            const CliOptions parsed = ParseCli(2, argv);
            if (parsed.retry_failed_job_url.empty())
            {
                fail("retry-failed-job protocol URL was not parsed");
            }
        }
        {
            wchar_t exe[] = L"notion_clipboard_win.exe";
            wchar_t flag[] = L"--dry-run-obsidian-file";
            wchar_t input[] = L"C:\\Temp\\in.md";
            wchar_t output[] = L"C:\\Temp\\out.md";
            wchar_t *argv[] = {exe, flag, input, output};
            const CliOptions parsed = ParseCli(4, argv);
            if (parsed.dry_run_obsidian_input_path.empty() || parsed.dry_run_obsidian_output_path.empty())
            {
                fail("dry-run-obsidian-file paths were not parsed");
            }
        }
        {
            const fs::path input_path = root / L"obsidian-preview-input.md";
            const fs::path output_path = root / L"obsidian-preview-output.md";
            AtomicWriteFile(input_path,
                            "预览标题\n\n"
                            "> 如果 DP 转移是\n"
                            "> [\n"
                            "> dp[i]=\\min_j{上一层dp[j]+cost(j,i)}\n"
                            "> ]\n");
            RunObsidianDryRunFile(input_path, output_path, "#算法 cpp", true);
            const std::string preview = ReadWholeFile(output_path);
            if (preview.find("---\ntags:\n  - \"算法\"\n  - \"cpp\"\n---\n\n# 预览标题\n\n> 如果 DP 转移是") != 0 ||
                preview.find("> $$\n> dp[i]=\\min_j{上一层dp[j]+cost(j,i)}\n> $$") == std::string::npos ||
                preview.find("# 预览标题\n\n预览标题") != std::string::npos)
            {
                fail("dry-run-obsidian-file did not write full Obsidian preview");
            }
        }

        PersistentQueue queue(root, 2);
        SuccessfulTarget target;
        UploadWorker worker(&queue, &target, nullptr);

        std::mutex mutex;
        std::condition_variable cv;
        bool called = false;
        bool success = false;
        UploadJob callback_job;
        std::string detail;

        worker.SetResultCallback([&](const UploadJob &job, bool result, const std::string &message)
                                 {
                                     std::lock_guard<std::mutex> lock(mutex);
                                     called = true;
                                     success = result;
                                     callback_job = job;
                                     detail = message;
                                     cv.notify_all();
                                 });

        UploadJob job = MakeUploadJob("Worker Callback\n\nBody", "obsidian");
        queue.Enqueue(job);
        worker.Start();

        {
            std::unique_lock<std::mutex> lock(mutex);
            if (!cv.wait_for(lock, std::chrono::seconds(5), [&]
                             { return called; }))
            {
                fail("upload worker did not emit result callback");
            }
        }
        worker.Stop();

        if (ok && (!success || callback_job.target != "obsidian" ||
                   callback_job.remote_id != "E:\\vault\\Inbox\\Worker Callback.md" ||
                   callback_job.remote_url.find("obsidian://open") != 0 || callback_job.remote_progress != 1 ||
                   !detail.empty()))
        {
            fail("upload worker result callback fields mismatch");
        }

        const fs::path report_path = root / L"recent-upload-results.md";
        WriteRecentUploadResultReport(report_path, callback_job, true, "");
        UploadJob notion_job = callback_job;
        notion_job.id = "notion-job";
        notion_job.target = "notion";
        notion_job.remote_id = "notion-page-id";
        notion_job.remote_url = "https://www.notion.so/notion-page-id";
        WriteRecentUploadResultReport(report_path, notion_job, true, "");
        UploadJob failed_job = callback_job;
        failed_job.id = "failed-job";
        failed_job.target = "notion";
        failed_job.remote_id.clear();
        failed_job.remote_url.clear();
        WriteRecentUploadResultReport(report_path, failed_job, false, "missing data_source_id");
        const std::string report = ReadWholeFile(report_path);
        if (report.find("# Recent Upload Results") != 0 ||
            report.find("FAILED - notion") == std::string::npos ||
            report.find("- Error: missing data_source_id") == std::string::npos ||
            report.find("SUCCESS - notion") == std::string::npos ||
            report.find("- Notion URL: <https://www.notion.so/notion-page-id>") == std::string::npos ||
            report.find("- Notion Page ID: notion-page-id") == std::string::npos ||
            report.find("SUCCESS - obsidian") == std::string::npos ||
            report.find("- Obsidian File: E:\\vault\\Inbox\\Worker Callback.md") == std::string::npos ||
            report.find("- Local File URI: <file:///E:/vault/Inbox/Worker%20Callback.md>") == std::string::npos ||
            report.find("- Obsidian URI: <obsidian://open?vault=Test&file=Inbox%2FWorker%20Callback.md>") ==
                std::string::npos ||
            report.find("FAILED - notion") > report.find("SUCCESS - notion") ||
            report.find("SUCCESS - notion") > report.find("SUCCESS - obsidian"))
        {
            fail("recent upload result report content mismatch");
        }

        const fs::path last_obsidian_path = root / L"last-obsidian-upload.ini";
        WriteLastObsidianUploadState(last_obsidian_path, callback_job);
        WriteLastObsidianUploadState(last_obsidian_path, notion_job);
        const std::string last_obsidian = ReadWholeFile(last_obsidian_path);
        const std::optional<std::string> last_file = ReadStateValue(last_obsidian_path, "file");
        const std::optional<std::string> last_uri = ReadStateValue(last_obsidian_path, "uri");
        const std::optional<std::string> last_title = ReadStateValue(last_obsidian_path, "title");
        if (last_obsidian.find("updated=") == std::string::npos || !last_file.has_value() ||
            *last_file != "E:\\vault\\Inbox\\Worker Callback.md" || !last_uri.has_value() ||
            *last_uri != "obsidian://open?vault=Test&file=Inbox%2FWorker%20Callback.md" || !last_title.has_value() ||
            *last_title != callback_job.title || last_obsidian.find("notion-page-id") != std::string::npos)
        {
            fail("last obsidian upload state content mismatch");
        }

        const fs::path protocol_config_path = root / L"notion_clipboard_win.ini";
        const fs::path protocol_vault = root / L"protocol-vault";
        fs::create_directories(protocol_vault);
        std::filesystem::path protocol_temp_path;
        const std::string protocol_url =
            "notion-clipboard-win:/validate-config/?path=" + WideToUtf8(protocol_config_path.wstring()) +
            "&content=upload_target%3Dobsidian%0Aobsidian_vault_dir%3D" + WideToUtf8(protocol_vault.wstring()) +
            "%0Aobsidian_folder%3DInbox%0A";
        const AppConfig protocol_config =
            LoadConfigFromProtocolUrlOrContent(protocol_url, protocol_config_path, &protocol_temp_path);
        if (protocol_config.upload_target != "obsidian" || protocol_config.obsidian_vault_dir != protocol_vault ||
            protocol_config.obsidian_folder != "Inbox" || protocol_temp_path.empty() || !fs::exists(protocol_temp_path))
        {
            fail("validate-config protocol content was not loaded from temporary ini");
        }
        fs::remove(protocol_temp_path, ignored);

        const std::string invalid_hotkey_content =
            "upload_target=obsidian\nobsidian_vault_dir=" + WideToUtf8(protocol_vault.wstring()) +
            "\nhotkey=Ctrl+DefinitelyNotAKey\n";
        bool invalid_hotkey_rejected = false;
        try
        {
            ValidateConfigContentForApply(protocol_config_path, invalid_hotkey_content);
        }
        catch (const std::exception &ex)
        {
            invalid_hotkey_rejected = std::string(ex.what()).find("hotkey") != std::string::npos;
        }
        if (!invalid_hotkey_rejected)
        {
            fail("apply-config validation did not reject invalid hotkey");
        }

        ValidateConfigContentForApply(protocol_config_path,
                                      "upload_target=obsidian\nobsidian_vault_dir=" +
                                          WideToUtf8(protocol_vault.wstring()) + "\nhotkey=Ctrl+Alt+N\n");

        AppConfig test_upload_config;
        test_upload_config.upload_target = "obsidian";
        test_upload_config.state_dir = root / L"test-upload-state";
        test_upload_config.obsidian_vault_dir = root / L"test-upload-vault";
        test_upload_config.obsidian_folder = "Inbox";
        fs::create_directories(test_upload_config.obsidian_vault_dir);
        const bool test_upload_ok = RunConfigTestUpload(test_upload_config, nullptr);
        const fs::path test_upload_report_path = RecentUploadResultsPath(test_upload_config);
        const std::string test_upload_report = ReadWholeFile(test_upload_report_path);
        const fs::path test_upload_inbox = test_upload_config.obsidian_vault_dir / L"Inbox";
        bool test_upload_file_found = false;
        if (fs::exists(test_upload_inbox))
        {
            for (const fs::directory_entry &entry : fs::directory_iterator(test_upload_inbox))
            {
                if (entry.path().extension() == L".md")
                {
                    test_upload_file_found = true;
                    break;
                }
            }
        }
        if (!test_upload_ok || !test_upload_file_found ||
            test_upload_report.find("SUCCESS - obsidian") == std::string::npos ||
            test_upload_report.find("Notion Clipboard Win 测试上传") == std::string::npos)
        {
            fail("configuration test upload did not write obsidian result");
        }

        AppConfig diagnostic_config;
        diagnostic_config.upload_target = "obsidian,notion";
        diagnostic_config.state_dir = root / L"diag-state";
        diagnostic_config.obsidian_vault_dir = root / L"diag-vault";
        diagnostic_config.obsidian_folder = "Inbox";
        fs::create_directories(diagnostic_config.obsidian_vault_dir);
        const fs::path diagnostics_path = root / L"config-diagnostics.md";
        const bool diagnostics_ok = WriteConfigDiagnosticsReport(diagnostics_path, diagnostic_config, nullptr);
        const std::string diagnostics = ReadWholeFile(diagnostics_path);
        if (diagnostics_ok || diagnostics.find("# Configuration Diagnostics") != 0 ||
            diagnostics.find("## obsidian") == std::string::npos ||
            diagnostics.find("- Status: OK") == std::string::npos ||
            diagnostics.find("## notion") == std::string::npos ||
            diagnostics.find("- Status: FAILED") == std::string::npos ||
            diagnostics.find("缺少 Notion token") == std::string::npos ||
            diagnostics.find("- Token: missing") == std::string::npos)
        {
            fail("configuration diagnostics report content mismatch");
        }
    }
    catch (const std::exception &ex)
    {
        fail(ex.what());
    }

    fs::remove_all(root, ignored);
    if (ok)
    {
        std::cout << "[PASS] upload worker result callback\n";
    }
    return ok ? 0 : 1;
}

int AppMain(int argc, wchar_t **argv)
{
#ifndef NOTION_CLIPBOARD_WIN_GUI
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    g_main_thread_id = GetCurrentThreadId();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    MSG bootstrap_msg;
    PeekMessageW(&bootstrap_msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    const CliOptions cli = ParseCli(argc, argv);
    if (!cli.apply_config_url.empty())
    {
        return ApplyConfigUrlAndRestart(cli.apply_config_url);
    }
    if (!cli.open_config_page_url.empty())
    {
        return OpenConfigPageUrl(cli.open_config_page_url);
    }
    if (!cli.validate_config_url.empty())
    {
        return ValidateConfigUrlAndOpenReport(cli.validate_config_url);
    }
    if (!cli.test_upload_url.empty())
    {
        return TestUploadUrlAndOpenReport(cli.test_upload_url);
    }
    if (!cli.preview_obsidian_clipboard_url.empty())
    {
        return PreviewObsidianClipboardUrlAndOpen(cli.preview_obsidian_clipboard_url);
    }
    if (!cli.open_config_diagnostics_url.empty())
    {
        return OpenConfigDiagnosticsUrl(cli.open_config_diagnostics_url);
    }
    if (!cli.open_upload_center_url.empty())
    {
        return OpenUploadCenterUrl(cli.open_upload_center_url);
    }
    if (!cli.retry_failed_job_url.empty())
    {
        return RetryFailedJobUrlAndOpenCenter(cli.retry_failed_job_url);
    }
    if (!cli.retry_failed_uploads_url.empty())
    {
        return RetryFailedUploadsUrlAndOpenCenter(cli.retry_failed_uploads_url);
    }
    if (!cli.open_recent_uploads_url.empty())
    {
        return OpenRecentUploadsUrl(cli.open_recent_uploads_url);
    }
    if (cli.help)
    {
        PrintHelp();
        return 0;
    }
    if (cli.self_test)
    {
        const int converter_result = RunSelfTest();
        if (converter_result != 0)
        {
            return converter_result;
        }
        const int target_result = RunUploadTargetSelfTest();
        if (target_result != 0)
        {
            return target_result;
        }
        const int obsidian_result = RunObsidianSelfTest();
        if (obsidian_result != 0)
        {
            return obsidian_result;
        }
        const int config_page_result = RunConfigPageSelfTest();
        if (config_page_result != 0)
        {
            return config_page_result;
        }
        const int upload_center_result = RunUploadCenterSelfTest();
        if (upload_center_result != 0)
        {
            return upload_center_result;
        }
        return RunMainSelfTest();
    }
    if (!cli.dry_run_file_path.empty())
    {
        return RunDryRunText(ReadWholeFile(cli.dry_run_file_path));
    }
    if (!cli.dry_run_obsidian_input_path.empty())
    {
        const AppConfig config = LoadConfig(cli.config_path);
        return RunObsidianDryRunFile(cli.dry_run_obsidian_input_path, cli.dry_run_obsidian_output_path,
                                     config.obsidian_tags);
    }

    AppConfig config = LoadConfig(cli.config_path);
    std::string startup_config_error;
    try
    {
        ValidateConfigOrThrow(config);
    }
    catch (const std::exception &ex)
    {
        if (cli.validate_config || cli.once)
        {
            throw;
        }
        startup_config_error = ex.what();
    }
    fs::create_directories(config.state_dir);

#ifdef NOTION_CLIPBOARD_WIN_GUI
    const bool mirror_console = false;
#else
    const bool mirror_console = true;
#endif
    Logger logger(config.state_dir / L"notion-clipboard-win.log", mirror_console);
    logger.Info("程序启动，config=" + WideToUtf8(cli.config_path.wstring()));
    if (!cli.validate_config && !cli.once)
    {
        RegisterConfigProtocolHandler(&logger);
    }

    if (cli.validate_config)
    {
        std::unique_ptr<UploadTarget> upload_target = CreateUploadTarget(config, &logger);
        upload_target->Validate();
        logger.Info("配置验证通过");
        return 0;
    }

    std::unique_ptr<UploadTarget> upload_target = startup_config_error.empty()
                                                      ? CreateUploadTarget(config, &logger)
                                                      : std::make_unique<DisabledUploadTarget>(startup_config_error);
    PersistentQueue queue(config.state_dir, config.max_retry_attempts);

    if (cli.once)
    {
        return RunOnce(config, &queue, upload_target.get(), &logger, cli.dry_run);
    }

    UploadWorker worker(&queue, upload_target.get(), &logger);
    if (startup_config_error.empty())
    {
        worker.Start();
    }
    int code = 0;
    try
    {
        TrayApplication app(&config, cli.config_path, &queue, &worker, &logger, startup_config_error,
                            cli.open_config_page_on_start);
        code = app.Run();
    }
    catch (...)
    {
        if (startup_config_error.empty())
        {
            worker.Stop();
        }
        throw;
    }
    if (startup_config_error.empty())
    {
        worker.Stop();
    }
    return code;
}
} // 命名空间

#ifdef NOTION_CLIPBOARD_WIN_GUI
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    try
    {
        return AppMain(argc, argv);
    }
    catch (const std::exception &ex)
    {
        MessageBoxW(nullptr, Utf8ToWide(ex.what()).c_str(), L"Notion Clipboard Win", MB_ICONERROR | MB_OK);
        return 1;
    }
}
#else
int wmain(int argc, wchar_t **argv)
{
    try
    {
        return AppMain(argc, argv);
    }
    catch (const std::exception &ex)
    {
        std::cerr << "错误: " << ex.what() << "\n";
        return 1;
    }
}
#endif
