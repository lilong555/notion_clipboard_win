#pragma once

#include <cstddef>
#include <filesystem>

namespace ncw
{
struct AppConfig;

std::filesystem::path WriteUploadCenterPage(const AppConfig &config, const std::filesystem::path &config_path);
std::size_t RetryFailedUploads(const AppConfig &config);
int RunUploadCenterSelfTest();
}
