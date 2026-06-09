#pragma once

#include <filesystem>
#include <string>

namespace ncw
{
std::wstring BuildAutoStartCommand(const std::filesystem::path &config_path);
bool IsAutoStartEnabled(const std::filesystem::path &config_path);
bool SetAutoStartEnabled(bool enabled, const std::filesystem::path &config_path, std::string *error);
}
