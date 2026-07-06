#include "config.h"

#include "hotkey.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace ncw
{
AppConfig::AppConfig() : state_dir(DefaultStateDir())
{
}

CliOptions::CliOptions() : config_path(DefaultConfigPath())
{
}

std::string CanonicalizeNotionId(std::string input)
{
    input = Trim(input);
    std::string hex;
    for (unsigned char ch : input)
    {
        if (std::isxdigit(ch))
        {
            hex.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    if (hex.size() != 32)
    {
        return input;
    }
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" +
           hex.substr(20);
}

std::vector<std::string> ParseUploadTargets(const std::string &value)
{
    std::vector<std::string> targets;
    std::string token;
    const std::string normalized = ToLowerAscii(value);
    auto flush = [&]
    {
        token = Trim(token);
        if (!token.empty() && std::find(targets.begin(), targets.end(), token) == targets.end())
        {
            targets.push_back(token);
        }
        token.clear();
    };

    for (unsigned char ch : normalized)
    {
        if (ch == ',' || ch == ';' || ch == '|' || std::isspace(ch))
        {
            flush();
        }
        else
        {
            token.push_back(static_cast<char>(ch));
        }
    }
    flush();
    return targets;
}

namespace
{
std::string SupportedUploadTargetsText()
{
    return "notion、obsidian（推荐稳定）；markdown_file、webhook、yuque、feishu_doc 为实验/未来兼容目标";
}

void ApplyConfigValue(AppConfig *config, const std::string &key, const std::string &value)
{
    const std::string normalized = ToLowerAscii(Trim(key));
    const std::string trimmed_value = Trim(value);
    if (normalized == "upload_target")
    {
        if (!trimmed_value.empty())
        {
            config->upload_target = ToLowerAscii(trimmed_value);
        }
    }
    else if (normalized == "notion_token")
    {
        config->notion_token = trimmed_value;
    }
    else if (normalized == "webhook_url")
    {
        config->webhook_url = trimmed_value;
    }
    else if (normalized == "webhook_bearer_token")
    {
        config->webhook_bearer_token = trimmed_value;
    }
    else if (normalized == "yuque_token")
    {
        config->yuque_token = trimmed_value;
    }
    else if (normalized == "yuque_namespace")
    {
        config->yuque_namespace = trimmed_value;
    }
    else if (normalized == "yuque_slug_prefix")
    {
        if (!trimmed_value.empty())
        {
            config->yuque_slug_prefix = trimmed_value;
        }
    }
    else if (normalized == "feishu_app_id")
    {
        config->feishu_app_id = trimmed_value;
    }
    else if (normalized == "feishu_app_secret")
    {
        config->feishu_app_secret = trimmed_value;
    }
    else if (normalized == "feishu_folder_token")
    {
        config->feishu_folder_token = trimmed_value;
    }
    else if (normalized == "data_source_id")
    {
        config->data_source_id = CanonicalizeNotionId(trimmed_value);
    }
    else if (normalized == "database_id")
    {
        config->database_id = CanonicalizeNotionId(trimmed_value);
    }
    else if (normalized == "title_property_name")
    {
        config->title_property_name = trimmed_value;
    }
    else if (normalized == "content_property_name")
    {
        config->content_property_name = trimmed_value;
    }
    else if (normalized == "created_time_property_name")
    {
        config->created_time_property_name = trimmed_value;
    }
    else if (normalized == "content_property_max_chars")
    {
        config->content_property_max_chars = std::max(100, ParseIntOrDefault(trimmed_value, 1800));
    }
    else if (normalized == "state_dir")
    {
        if (!trimmed_value.empty())
        {
            config->state_dir = fs::path(Utf8ToWide(trimmed_value));
        }
    }
    else if (normalized == "hotkey")
    {
        if (!trimmed_value.empty())
        {
            config->hotkey = trimmed_value;
        }
    }
    else if (normalized == "enable_hotkey")
    {
        config->enable_hotkey = ParseBool(trimmed_value);
    }
    else if (normalized == "enable_clipboard_listener")
    {
        config->enable_clipboard_listener = ParseBool(trimmed_value);
    }
    else if (normalized == "tray_notifications")
    {
        config->tray_notifications = ParseBool(trimmed_value);
    }
    else if (normalized == "start_with_windows")
    {
        config->start_with_windows = ParseBool(trimmed_value);
        config->start_with_windows_configured = true;
    }
    else if (normalized == "debounce_ms")
    {
        config->debounce_ms = std::max(100, ParseIntOrDefault(trimmed_value, 750));
    }
    else if (normalized == "duplicate_suppression_ms")
    {
        config->duplicate_suppression_ms = std::max(0, ParseIntOrDefault(trimmed_value, 3000));
    }
    else if (normalized == "upload_initial_clipboard")
    {
        config->upload_initial_clipboard = ParseBool(trimmed_value);
    }
    else if (normalized == "markdown_output_dir")
    {
        if (!trimmed_value.empty())
        {
            config->markdown_output_dir = fs::path(Utf8ToWide(trimmed_value));
        }
    }
    else if (normalized == "obsidian_vault_dir")
    {
        if (!trimmed_value.empty())
        {
            config->obsidian_vault_dir = fs::path(Utf8ToWide(trimmed_value));
        }
    }
    else if (normalized == "obsidian_folder")
    {
        config->obsidian_folder = trimmed_value;
    }
    else if (normalized == "obsidian_tags")
    {
        config->obsidian_tags = trimmed_value;
    }
    else if (normalized == "max_clipboard_bytes")
    {
        config->max_clipboard_bytes = std::max<std::uint64_t>(1024, ParseU64OrDefault(trimmed_value, 262144));
    }
    else if (normalized == "min_request_interval_ms")
    {
        config->min_request_interval_ms = std::max(0, ParseIntOrDefault(trimmed_value, 400));
    }
    else if (normalized == "append_batch_size")
    {
        config->append_batch_size = std::max(1, std::min(90, ParseIntOrDefault(trimmed_value, 40)));
    }
    else if (normalized == "max_retry_attempts")
    {
        config->max_retry_attempts = std::max(0, ParseIntOrDefault(trimmed_value, 12));
    }
    else if (normalized == "http_retry_attempts")
    {
        config->http_retry_attempts = std::max(0, std::min(8, ParseIntOrDefault(trimmed_value, 3)));
    }
}

bool IsApplyConfigProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"apply-config") != std::wstring::npos;
}

bool IsOpenConfigPageProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"open-config-page") != std::wstring::npos;
}

