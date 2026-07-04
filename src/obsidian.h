#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ncw
{
struct ObsidianVault
{
    std::string id;
    std::string name;
    std::filesystem::path path;
    bool open = false;
};

std::vector<ObsidianVault> ParseObsidianVaultRegistry(const std::string &json_text);
std::vector<ObsidianVault> DiscoverObsidianVaults();
std::optional<ObsidianVault> FindRegisteredObsidianVault(const std::filesystem::path &vault_dir);
std::string BuildObsidianOpenUri(const std::string &vault_name, const std::string &relative_file);
std::string BuildObsidianOpenUri(const std::filesystem::path &vault_dir, const std::filesystem::path &file_path);

int RunObsidianSelfTest();
}
