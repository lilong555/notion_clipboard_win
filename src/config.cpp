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
    if (normalized == "notion_token")
    {
        config->notion_token = trimmed_value;
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

    if (config.notion_token.empty())
    {
        config.notion_token = GetEnvUtf8(L"NOTION_TOKEN");
    }
    if (config.data_source_id.empty())
    {
        config.data_source_id = CanonicalizeNotionId(GetEnvUtf8(L"NOTION_DATA_SOURCE_ID"));
    }
    if (config.database_id.empty())
    {
        config.database_id = CanonicalizeNotionId(GetEnvUtf8(L"NOTION_DATABASE_ID"));
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
              << "  notion_clipboard_win.exe --validate-config            验证 Notion 配置\n"
              << "  notion_clipboard_win.exe --dry-run --once             读取剪贴板但不上传\n\n"
              << "  notion_clipboard_win.exe --self-test                  运行本地转换回归测试\n\n"
              << "  notion_clipboard_win.exe --dry-run-file path          读取文件并转换统计，不上传\n\n"
              << "默认热键:\n"
              << "  Ctrl+Shift+B\n\n"
              << "配置默认路径:\n"
              << "  " << WideToUtf8(DefaultConfigPath().wstring()) << "\n";
}

void ValidateConfigOrThrow(const AppConfig &config)
{
    if (config.notion_token.empty())
    {
        throw std::runtime_error("缺少 Notion token，请在配置里设置 notion_token 或设置 NOTION_TOKEN");
    }
    if (config.data_source_id.empty() && config.database_id.empty())
    {
        throw std::runtime_error("缺少 data_source_id 或 database_id");
    }
}
}
