#pragma once

#include <filesystem>

namespace ncw
{
struct AppConfig;

std::filesystem::path WriteUploadCenterPage(const AppConfig &config);
int RunUploadCenterSelfTest();
}
