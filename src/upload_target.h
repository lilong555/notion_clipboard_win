#pragma once

#include <functional>
#include <memory>
#include <string>

namespace ncw
{
struct AppConfig;
class Logger;
struct UploadJob;

class UploadTarget
{
public:
    virtual ~UploadTarget() = default;

    virtual std::string Name() const = 0;
    virtual void Validate() = 0;
    virtual void ProcessJob(UploadJob *job, const std::function<void()> &checkpoint) = 0;
};

std::unique_ptr<UploadTarget> CreateUploadTarget(const AppConfig &config, Logger *logger);
std::string BuildObsidianMarkdownDebugDocument(const std::string &content, const std::string &obsidian_tags);
int RunUploadTargetSelfTest();
}
