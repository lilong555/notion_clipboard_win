#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace ncw
{
struct AppConfig
{
    AppConfig();

    std::string notion_token;
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
    int debounce_ms = 750;
    int duplicate_suppression_ms = 3000;
    bool upload_initial_clipboard = false;
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
    bool once = false;
    bool validate_config = false;
    bool dry_run = false;
    bool self_test = false;
    bool help = false;
};

std::string CanonicalizeNotionId(std::string input);
AppConfig LoadConfig(const std::filesystem::path &path);
CliOptions ParseCli(int argc, wchar_t **argv);
void PrintHelp();
void ValidateConfigOrThrow(const AppConfig &config);
}