bool IsValidateConfigProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"validate-config") != std::wstring::npos;
}

bool IsTestUploadProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"test-upload") != std::wstring::npos;
}

bool IsPreviewObsidianClipboardProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 &&
           arg.find(L"preview-obsidian-clipboard") != std::wstring::npos;
}

bool IsOpenConfigDiagnosticsProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"open-config-diagnostics") != std::wstring::npos;
}

bool IsOpenUploadCenterProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"open-upload-center") != std::wstring::npos;
}

bool IsRetryFailedUploadsProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"retry-failed-uploads") != std::wstring::npos;
}

bool IsRetryFailedJobProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"retry-failed-job") != std::wstring::npos;
}

bool IsOpenRecentUploadsProtocolUrl(const std::wstring &arg)
{
    return arg.rfind(L"notion-clipboard-win:", 0) == 0 && arg.find(L"open-recent-uploads") != std::wstring::npos;
}
}

AppConfig LoadConfig(const fs::path &path)
{
    AppConfig config;
    if (fs::exists(path))
    {
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
            ApplyConfigValue(&config, line.substr(0, eq), line.substr(eq + 1));
        }
    }

    if (const std::string upload_target = GetEnvUtf8(L"NCW_UPLOAD_TARGET"); !upload_target.empty())
    {
        config.upload_target = ToLowerAscii(Trim(upload_target));
    }
    if (config.notion_token.empty())
    {
        config.notion_token = GetEnvUtf8(L"NOTION_TOKEN");
    }
    if (config.webhook_url.empty())
    {
        config.webhook_url = GetEnvUtf8(L"NCW_WEBHOOK_URL");
    }
    if (config.webhook_bearer_token.empty())
    {
        config.webhook_bearer_token = GetEnvUtf8(L"NCW_WEBHOOK_BEARER_TOKEN");
    }
    if (config.yuque_token.empty())
    {
        config.yuque_token = GetEnvUtf8(L"NCW_YUQUE_TOKEN");
    }
    if (config.yuque_namespace.empty())
    {
        config.yuque_namespace = Trim(GetEnvUtf8(L"NCW_YUQUE_NAMESPACE"));
    }
    if (const std::string yuque_slug_prefix = GetEnvUtf8(L"NCW_YUQUE_SLUG_PREFIX"); !yuque_slug_prefix.empty())
    {
        config.yuque_slug_prefix = Trim(yuque_slug_prefix);
    }
    if (config.feishu_app_id.empty())
    {
        config.feishu_app_id = GetEnvUtf8(L"NCW_FEISHU_APP_ID");
    }
    if (config.feishu_app_secret.empty())
    {
        config.feishu_app_secret = GetEnvUtf8(L"NCW_FEISHU_APP_SECRET");
    }
    if (config.feishu_folder_token.empty())
    {
        config.feishu_folder_token = GetEnvUtf8(L"NCW_FEISHU_FOLDER_TOKEN");
    }
    if (config.data_source_id.empty())
    {
        config.data_source_id = CanonicalizeNotionId(GetEnvUtf8(L"NOTION_DATA_SOURCE_ID"));
    }
    if (config.database_id.empty())
    {
        config.database_id = CanonicalizeNotionId(GetEnvUtf8(L"NOTION_DATABASE_ID"));
    }
    if (config.markdown_output_dir.empty())
    {
        if (const std::string output_dir = GetEnvUtf8(L"NCW_MARKDOWN_OUTPUT_DIR"); !output_dir.empty())
        {
            config.markdown_output_dir = fs::path(Utf8ToWide(output_dir));
        }
    }
    if (config.obsidian_vault_dir.empty())
    {
        if (const std::string vault_dir = GetEnvUtf8(L"NCW_OBSIDIAN_VAULT_DIR"); !vault_dir.empty())
        {
            config.obsidian_vault_dir = fs::path(Utf8ToWide(vault_dir));
        }
    }
    if (const std::string obsidian_folder = GetEnvUtf8(L"NCW_OBSIDIAN_FOLDER"); !obsidian_folder.empty())
    {
        config.obsidian_folder = Trim(obsidian_folder);
    }
    if (const std::string obsidian_tags = GetEnvUtf8(L"NCW_OBSIDIAN_TAGS"); !obsidian_tags.empty())
    {
        config.obsidian_tags = Trim(obsidian_tags);
    }
    return config;
}

