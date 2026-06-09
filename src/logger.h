#pragma once

#include <filesystem>
#include <mutex>
#include <string>

namespace ncw
{
class Logger
{
public:
    Logger(std::filesystem::path log_path, bool mirror_console);

    void Info(const std::string &message);
    void Warn(const std::string &message);
    void Error(const std::string &message);

private:
    void Write(const char *level, const std::string &message);

    std::filesystem::path log_path_;
    bool mirror_console_ = false;
    std::mutex mutex_;
};
}
