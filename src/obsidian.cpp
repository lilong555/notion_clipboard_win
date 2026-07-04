#include "obsidian.h"

#include "json.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace ncw
{
namespace
{
std::string PathValue(const fs::path &path)
{
    return path.empty() ? "" : WideToUtf8(path.wstring());
}

std::string VaultNameFromPath(const fs::path &path, const std::string &fallback)
{
    const fs::path filename = path.filename();
    if (!filename.empty())
    {
        return WideToUtf8(filename.wstring());
    }
    return fallback;
}

std::string NormalizePathKey(const fs::path &input)
{
    if (input.empty())
    {
        return "";
    }

    std::error_code ec;
    fs::path path = input.is_absolute() ? input : fs::absolute(input, ec);
    if (ec)
    {
        path = input;
        ec.clear();
    }

    fs::path normalized = fs::weakly_canonical(path, ec);
    if (ec)
    {
        normalized = path.lexically_normal();
    }

    std::wstring wide = normalized.wstring();
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    while (wide.size() > 3 && (wide.back() == L'\\' || wide.back() == L'/'))
    {
        wide.pop_back();
    }
    return ToLowerAscii(WideToUtf8(wide));
}

std::string PercentEncodeUriComponent(const std::string &input)
{
    std::ostringstream output;
    output << std::uppercase << std::hex;
    for (unsigned char ch : input)
    {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == '~')
        {
            output << static_cast<char>(ch);
        }
        else
        {
            output << '%' << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
    }
    return output.str();
}

std::optional<fs::path> RelativePathWithinVault(const fs::path &vault_dir, const fs::path &file_path)
{
    std::error_code ec;
    fs::path vault = fs::weakly_canonical(vault_dir, ec);
    if (ec)
    {
        ec.clear();
        vault = (vault_dir.is_absolute() ? vault_dir : fs::absolute(vault_dir, ec)).lexically_normal();
    }

    ec.clear();
    fs::path file = fs::weakly_canonical(file_path, ec);
    if (ec)
    {
        ec.clear();
        file = (file_path.is_absolute() ? file_path : fs::absolute(file_path, ec)).lexically_normal();
    }

    const fs::path relative = file.lexically_relative(vault);
    if (relative.empty() || relative.is_absolute())
    {
        return std::nullopt;
    }
    for (const fs::path &part : relative)
    {
        if (part == L"..")
        {
            return std::nullopt;
        }
    }
    return relative;
}
}

std::vector<ObsidianVault> ParseObsidianVaultRegistry(const std::string &json_text)
{
    std::vector<ObsidianVault> vaults;
    try
    {
        const JsonValue root = ParseJson(json_text);
        const JsonValue *vaults_json = root.find("vaults");
        if (vaults_json == nullptr || !vaults_json->is_object())
        {
            return vaults;
        }

        for (const auto &item : vaults_json->as_object())
        {
            const JsonValue *path_json = item.second.find("path");
            if (path_json == nullptr || !path_json->is_string() || path_json->as_string().empty())
            {
                continue;
            }

            ObsidianVault vault;
            vault.id = item.first;
            vault.path = fs::path(Utf8ToWide(path_json->as_string()));
            vault.name = VaultNameFromPath(vault.path, vault.id);
            const JsonValue *open_json = item.second.find("open");
            vault.open = open_json != nullptr && open_json->is_bool() && open_json->as_bool();
            vaults.push_back(std::move(vault));
        }
    }
    catch (...)
    {
        vaults.clear();
    }

    std::sort(vaults.begin(), vaults.end(), [](const ObsidianVault &left, const ObsidianVault &right)
              {
                  if (left.open != right.open)
                  {
                      return left.open && !right.open;
                  }
                  if (left.name != right.name)
                  {
                      return left.name < right.name;
                  }
                  return PathValue(left.path) < PathValue(right.path);
              });
    return vaults;
}

std::vector<ObsidianVault> DiscoverObsidianVaults()
{
    const std::wstring app_data = GetEnvWide(L"APPDATA");
    if (app_data.empty())
    {
        return {};
    }
    const fs::path registry = fs::path(app_data) / L"obsidian" / L"obsidian.json";
    std::error_code ec;
    if (!fs::exists(registry, ec))
    {
        return {};
    }
    try
    {
        return ParseObsidianVaultRegistry(ReadWholeFile(registry));
    }
    catch (...)
    {
        return {};
    }
}

std::optional<ObsidianVault> FindRegisteredObsidianVault(const fs::path &vault_dir)
{
    const std::string target = NormalizePathKey(vault_dir);
    if (target.empty())
    {
        return std::nullopt;
    }

    for (const ObsidianVault &vault : DiscoverObsidianVaults())
    {
        if (NormalizePathKey(vault.path) == target)
        {
            return vault;
        }
    }
    return std::nullopt;
}

std::string BuildObsidianOpenUri(const std::string &vault_name, const std::string &relative_file)
{
    if (Trim(vault_name).empty() || Trim(relative_file).empty())
    {
        return "";
    }
    return "obsidian://open?vault=" + PercentEncodeUriComponent(vault_name) +
           "&file=" + PercentEncodeUriComponent(relative_file);
}

std::string BuildObsidianOpenUri(const fs::path &vault_dir, const fs::path &file_path)
{
    const std::optional<ObsidianVault> vault = FindRegisteredObsidianVault(vault_dir);
    if (!vault.has_value())
    {
        return "";
    }

    const std::optional<fs::path> relative = RelativePathWithinVault(vault->path, file_path);
    if (!relative.has_value())
    {
        return "";
    }

    return BuildObsidianOpenUri(vault->name, WideToUtf8(relative->generic_wstring()));
}

int RunObsidianSelfTest()
{
    bool ok = true;
    auto fail = [&](const std::string &message)
    {
        std::cout << "[FAIL] obsidian self-test: " << message << "\n";
        ok = false;
    };

    const std::string registry =
        "{\"vaults\":{"
        "\"closed\":{\"path\":\"D:\\\\Notes\\\\Closed\",\"open\":false},"
        "\"open\":{\"path\":\"E:\\\\obsidian\\\\第一个库\",\"open\":true}"
        "}}";
    const std::vector<ObsidianVault> vaults = ParseObsidianVaultRegistry(registry);
    if (vaults.size() != 2 || vaults.front().id != "open" || vaults.front().name != "第一个库")
    {
        fail("vault registry parsing or sorting failed");
    }

    const std::string uri =
        BuildObsidianOpenUri(std::string("第一个库"), std::string("Inbox/Clipboard/测试 文件.md"));
    if (uri.find("obsidian://open?vault=") != 0 || uri.find("%E7%AC%AC") == std::string::npos ||
        uri.find("Inbox%2FClipboard%2F") == std::string::npos || uri.find("%20") == std::string::npos ||
        uri.find('\\') != std::string::npos)
    {
        fail("obsidian open URI encoding failed");
    }

    if (ok)
    {
        std::cout << "[PASS] obsidian vault discovery helpers\n";
    }
    return ok ? 0 : 1;
}
}