CliOptions ParseCli(int argc, wchar_t **argv)
{
    CliOptions options;
    for (int i = 1; i < argc; ++i)
    {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h" || arg == L"/?")
        {
            options.help = true;
        }
        else if (arg == L"--once")
        {
            options.once = true;
        }
        else if (arg == L"--validate-config")
        {
            options.validate_config = true;
        }
        else if (arg == L"--dry-run")
        {
            options.dry_run = true;
        }
        else if (arg == L"--dry-run-file")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--dry-run-file 缺少路径");
            }
            options.dry_run = true;
            options.dry_run_file_path = fs::path(argv[++i]);
        }
        else if (arg == L"--dry-run-obsidian-file")
        {
            if (i + 2 >= argc)
            {
                throw std::runtime_error("--dry-run-obsidian-file 缺少输入或输出路径");
            }
            options.dry_run_obsidian_input_path = fs::path(argv[++i]);
            options.dry_run_obsidian_output_path = fs::path(argv[++i]);
        }
        else if (arg == L"--self-test")
        {
            options.self_test = true;
        }
        else if (arg == L"--open-config-page-on-start")
        {
            options.open_config_page_on_start = true;
        }
        else if (arg == L"--config")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--config 缺少路径");
            }
            options.config_path = fs::path(argv[++i]);
        }
        else if (arg == L"--apply-config-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--apply-config-url 缺少 URL");
            }
            options.apply_config_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--open-config-page-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--open-config-page-url 缺少 URL");
            }
            options.open_config_page_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--validate-config-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--validate-config-url 缺少 URL");
            }
            options.validate_config_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--test-upload-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--test-upload-url 缺少 URL");
            }
            options.test_upload_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--preview-obsidian-clipboard-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--preview-obsidian-clipboard-url 缺少 URL");
            }
            options.preview_obsidian_clipboard_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--open-config-diagnostics-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--open-config-diagnostics-url 缺少 URL");
            }
            options.open_config_diagnostics_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--open-upload-center-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--open-upload-center-url 缺少 URL");
            }
            options.open_upload_center_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--retry-failed-uploads-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--retry-failed-uploads-url 缺少 URL");
            }
            options.retry_failed_uploads_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--retry-failed-job-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--retry-failed-job-url 缺少 URL");
            }
            options.retry_failed_job_url = WideToUtf8(argv[++i]);
        }
        else if (arg == L"--open-recent-uploads-url")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--open-recent-uploads-url 缺少 URL");
            }
            options.open_recent_uploads_url = WideToUtf8(argv[++i]);
        }
        else if (IsApplyConfigProtocolUrl(arg))
        {
            options.apply_config_url = WideToUtf8(arg);
        }
        else if (IsOpenConfigPageProtocolUrl(arg))
        {
            options.open_config_page_url = WideToUtf8(arg);
        }
        else if (IsValidateConfigProtocolUrl(arg))
        {
            options.validate_config_url = WideToUtf8(arg);
        }
        else if (IsTestUploadProtocolUrl(arg))
        {
            options.test_upload_url = WideToUtf8(arg);
        }
        else if (IsPreviewObsidianClipboardProtocolUrl(arg))
        {
            options.preview_obsidian_clipboard_url = WideToUtf8(arg);
        }
        else if (IsOpenConfigDiagnosticsProtocolUrl(arg))
        {
            options.open_config_diagnostics_url = WideToUtf8(arg);
        }
        else if (IsOpenUploadCenterProtocolUrl(arg))
        {
            options.open_upload_center_url = WideToUtf8(arg);
        }
        else if (IsRetryFailedUploadsProtocolUrl(arg))
        {
            options.retry_failed_uploads_url = WideToUtf8(arg);
        }
        else if (IsRetryFailedJobProtocolUrl(arg))
        {
            options.retry_failed_job_url = WideToUtf8(arg);
        }
        else if (IsOpenRecentUploadsProtocolUrl(arg))
        {
            options.open_recent_uploads_url = WideToUtf8(arg);
        }
        else
        {
            throw std::runtime_error("未知参数: " + WideToUtf8(arg));
        }
    }
    return options;
}

