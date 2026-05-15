#pragma once

#include <atomic>
#include <functional>
#include <thread>
#include <mutex>
#include <chrono>
#include <vector>
#include <string>
#include <unordered_map>

/**
 * WatchDog -- 线程看门狗保护机制
 *
 * 功能：
 *   1. 定时监控死锁检测：周期性检查被监控线程的心跳，超时未响应则判定为死锁/卡死
 *   2. 策略回滚机制：超时次数达到阈值后触发全局恢复回调，自动退回默认策略
 *   3. 策略失效快速恢复：每个被监控线程可绑定独立的超时回调，实现单线程级别的快速恢复
 *
 * 使用方式：
 *   - 被监控线程持有 std::atomic<bool> heartbeat，定期调用 WatchDog::feed(heartbeat)
 *   - 主线程创建 WatchDog，注册被监控线程并启动
 *   - 超时后自动触发 onTimeout / globalRecovery
 */

class WatchDog {
public:
    struct WatchedThread {
        std::string name;
        std::atomic<bool>* heartbeat;    ///< 线程心跳标志（指向外部原子变量）
        std::chrono::seconds timeout;     ///< 超时时间（无心跳的最大容忍时长）
        std::function<void()> onTimeout;  ///< 单线程超时回调（策略失效快速恢复）
        bool timedOut = false;            ///< 是否已超时（非原子，受 mutex_ 保护）
    };

private:
    std::vector<WatchedThread> watchedThreads_;
    std::atomic<bool> running_{false};
    std::thread watchThread_;
    mutable std::mutex mutex_;
    int checkIntervalMs_;                              ///< 检查间隔（毫秒）
    int maxTimeoutCount_;                              ///< 连续超时次数达到此值后触发恢复
    std::function<void()> globalRecoveryCallback_;     ///< 全局恢复回调（策略回滚）
    mutable std::unordered_map<std::string, int> timeoutCounts_;   ///< 各线程累计超时次数
    std::unordered_map<std::string,
        std::chrono::steady_clock::time_point> lastFeedTimes_;     ///< 各线程最后一次心跳时间

public:
    // ------------------------------------------------------------------
    //  构造 / 析构
    // ------------------------------------------------------------------

    /**
     * @param checkIntervalMs   心跳检查周期，默认 5000 ms
     * @param maxTimeoutCount   连续超时多少次后触发全局恢复，默认 3
     */
    WatchDog(int checkIntervalMs = 5000, int maxTimeoutCount = 3)
        : checkIntervalMs_(checkIntervalMs)
        , maxTimeoutCount_(maxTimeoutCount)
    {}

    ~WatchDog() {
        stop();
    }

    // 禁止拷贝和移动（持有 thread 和引用）
    WatchDog(const WatchDog&) = delete;
    WatchDog& operator=(const WatchDog&) = delete;
    WatchDog(WatchDog&&) = delete;
    WatchDog& operator=(WatchDog&&) = delete;

    // ------------------------------------------------------------------
    //  注册 / 配置
    // ------------------------------------------------------------------

    /**
     * 注册一个需要监控的线程。
     *
     * @param name        线程名称（用于日志和查询）
     * @param heartbeat   指向该线程持有的 std::atomic<bool>，线程通过 feed() 置 true
     * @param timeout     允许无心跳的最大时长
     * @param onTimeout   该线程超时后的回调（可做空）
     */
    void registerThread(const std::string& name,
                        std::atomic<bool>* heartbeat,
                        std::chrono::seconds timeout,
                        std::function<void()> onTimeout)
    {
        std::lock_guard lock(mutex_);

        // 如果同名线程已存在，先移除旧记录（按索引移除，避免 move atomic）
        for (auto it = watchedThreads_.begin(); it != watchedThreads_.end(); ++it) {
            if (it->name == name) {
                watchedThreads_.erase(it);
                break;
            }
        }

        WatchedThread wt;
        wt.name       = name;
        wt.heartbeat  = heartbeat;
        wt.timeout    = timeout;
        wt.onTimeout  = std::move(onTimeout);
        wt.timedOut   = false;
        watchedThreads_.push_back(std::move(wt));

        timeoutCounts_[name] = 0;
        lastFeedTimes_[name] = std::chrono::steady_clock::now();
    }

    /**
     * 设置全局恢复回调。
     * 当任意线程连续超时次数达到 maxTimeoutCount_ 时触发，
     * 用于执行策略回滚（如重置调度参数、切换到安全模式等）。
     */
    void setGlobalRecovery(std::function<void()> callback) {
        std::lock_guard lock(mutex_);
        globalRecoveryCallback_ = std::move(callback);
    }

