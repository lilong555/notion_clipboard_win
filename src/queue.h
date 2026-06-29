#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ncw
{
class Logger;

class UploadFailure : public std::runtime_error
{
public:
    UploadFailure(std::string message, bool retryable, int retry_after_seconds);

    bool retryable() const;
    int retry_after_seconds() const;

private:
    bool retryable_ = false;
    int retry_after_seconds_ = 0;
};

struct UploadJob
{
    std::string id;
    std::uint64_t created_at_ms = 0;
    std::uint64_t not_before_ms = 0;
    int attempts = 0;
    std::string target;
    std::string hash;
    std::string title;
    std::string content;
    std::string remote_id;
    std::string remote_url;
    std::size_t remote_progress = 0;
    std::string last_error;
};

class PersistentQueue
{
public:
    PersistentQueue(std::filesystem::path state_dir, int max_retry_attempts);

    void Enqueue(const UploadJob &job);
    void Update(const std::filesystem::path &path, const UploadJob &job);
    void MarkSuccess(const std::filesystem::path &path);
    void MarkFailure(const std::filesystem::path &path, UploadJob job, const std::string &error, bool retryable,
                     int retry_after_seconds, Logger *logger);
    std::optional<std::pair<UploadJob, std::filesystem::path>> NextDueJob(std::uint64_t now_ms,
                                                                          std::uint64_t *next_due_ms,
                                                                          Logger *logger);

private:
    std::filesystem::path JobPath(const std::string &id) const;
    std::vector<std::filesystem::path> ListJobFiles() const;
    static std::uint64_t ComputeBackoffMs(int attempts, int retry_after_seconds);

    std::filesystem::path queue_dir_;
    std::filesystem::path failed_dir_;
    int max_retry_attempts_ = 12;
    std::mutex mutex_;
};
}
