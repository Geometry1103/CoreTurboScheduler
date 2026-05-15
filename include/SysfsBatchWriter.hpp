#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <cstdint>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/**
 * SysfsBatchWriter -- 批量 sysfs 写入器
 *
 * 目的：
 *   - 减少 scheduler latency 与 wakelock 抖动
 *   - 抑制对同一路径的重复写入（dirtyFlag）
 *   - 合并相邻周期的写入（mergeWrite）
 *   - 异步延迟应用（delayedApply）
 *
 * 用法：
 *   SysfsBatchWriter w;
 *   w.start();
 *   w.queue("/sys/.../scaling_max_freq", "2400000");
 *   w.flush();   // 立刻应用
 */
class SysfsBatchWriter {
public:
    SysfsBatchWriter() = default;
    ~SysfsBatchWriter() { stop(); }

    SysfsBatchWriter(const SysfsBatchWriter&) = delete;
    SysfsBatchWriter& operator=(const SysfsBatchWriter&) = delete;

    void start(int flushIntervalMs = 16) {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        flushIntervalMs_ = flushIntervalMs;
        worker_ = std::thread(&SysfsBatchWriter::workerLoop, this);
    }

    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false)) return;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        flushAll();
    }

    /// 加入写队列。若与上次写入值相同则丢弃（dirtyFlag 抑制）。
    void queue(const std::string& path, const std::string& value) {
        if (path.empty()) return;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = lastApplied_.find(path);
        if (it != lastApplied_.end() && it->second == value) {
            return;  // 相同值，跳过
        }
        pending_[path] = value;  // mergeWrite：相同 path 直接覆盖
        cv_.notify_one();
    }

    /// 立即同步写入，绕过批量队列（高优先级路径用，如 launch boost）。
    bool writeNow(const std::string& path, const std::string& value) {
        if (path.empty()) return false;
        bool ok = doWrite(path, value);
        if (ok) {
            std::lock_guard<std::mutex> lock(mu_);
            lastApplied_[path] = value;
            pending_.erase(path);
        }
        return ok;
    }

    /// 立即把所有 pending 写出。
    void flush() {
        flushAll();
    }

    /// 清空 dirtyFlag 缓存（强制下次重写）。
    void invalidate(const std::string& path) {
        std::lock_guard<std::mutex> lock(mu_);
        lastApplied_.erase(path);
    }

    void invalidateAll() {
        std::lock_guard<std::mutex> lock(mu_);
        lastApplied_.clear();
    }

    /// 统计：返回累计写入次数与跳过次数。
    void getStats(std::uint64_t& written, std::uint64_t& skipped) const {
        written = writeCount_.load();
        skipped = skipCount_.load();
    }

private:
    void workerLoop() {
        while (running_.load()) {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(flushIntervalMs_),
                         [this] { return !pending_.empty() || !running_.load(); });
            if (!running_.load() && pending_.empty()) break;

            auto batch = std::move(pending_);
            pending_.clear();
            lock.unlock();

            for (const auto& kv : batch) {
                if (doWrite(kv.first, kv.second)) {
                    std::lock_guard<std::mutex> lk(mu_);
                    lastApplied_[kv.first] = kv.second;
                }
            }
        }
    }

    void flushAll() {
        std::unordered_map<std::string, std::string> batch;
        {
            std::lock_guard<std::mutex> lock(mu_);
            batch = std::move(pending_);
            pending_.clear();
        }
        for (const auto& kv : batch) {
            if (doWrite(kv.first, kv.second)) {
                std::lock_guard<std::mutex> lock(mu_);
                lastApplied_[kv.first] = kv.second;
            }
        }
    }

    bool doWrite(const std::string& path, const std::string& value) {
        int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0) {
            chmod(path.c_str(), 0666);
            fd = open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NONBLOCK);
        }
        if (fd < 0) {
            skipCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ssize_t n = write(fd, value.data(), value.size());
        close(fd);
        if (n == static_cast<ssize_t>(value.size())) {
            writeCount_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        skipCount_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

private:
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<std::string, std::string> pending_;
    std::unordered_map<std::string, std::string> lastApplied_;  // dirtyFlag
    int flushIntervalMs_ = 16;
    std::atomic<std::uint64_t> writeCount_{0};
    std::atomic<std::uint64_t> skipCount_{0};
};