void PrintHelp()
{
    std::cout << "Notion Clipboard Win\n\n"
              << "用法:\n"
              << "  notion_clipboard_win.exe [--config path]              启动后台托盘进程\n"
              << "  notion_clipboard_win.exe --once [--config path]       只上传当前剪贴板一次\n"
              << "  notion_clipboard_win.exe --validate-config            验证上传后端配置\n"
              << "  notion_clipboard_win.exe --dry-run --once             读取剪贴板但不上传\n\n"
              << "  notion_clipboard_win.exe --self-test                  运行本地转换回归测试\n\n"
              << "  notion_clipboard_win.exe --dry-run-file path          读取文件并转换统计，不上传\n\n"
              << "  notion_clipboard_win.exe --dry-run-obsidian-file in out 生成 Obsidian Markdown 预览\n\n"
              << "  notion_clipboard_win.exe --test-upload-url URL        使用配置页内容测试上传一次\n\n"
              << "  notion_clipboard_win.exe --preview-obsidian-clipboard-url URL 预览剪贴板 Obsidian Markdown\n\n"
              << "  notion_clipboard_win.exe --open-upload-center-url URL 打开上传中心\n\n"
              << "  notion_clipboard_win.exe --retry-failed-job-url URL   重试单个 failed 任务\n\n"
              << "  notion_clipboard_win.exe --retry-failed-uploads-url URL 重试 failed 队列任务\n\n"
              << "  notion_clipboard_win.exe --open-config-page-on-start  启动后打开配置页一次\n\n"
              << "默认热键:\n"
              << "  Ctrl+Shift+B\n\n"
              << "上传后端:\n"
              << "  upload_target=notion 或 upload_target=notion,obsidian\n"
              << "  当前主路径: Notion、Obsidian；" << SupportedUploadTargetsText() << "\n\n"
              << "配置默认路径:\n"
              << "  " << WideToUtf8(DefaultConfigPath().wstring()) << "\n";
}

