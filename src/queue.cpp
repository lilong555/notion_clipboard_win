#include "queue.h"

#include "json.h"
#include "logger.h"
#include "util.h"
#include "win_util.h"

#include <algorithm>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace ncw
{
UploadFailure::UploadFailure(std::string message, bool retryable, int retry_after_seconds)
    : std::runtime_error(std::move(message)), retryable_(retryable), retry_after_seconds_(retry_after_seconds)
{
}

bool UploadFailure::retryable() const
{
    return retryable_;
}

int UploadFailure::retry_after_seconds() const
{
    return retry_after_seconds_;
}

namespace
{
std::string JobToJson(const UploadJob &job)
{
    std::ostringstream oss;
    oss << "{\n"
        << "  \"id\":\"" << EscapeJson(job.id) << "\",\n"
        << "  \"created_at_ms\":" << job.created_at_ms << ",\n"
        << "  \"not_before_ms\":" << job.not_before_ms << ",\n"
        << "  \"attempts\":" << job.attempts << ",\n"
        << "  \"target\":\"" << EscapeJson(job.target) << "\",\n"
        << "  \"hash\":\"" << EscapeJson(job.hash) << "\",\n"
        << "  \"title\":\"" << EscapeJson(job.title) << "\",\n"
        << "  \"content\":\"" << EscapeJson(job.content) << "\",\n"
        << "  \"remote_id\":\"" << EscapeJson(job.remote_id) << "\",\n"
        << "  \"remote_url\":\"" << EscapeJson(job.remote_url) << "\",\n"
        << "  \"remote_progress\":" << static_cast<unsigned long long>(job.remote_progress) << ",\n"
        << "  \"last_error\":\"" << EscapeJson(job.last_error) << "\"\n"
        << "}\n";
    return oss.str();
}

std::uint64_t JsonNumberAsU64(const JsonValue *value, std::uint64_t fallback)
{
    if (value == nullptr)
    {
        return fallback;
    }
    if (value->is_number())
    {
        return static_cast<std::uint64_t>(value->as_number());
    }
    if (value->is_string())
    {
        return ParseU64OrDefault(value->as_string(), fallback);
    }
    return fallback;
}

std::size_t JsonNumberAsSize(const JsonValue *value, std::size_t fallback)
{
    return static_cast<std::size_t>(JsonNumberAsU64(value, static_cast<std::uint64_t>(fallback)));
}

int JsonNumberAsInt(const JsonValue *value, int fallback)
{
    return static_cast<int>(JsonNumberAsU64(value, static_cast<std::uint64_t>(fallback)));
}

std::string JsonStringOrEmpty(const JsonValue *value)
{
    if (value == nullptr || !value->is_string())
    {
        return "";
    }
    return value->as_string();
}

UploadJob JobFromJson(const std::string &text)
{
    const JsonValue json = ParseJson(text);
    if (!json.is_object())
    {
        throw std::runtime_error("任务文件不是 JSON object");
    }
    UploadJob job;
    job.id = JsonStringOrEmpty(json.find("id"));
    job.created_at_ms = JsonNumberAsU64(json.find("created_at_ms"), 0);
    job.not_before_ms = JsonNumberAsU64(json.find("not_before_ms"), 0);
    job.attempts = JsonNumberAsInt(json.find("attempts"), 0);
    job.target = JsonStringOrEmpty(json.find("target"));
    job.hash = JsonStringOrEmpty(json.find("hash"));
    job.title = JsonStringOrEmpty(json.find("title"));
    job.content = JsonStringOrEmpty(json.find("content"));
    job.remote_id = JsonStringOrEmpty(json.find("remote_id"));
    if (job.remote_id.empty())
    {
        job.remote_id = JsonStringOrEmpty(json.find("page_id"));
    }
    job.remote_url = JsonStringOrEmpty(json.find("remote_url"));
    if (job.remote_url.empty())
    {
        job.remote_url = JsonStringOrEmpty(json.find("page_url"));
    }
    job.remote_progress = JsonNumberAsSize(json.find("remote_progress"),
                                          JsonNumberAsSize(json.find("appended_block_count"), 0));
    job.last_error = JsonStringOrEmpty(json.find("last_error"));
    if (job.id.empty() || job.content.empty())
    {
        throw std::runtime_error("任务文件缺少 id 或 content");
    }
    return job;
}
}

PersistentQueue::PersistentQueue(fs::path state_dir, int max_retry_attempts)
    : queue_dir_(std::move(state_dir) / L"queue"),
      failed_dir_(queue_dir_.parent_path() / L"failed"),
      max_retry_attempts_(max_retry_attempts)
{
    fs::create_directories(queue_dir_);
    fs::create_directories(failed_dir_);
}

void PersistentQueue::Enqueue(const UploadJob &job)
{
    std::lock_guard<std::mutex> lock(mutex_);
    AtomicWriteFile(JobPath(job.id), JobToJson(job));
}

void PersistentQueue::Update(const fs::path &path, const UploadJob &job)
{
    std::lock_guard<std::mutex> lock(mutex_);
    AtomicWriteFile(path, JobToJson(job));
}

void PersistentQueue::MarkSuccess(const fs::path &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ignored;
    fs::remove(path, ignored);
}

void PersistentQueue::MarkFailure(const fs::path &path, UploadJob job, const std::string &error, bool retryable,
                                  int retry_after_seconds, Logger *logger)
{
    std::lock_guard<std::mutex> lock(mutex_);
    job.last_error = error;
    job.attempts += 1;

    if (!retryable || job.attempts > max_retry_attempts_)
    {
        const fs::path failed_path = failed_dir_ / path.filename();
        AtomicWriteFile(failed_path, JobToJson(job));
        std::error_code ignored;
        fs::remove(path, ignored);
        if (logger != nullptr)
        {
            logger->Error("任务移入 failed: " + job.id + "，原因: " + SummarizeForLog(error));
        }
        return;
    }

    job.not_before_ms = NowUnixMs() + ComputeBackoffMs(job.attempts, retry_after_seconds);
    AtomicWriteFile(path, JobToJson(job));
    if (logger != nullptr)
    {
        logger->Warn("任务稍后重试: " + job.id + "，attempt=" + std::to_string(job.attempts));
    }
}

std::optional<std::pair<UploadJob, fs::path>> PersistentQueue::NextDueJob(std::uint64_t now_ms,
                                                                          std::uint64_t *next_due_ms, Logger *logger)
{
    std::lock_guard<std::mutex> lock(mutex_);
    *next_due_ms = 0;
    for (const fs::path &path : ListJobFiles())
    {
        UploadJob job;
        try
        {
            job = JobFromJson(ReadWholeFile(path));
        }
        catch (const std::exception &ex)
        {
            const fs::path failed_path = failed_dir_ / path.filename();
            std::error_code ignored;
            fs::rename(path, failed_path, ignored);
            if (logger != nullptr)
            {
                logger->Error("任务文件损坏，已移动到 failed: " + WideToUtf8(path.filename().wstring()) +
                              "，原因: " + ex.what());
            }
            continue;
        }

        if (job.not_before_ms == 0 || job.not_before_ms <= now_ms)
        {
            return std::make_pair(job, path);
        }
        if (*next_due_ms == 0 || job.not_before_ms < *next_due_ms)
        {
            *next_due_ms = job.not_before_ms;
        }
    }
    return std::nullopt;
}

fs::path PersistentQueue::JobPath(const std::string &id) const
{
    return queue_dir_ / (Utf8ToWide(id) + L".job");
}

std::vector<fs::path> PersistentQueue::ListJobFiles() const
{
    std::vector<fs::path> files;
    if (!fs::exists(queue_dir_))
    {
        return files;
    }
    for (const fs::directory_entry &entry : fs::directory_iterator(queue_dir_))
    {
        if (entry.is_regular_file() && entry.path().extension() == L".job")
        {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::uint64_t PersistentQueue::ComputeBackoffMs(int attempts, int retry_after_seconds)
{
    if (retry_after_seconds > 0)
    {
        return static_cast<std::uint64_t>(retry_after_seconds) * 1000ull;
    }

    const int exponent = std::min(attempts, 10);
    const std::uint64_t base = 2000ull;
    const std::uint64_t max_delay = 15ull * 60ull * 1000ull;
    const std::uint64_t delay = std::min(max_delay, base << exponent);
    const std::uint64_t jitter = Fnv1a64(std::to_string(NowUnixMs())) % 1000ull;
    return delay + jitter;
}
}
