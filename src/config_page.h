#pragma once

#include <filesystem>

namespace ncw
{
struct AppConfig;

std::filesystem::path WriteConfigPage(const AppConfig &config, const std::filesystem::path &config_path);
int RunConfigPageSelfTest();
}
