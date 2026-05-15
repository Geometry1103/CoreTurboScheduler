#pragma once

#include "Utils.hpp"
#include "LibUtils.hpp"
#include <condition_variable>

class Logger {
private:
    static constexpr const char* logpath = "/sdcard/Android/CTS/log.txt";
    static constexpr long LOG_MAX_SIZE = 1024 * 10; // 10KB auto-clear threshold

    // ---- Ring Buffer ----
    static constexpr int RING_SIZE = 1024;       // number of log entries
    static constexpr int ENTRY_SIZE = 256;        // max bytes per entry

    struct LogEntry {
        char data[ENTRY_SIZE];
        int  len;                                 // actual length (<= ENTRY_SIZE)
    };

    LogEntry ringBuf_[RING_SIZE];
    int      head_ = 0;                          // next write position
    int      tail_ = 0;                          // next read position
    int      count_ = 0;                         // number of pending entries

    // ---- Synchronisation ----
    mutex              ringMutex_;
    std::condition_variable cv_;
    atomic<bool>       running_{true};

    // ---- Background thread ----
    thread bgThread_;

    // ---- Log level ----
    LOG_LEVEL logLevel_ = LOG_LEVEL::INFO;

    // ---- Level string mapping (unchanged) ----
    inline static const unordered_map<LOG_LEVEL, const char*> levelStrings = {
        {LOG_LEVEL::DEBUG, "调试 ->"},
        {LOG_LEVEL::INFO,  "信息 ->"},
        {LOG_LEVEL::WARN,  "警告 ->"},
        {LOG_LEVEL::ERROR, "错误 ->"},
    };

    // ---- Private helpers ----

    // Background worker: waits for entries and writes them to file
    void bgWorker() {
        while (running_.load()) {
            LogEntry entry;
            bool gotEntry = false;

            {
                std::unique_lock<std::mutex> lock(ringMutex_);
                cv_.wait(lock, [this]() {
                    return !running_.load() || count_ > 0;
                });

                // Drain all available entries
                if (count_ > 0) {
                    entry = ringBuf_[tail_];
                    tail_ = (tail_ + 1) % RING_SIZE;
                    --count_;
                    gotEntry = true;
                }
            }

            if (gotEntry) {
                WriteFile(entry.data, entry.len);
            }
        }

        // Flush remaining entries after stop signal
        flushRemaining();
    }

    // Flush everything left in the ring buffer (called from bgWorker after stop)
    void flushRemaining() {
        while (true) {
            LogEntry entry;
            bool gotEntry = false;

            {
                lock_guard<mutex> lock(ringMutex_);
                if (count_ > 0) {
                    entry = ringBuf_[tail_];
                    tail_ = (tail_ + 1) % RING_SIZE;
                    --count_;
                    gotEntry = true;
                }
            }

            if (!gotEntry) break;
            WriteFile(entry.data, entry.len);
        }
    }

    // Push a formatted log entry into the ring buffer (non-blocking)
    void pushEntry(const char* data, int len) {
        lock_guard<mutex> lock(ringMutex_);

        if (count_ >= RING_SIZE) {
            // Buffer full: overwrite oldest entry (drop it)
            tail_ = (tail_ + 1) % RING_SIZE;
            --count_;
        }

        int copyLen = (len < ENTRY_SIZE) ? len : (ENTRY_SIZE - 1);
        memcpy(ringBuf_[head_].data, data, copyLen);
        ringBuf_[head_].data[copyLen] = '\0';
        ringBuf_[head_].len = copyLen;
        head_ = (head_ + 1) % RING_SIZE;
        ++count_;

        cv_.notify_one();
    }

    void WriteFile(const char* content, const int len) noexcept {
        int fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0666);

