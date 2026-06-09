#include "logger.h"

#include "util.h"
#include "win_util.h"

#include <windows.h>

#include <fstream>
#include <iostream>
#include <utility>

namespace fs = std::filesystem;

namespace ncw
{
Logger::Logger(fs::path log_path, bool mirror_console)
    : log_path_(std::move(log_path)), mirror_console_(mirror_console)
{
    fs::create_directories(log_path_.parent_path());
}

void Logger::Info(const std::string &message)
{
    Write("INFO", message);
}

void Logger::Warn(const std::string &message)
{
    Write("WARN", message);
}

void Logger::Error(const std::string &message)
{
    Write("ERROR", message);
}

void Logger::Write(const char *level, const std::string &message)
{
    const std::string line = LocalTimestamp() + " [" + level + "] " + message + "\n";
    std::lock_guard<std::mutex> lock(mutex_);
    std::ofstream output(log_path_, std::ios::binary | std::ios::app);
    if (output)
    {
        output.write(line.data(), static_cast<std::streamsize>(line.size()));
    }
    if (mirror_console_)
    {
        std::cout << line;
    }
    OutputDebugStringW(Utf8ToWide(line).c_str());
}
}
