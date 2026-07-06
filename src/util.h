#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ncw
{
std::string Trim(const std::string &input);
std::string ToLowerAscii(std::string input);
std::string EscapeJson(const std::string &input);
bool ParseBool(std::string value);
int ParseIntOrDefault(const std::string &value, int fallback);
std::uint64_t ParseU64OrDefault(const std::string &value, std::uint64_t fallback);
std::uint64_t Fnv1a64(const std::string &text);
std::string Hex64(std::uint64_t value);
std::string SummarizeForLog(const std::string &text, std::size_t limit = 600);

std::string ReadWholeFile(const std::filesystem::path &path);
void AtomicWriteFile(const std::filesystem::path &path, const std::string &content);
void UpsertConfigValue(const std::filesystem::path &path, const std::string &key, const std::string &value);

std::uint64_t NowUnixMs();
std::string LocalTimestamp();
std::string IsoUtcTimestampFromUnixMs(std::uint64_t unix_ms);
std::filesystem::path UniqueTempDirectoryPath(const std::wstring &prefix);

std::filesystem::path ModuleDirectory();
std::wstring GetEnvWide(const wchar_t *name);
std::string GetEnvUtf8(const wchar_t *name);
std::filesystem::path DefaultStateDir();
std::filesystem::path DefaultConfigPath();
}