        if (fd >= 0) {
            // Auto-clear log when exceeds 10KB
            struct stat st;
            if (fstat(fd, &st) == 0 && st.st_size >= LOG_MAX_SIZE) {
                if (ftruncate(fd, 0) == 0) {
                    // File truncated, write will start at offset 0
                }
            }
            write(fd, content, len);
            close(fd);
        }
    }

    int getCurrentTimeStr(char* buf, size_t size) {
        time_t now = time(nullptr);
        struct tm* local_time = localtime(&now);
        return strftime(buf, size, "%Y-%m-%d %H:%M:%S ", local_time);
    }

    // ---- Core Log (no-args version) ----
    void Log(LOG_LEVEL level, const char* message) {
        if (level < logLevel_) return;

        char buf[ENTRY_SIZE];
        int len = getCurrentTimeStr(buf, sizeof(buf));
        len += FastSnprintf(buf + len, sizeof(buf) - len, "%s %s\n", levelStrings.at(level), message);

        #if DEBUG_DURATION
            printf("%s\n", buf);
        #endif

        pushEntry(buf, len);
    }

    // ---- Core Log (variadic template version) ----
    template<typename... Args>
    void Log(LOG_LEVEL level, const char* message, Args&&... args) {
        if (level < logLevel_) return;

        char buf[ENTRY_SIZE];
        const int prefixLen = getCurrentTimeStr(buf, sizeof(buf));

        int len = prefixLen + FastSnprintf(buf + prefixLen, sizeof(buf) - prefixLen, "%s ", levelStrings.at(level));

        len += snprintf(buf + len, sizeof(buf) - len, message, std::forward<Args>(args)...);

        if (len <= prefixLen || (size_t)len >= sizeof(buf) - 1) {
            buf[prefixLen] = '\0';
            fprintf(stderr, "日志异常: len[%d] buf[%s]\n", len, buf);
            return;
        }

        buf[len++] = '\n';

        #if DEBUG_DURATION
            printf("%s\n", buf);
        #endif

        pushEntry(buf, len);
    }

public:
    Logger() {
        bgThread_ = thread(&Logger::bgWorker, this);
    }

    ~Logger() {
        running_.store(false);
        cv_.notify_one();
        if (bgThread_.joinable()) {
            bgThread_.join();
        }
    }

    // Non-copyable, non-movable
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // ---- Public log interface (unchanged) ----

    void Debug(const char* message) {
        Log(LOG_LEVEL::DEBUG, message);
    }

    void Info(const char* message) {
        Log(LOG_LEVEL::INFO, message);
    }

    void Warn(const char* message) {
        Log(LOG_LEVEL::WARN, message);
    }

    void Error(const char* message) {
        Log(LOG_LEVEL::ERROR, message);
    }

    template<typename... Args>
    void Debug(const char* message, Args&&... args) {
        Log(LOG_LEVEL::DEBUG, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Info(const char* message, Args&&... args) {
        Log(LOG_LEVEL::INFO, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Warn(const char* message, Args&&... args) {
        Log(LOG_LEVEL::WARN, message, std::forward<Args>(args)...);
    }

    template<typename... Args>
    void Error(const char* message, Args&&... args) {
        Log(LOG_LEVEL::ERROR, message, std::forward<Args>(args)...);
    }

    void setLogLevel(string_t& level) {
        if (level == "DEBUG")
            logLevel_ = LOG_LEVEL::DEBUG;
        else if (level == "INFO")
            logLevel_ = LOG_LEVEL::INFO;
        else if (level == "WARN")
            logLevel_ = LOG_LEVEL::WARN;
        else if (level == "ERROR")
            logLevel_ = LOG_LEVEL::ERROR;
    }

    void clear_log() {
        // Create parent directory if needed
        mkdir("/sdcard/Android/CTS", 0755);
        mkdir("/sdcard/Android", 0755);
        auto temp = fopen(logpath, "wb");
        if (!temp) {
            fprintf(stderr, "ERROR:清理日志文件失败");
            return;
        }
        fclose(temp);
    }
};
