#include "config.h"

#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <stdexcept>

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

namespace
{
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
    else if (normalized == "github_token")
    {
        config->github_token = trimmed_value;
    }
    else if (normalized == "github_gist_public")
    {
        config->github_gist_public = ParseBool(trimmed_value);
    }
    else if (normalized == "github_gist_filename_prefix")
    {
        if (!trimmed_value.empty())
        {
            config->github_gist_filename_prefix = trimmed_value;
        }
    }
    else if (normalized == "github_repo_owner")
    {
        config->github_repo_owner = trimmed_value;
    }
    else if (normalized == "github_repo_name")
    {
        config->github_repo_name = trimmed_value;
    }
    else if (normalized == "github_repo_branch")
    {
        config->github_repo_branch = trimmed_value;
    }
    else if (normalized == "github_repo_directory")
    {
        config->github_repo_directory = trimmed_value;
    }
    else if (normalized == "github_repo_filename_prefix")
    {
        if (!trimmed_value.empty())
        {
            config->github_repo_filename_prefix = trimmed_value;
        }
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
    else if (normalized == "obsidian_filename_prefix")
    {
        if (!trimmed_value.empty())
        {
            config->obsidian_filename_prefix = trimmed_value;
        }
    }
    else if (normalized == "local_git_repo_dir")
    {
        if (!trimmed_value.empty())
        {
            config->local_git_repo_dir = fs::path(Utf8ToWide(trimmed_value));
        }
    }
    else if (normalized == "local_git_directory")
    {
        config->local_git_directory = trimmed_value;
    }
    else if (normalized == "local_git_filename_prefix")
    {
        if (!trimmed_value.empty())
        {
            config->local_git_filename_prefix = trimmed_value;
        }
    }
    else if (normalized == "local_git_auto_commit")
    {
        config->local_git_auto_commit = ParseBool(trimmed_value);
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
    if (config.github_token.empty())
    {
        std::string github_token = GetEnvUtf8(L"NCW_GITHUB_TOKEN");
        if (github_token.empty())
        {
            github_token = GetEnvUtf8(L"GITHUB_TOKEN");
        }
        config.github_token = github_token;
    }
    if (const std::string gist_public = GetEnvUtf8(L"NCW_GITHUB_GIST_PUBLIC"); !gist_public.empty())
    {
        config.github_gist_public = ParseBool(gist_public);
    }
    if (const std::string gist_prefix = GetEnvUtf8(L"NCW_GITHUB_GIST_FILENAME_PREFIX"); !gist_prefix.empty())
    {
        config.github_gist_filename_prefix = Trim(gist_prefix);
    }
    if (config.github_repo_owner.empty())
    {
        config.github_repo_owner = Trim(GetEnvUtf8(L"NCW_GITHUB_REPO_OWNER"));
    }
    if (config.github_repo_name.empty())
    {
        config.github_repo_name = Trim(GetEnvUtf8(L"NCW_GITHUB_REPO_NAME"));
    }
    if (config.github_repo_branch.empty())
    {
        config.github_repo_branch = Trim(GetEnvUtf8(L"NCW_GITHUB_REPO_BRANCH"));
    }
    if (const std::string repo_dir = GetEnvUtf8(L"NCW_GITHUB_REPO_DIRECTORY"); !repo_dir.empty())
    {
        config.github_repo_directory = Trim(repo_dir);
    }
    if (const std::string repo_prefix = GetEnvUtf8(L"NCW_GITHUB_REPO_FILENAME_PREFIX"); !repo_prefix.empty())
    {
        config.github_repo_filename_prefix = Trim(repo_prefix);
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
    if (const std::string obsidian_prefix = GetEnvUtf8(L"NCW_OBSIDIAN_FILENAME_PREFIX"); !obsidian_prefix.empty())
    {
        config.obsidian_filename_prefix = Trim(obsidian_prefix);
    }
    if (config.local_git_repo_dir.empty())
    {
        if (const std::string repo_dir = GetEnvUtf8(L"NCW_LOCAL_GIT_REPO_DIR"); !repo_dir.empty())
        {
            config.local_git_repo_dir = fs::path(Utf8ToWide(repo_dir));
        }
    }
    if (const std::string git_dir = GetEnvUtf8(L"NCW_LOCAL_GIT_DIRECTORY"); !git_dir.empty())
    {
        config.local_git_directory = Trim(git_dir);
    }
    if (const std::string git_prefix = GetEnvUtf8(L"NCW_LOCAL_GIT_FILENAME_PREFIX"); !git_prefix.empty())
    {
        config.local_git_filename_prefix = Trim(git_prefix);
    }
    if (const std::string git_auto_commit = GetEnvUtf8(L"NCW_LOCAL_GIT_AUTO_COMMIT"); !git_auto_commit.empty())
    {
        config.local_git_auto_commit = ParseBool(git_auto_commit);
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
        else if (arg == L"--self-test")
        {
            options.self_test = true;
        }
        else if (arg == L"--config")
        {
            if (i + 1 >= argc)
            {
                throw std::runtime_error("--config 缺少路径");
            }
            options.config_path = fs::path(argv[++i]);
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
              << "默认热键:\n"
              << "  Ctrl+Shift+B\n\n"
              << "上传后端:\n"
              << "  upload_target=notion、markdown_file、obsidian、local_git、webhook、github_gist、github_repo、yuque 或 feishu_doc\n\n"
              << "配置默认路径:\n"
              << "  " << WideToUtf8(DefaultConfigPath().wstring()) << "\n";
}

void ValidateConfigOrThrow(const AppConfig &config)
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
        if (Trim(config.obsidian_filename_prefix).empty())
        {
            throw std::runtime_error("obsidian_filename_prefix 不能为空");
        }
        return;
    }
    if (config.upload_target == "local_git")
    {
        if (config.local_git_repo_dir.empty())
        {
            throw std::runtime_error("local_git_repo_dir 不能为空");
        }
        if (!fs::exists(config.local_git_repo_dir) || !fs::is_directory(config.local_git_repo_dir))
        {
            throw std::runtime_error("local_git_repo_dir 不存在或不是目录");
        }
        if (!fs::exists(config.local_git_repo_dir / L".git"))
        {
            throw std::runtime_error("local_git_repo_dir 不是 Git 工作区");
        }
        if (Trim(config.local_git_filename_prefix).empty())
        {
            throw std::runtime_error("local_git_filename_prefix 不能为空");
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
    if (config.upload_target == "github_gist")
    {
        if (config.github_token.empty())
        {
            throw std::runtime_error("缺少 GitHub token，请在配置里设置 github_token 或设置 NCW_GITHUB_TOKEN");
        }
        if (Trim(config.github_gist_filename_prefix).empty())
        {
            throw std::runtime_error("github_gist_filename_prefix 不能为空");
        }
        return;
    }
    if (config.upload_target == "github_repo")
    {
        if (config.github_token.empty())
        {
            throw std::runtime_error("缺少 GitHub token，请在配置里设置 github_token 或设置 NCW_GITHUB_TOKEN");
        }
        if (Trim(config.github_repo_owner).empty())
        {
            throw std::runtime_error("github_repo_owner 不能为空");
        }
        if (Trim(config.github_repo_name).empty())
        {
            throw std::runtime_error("github_repo_name 不能为空");
        }
        if (Trim(config.github_repo_filename_prefix).empty())
        {
            throw std::runtime_error("github_repo_filename_prefix 不能为空");
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
    throw std::runtime_error("未知 upload_target: " + config.upload_target +
                             "，支持 notion、markdown_file、obsidian、local_git、webhook、github_gist、github_repo、yuque 或 feishu_doc");
}
}