namespace
{
void ValidateSingleUploadTargetOrThrow(const AppConfig &config)
{
    if (config.upload_target == "notion")
    {
        if (config.notion_token.empty())
        {
            throw std::runtime_error("缺少 Notion token，请在配置里设置 notion_token 或设置 NOTION_TOKEN");
        }
        if (config.data_source_id.empty() && config.database_id.empty())
        {
            throw std::runtime_error("缺少 data_source_id 或 database_id");
        }
        return;
    }
    if (config.upload_target == "markdown_file")
    {
        return;
    }
    if (config.upload_target == "obsidian")
    {
        if (config.obsidian_vault_dir.empty())
        {
            throw std::runtime_error("obsidian_vault_dir 不能为空");
        }
        if (!fs::exists(config.obsidian_vault_dir) || !fs::is_directory(config.obsidian_vault_dir))
        {
            throw std::runtime_error("obsidian_vault_dir 不存在或不是目录");
        }
        return;
    }
    if (config.upload_target == "webhook")
    {
        if (config.webhook_url.empty())
        {
            throw std::runtime_error("缺少 webhook_url");
        }
        const std::string lower_url = ToLowerAscii(config.webhook_url);
        if (lower_url.rfind("https://", 0) != 0 && lower_url.rfind("http://", 0) != 0)
        {
            throw std::runtime_error("webhook_url 只支持 http 或 https");
        }
        if (config.webhook_url.find_first_of(" \t\r\n") != std::string::npos)
        {
            throw std::runtime_error("webhook_url 不能包含空白字符");
        }
        return;
    }
    if (config.upload_target == "yuque")
    {
        if (config.yuque_token.empty())
        {
            throw std::runtime_error("yuque_token 不能为空");
        }
        if (Trim(config.yuque_namespace).empty())
        {
            throw std::runtime_error("yuque_namespace 不能为空");
        }
        if (Trim(config.yuque_slug_prefix).empty())
        {
            throw std::runtime_error("yuque_slug_prefix 不能为空");
        }
        return;
    }
    if (config.upload_target == "feishu_doc")
    {
        if (config.feishu_app_id.empty())
        {
            throw std::runtime_error("feishu_app_id 不能为空");
        }
        if (config.feishu_app_secret.empty())
        {
            throw std::runtime_error("feishu_app_secret 不能为空");
        }
        return;
    }
    throw std::runtime_error("未知 upload_target: " + config.upload_target + "，支持 " + SupportedUploadTargetsText());
}
}

void ValidateConfigOrThrow(const AppConfig &config)
{
    try
    {
        ParseHotkeyOrThrow(config.hotkey);
    }
    catch (const std::exception &ex)
    {
        throw std::runtime_error("hotkey 配置无效: " + std::string(ex.what()));
    }

    const std::vector<std::string> targets = ParseUploadTargets(config.upload_target);
    if (targets.empty())
    {
        throw std::runtime_error("upload_target 不能为空，支持 " + SupportedUploadTargetsText());
    }

    for (const std::string &target : targets)
    {
        AppConfig target_config = config;
        target_config.upload_target = target;
        try
        {
            ValidateSingleUploadTargetOrThrow(target_config);
        }
        catch (const std::exception &ex)
        {
            throw std::runtime_error("upload_target=" + target + " 配置无效: " + ex.what());
        }
    }
}
}
