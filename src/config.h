#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ncw
{
struct AppConfig
{
    AppConfig();

    std::string upload_target = "notion";
    std::string notion_token;
    std::string webhook_url;
    std::string webhook_bearer_token;
    std::string yuque_token;
    std::string yuque_namespace;
    std::string yuque_slug_prefix = "clipboard";
    std::string feishu_app_id;
    std::string feishu_app_secret;
    std::string feishu_folder_token;
    std::string data_source_id;
    std::string database_id;
    std::string title_property_name;
    std::string content_property_name;
    std::string created_time_property_name = "创建时间";
    int content_property_max_chars = 1800;
    std::filesystem::path state_dir;
    std::string hotkey = "Ctrl+Shift+B";
    bool enable_hotkey = true;
    bool enable_clipboard_listener = false;
    bool tray_notifications = true;
    bool start_with_windows = false;
    bool start_with_windows_configured = false;
    int debounce_ms = 750;
    int duplicate_suppression_ms = 3000;
    bool upload_initial_clipboard = false;
    std::filesystem::path markdown_output_dir;
    std::filesystem::path obsidian_vault_dir;
    std::string obsidian_folder = "Clipboard";
    std::string obsidian_tags;
    std::uint64_t max_clipboard_bytes = 262144;
    int min_request_interval_ms = 400;
    int append_batch_size = 40;
    int max_retry_attempts = 12;
    int http_retry_attempts = 3;
};

struct CliOptions
{
    CliOptions();

    std::filesystem::path config_path;
    std::filesystem::path dry_run_file_path;
    std::string apply_config_url;
    std::string open_config_page_url;
    std::string validate_config_url;
    std::string open_config_diagnostics_url;
    std::string open_recent_uploads_url;
    bool once = false;
    bool validate_config = false;
    bool dry_run = false;
    bool self_test = false;
    bool help = false;
    bool open_config_page_on_start = false;
};

std::string CanonicalizeNotionId(std::string input);
std::vector<std::string> ParseUploadTargets(const std::string &value);
AppConfig LoadConfig(const std::filesystem::path &path);
CliOptions ParseCli(int argc, wchar_t **argv);
void PrintHelp();
void ValidateConfigOrThrow(const AppConfig &config);
}