    // ------------------------------------------------------------------
    //  启动 / 停止
    // ------------------------------------------------------------------

    /** 启动看门狗监控线程。重复调用无效。 */
    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel)) {
            return; // 已经在运行
        }
        watchThread_ = std::thread(&WatchDog::watchLoop, this);
    }

    /** 停止看门狗监控线程并等待其退出。重复调用无效。 */
    void stop() {
        bool expected = true;
        if (!running_.compare_exchange_strong(expected, false,
                                              std::memory_order_acq_rel)) {
            return; // 已经停止
        }
        if (watchThread_.joinable()) {
            watchThread_.join();
        }
    }

    // ------------------------------------------------------------------
    //  心跳喂狗
    // ------------------------------------------------------------------

    /**
     * 被监控线程定期调用此方法报告存活。
     * 将 heartbeat 置为 true，看门狗在下次检查时读取并重置。
     *
     * 用法（在被监控线程内部）：
     *   std::atomic<bool> heartbeat{false};
     *   // ... 注册到 WatchDog ...
     *   while (running) {
     *       doWork();
     *       WatchDog::feed(heartbeat);
     *   }
     */
    static void feed(std::atomic<bool>& heartbeat) {
        heartbeat.store(true, std::memory_order_release);
    }

    // ------------------------------------------------------------------
    //  状态查询
    // ------------------------------------------------------------------

    /** 查询指定线程是否健康（未超时） */
    bool isThreadHealthy(const std::string& name) const {
        std::lock_guard lock(mutex_);
        for (const auto& wt : watchedThreads_) {
            if (wt.name == name) {
                return !wt.timedOut;
            }
        }
        return false; // 未注册的线程视为不健康
    }

    /** 获取指定线程的累计超时次数 */
    int getTimeoutCount(const std::string& name) const {
        std::lock_guard lock(mutex_);
        auto it = timeoutCounts_.find(name);
        if (it != timeoutCounts_.end()) {
            return it->second;
        }
        return 0;
    }

    // ------------------------------------------------------------------
    //  内部实现
    // ------------------------------------------------------------------

private:
    /**
     * 看门狗主循环，运行在独立线程中。
     *
     * 每隔 checkIntervalMs_ 毫秒检查一次所有被监控线程：
     *   - 若 heartbeat 为 true → 线程存活，重置心跳和超时计数
     *   - 若 heartbeat 为 false 且距离上次喂狗超过 timeout → 累加超时计数
     *   - 超时计数 >= maxTimeoutCount_ → 标记 timedOut，触发 onTimeout 和 globalRecovery
     */
    void watchLoop() {
        while (running_.load(std::memory_order_acquire)) {
            // 使用 sleep_for + 条件检查，确保 stop() 能及时响应
            for (int i = 0; i < checkIntervalMs_ && running_.load(std::memory_order_acquire); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if (!running_.load(std::memory_order_acquire)) {
                break;
            }

            std::lock_guard lock(mutex_);
            auto now = std::chrono::steady_clock::now();
            bool needGlobalRecovery = false;

            for (auto& wt : watchedThreads_) {
                // ---- 检查心跳 ----
                bool alive = wt.heartbeat->load(std::memory_order_acquire);

                if (alive) {
                    // 线程存活：记录喂狗时间，重置心跳标志和超时计数
                    lastFeedTimes_[wt.name] = now;
                    wt.heartbeat->store(false, std::memory_order_release);
                    wt.timedOut = false;
                    timeoutCounts_[wt.name] = 0;
                } else {
                    // 线程未喂狗：检查是否超过该线程的 timeout 阈值
                    auto feedIt = lastFeedTimes_.find(wt.name);
                    if (feedIt != lastFeedTimes_.end()) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - feedIt->second);

                        if (elapsed >= wt.timeout) {
                            // 超时：累加计数
                            ++timeoutCounts_[wt.name];

                            if (timeoutCounts_[wt.name] >= maxTimeoutCount_) {
                                // 达到最大超时次数，标记为超时
                                bool wasAlreadyTimedOut = wt.timedOut;

                                // 首次进入超时状态时触发回调（避免重复触发）
                                if (!wasAlreadyTimedOut) {
                                    wt.timedOut = true;
                                    // 单线程级快速恢复
                                    if (wt.onTimeout) {
                                        wt.onTimeout();
                                    }
                                    needGlobalRecovery = true;
                                }
                            }
                        }
                    }
                }
            }

            // 全局恢复回调（策略回滚）
            if (needGlobalRecovery && globalRecoveryCallback_) {
                globalRecoveryCallback_();
            }
        }
    }
};
