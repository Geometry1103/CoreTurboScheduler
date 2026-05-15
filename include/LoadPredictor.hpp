#pragma once

#include <atomic>
#include <cmath>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <numeric>

class LoadPredictor {
public:
    // 负载级别枚举
    enum class LoadLevel {
        IDLE,       // CPU < 20%
        NORMAL,     // 20-70%
        HEAVY,      // > 70%
        CRITICAL    // > 95%
    };

    struct PredictResult {
        LoadLevel level;
        double emaValue;        // 当前EMA值
        double predictedLoad;   // 预测负载
        bool levelChanged;      // 负载级别是否变化
    };

private:
    static constexpr double DEFAULT_ALPHA = 0.2;     // EMA平滑系数（更平滑，适合100ms高频采样）
    static constexpr double IDLE_THRESHOLD = 20.0;
    static constexpr double NORMAL_THRESHOLD = 70.0;
    static constexpr double CRITICAL_THRESHOLD = 95.0;
    static constexpr int BUFFER_SIZE = 10;            // 阈值缓冲计数（100ms x 10 = 1秒确认时间）
    static constexpr int HISTORY_SIZE = 20;           // 历史采样数（更多历史点用于趋势预测）

    // --- 可变状态（受 mutex 保护） ---
    double alpha_;                   // EMA平滑系数
    double emaValue_;                // 当前EMA值
    double history_[HISTORY_SIZE];   // 历史采样值
    int historyIndex_;               // 历史索引（下一个写入位置）
    int historyCount_;               // 已采集数量
    LoadLevel currentLevel_;         // 当前负载级别
    int levelBufferCount_;           // 级别变化缓冲计数
    LoadLevel pendingLevel_;         // 待切换的级别

    // --- 动态自适应阈值 ---
    double idleThreshold_;           // 可调整的空闲阈值
    double normalThreshold_;         // 可调整的正常阈值
    double criticalThreshold_;       // 可调整的临界阈值

    // --- 自适应统计 ---
    double adaptiveSum_;             // 自适应窗口内的负载总和
    int adaptiveCount_;              // 自适应窗口内的采样计数
    static constexpr int ADAPTIVE_WINDOW = 50;  // 自适应窗口大小

    // --- 预测阈值（参考 uperf v3） ---
    double predictThd_;              // 负载增量预测阈值（默认0.3）
    double lastMaxLoad_;             // 上一次最大负载值，用于计算负载增量

    // --- 非对称 EMA（参考 Linux 内核 UTIL_EST_FASTUP） ---
    double alphaUp_ = 1.0;           // 上升系数（1.0 = 即时）
    double alphaDown_ = 0.15;        // 下降系数（0.15 = 缓慢衰减）

    // --- CUSUM 变点检测 ---
    double cusumHigh_ = 0.0;         // 向上累积和
    double cusumLow_ = 0.0;          // 向下累积和
    double cusumK_ = 5.0;            // 允许偏差（阈值的百分比）
    double cusumH_ = 25.0;           // 决策阈值（触发值）
    bool cusumTriggered_ = false;    // 是否触发过突增检测

    mutable std::mutex mutex_;       // 保护所有可变状态

public:
    LoadPredictor(double alpha = DEFAULT_ALPHA)
        : alpha_(alpha)
        , emaValue_(0.0)
        , historyIndex_(0)
        , historyCount_(0)
        , currentLevel_(LoadLevel::NORMAL)
        , levelBufferCount_(0)
        , pendingLevel_(LoadLevel::NORMAL)
        , idleThreshold_(IDLE_THRESHOLD)
        , normalThreshold_(NORMAL_THRESHOLD)
        , criticalThreshold_(CRITICAL_THRESHOLD)
        , adaptiveSum_(0.0)
        , adaptiveCount_(0)
        , predictThd_(0.3)
        , lastMaxLoad_(0.0)
    {
        // 初始化历史数组为零
        for (int i = 0; i < HISTORY_SIZE; ++i) {
            history_[i] = 0.0;
        }
    }

    // 输入新的负载数据（调用方负责计算 efficiency 加权后的负载值）
    // 返回预测结果
    PredictResult update(double currentLoad) {
        std::lock_guard<std::mutex> lock(mutex_);

        PredictResult result{};

        // 1. 将当前负载值钳制到 [0, 100] 范围
        currentLoad = std::clamp(currentLoad, 0.0, 100.0);

        // 2. predictThd 逻辑（参考 uperf v3）：
        //    如果 CPU 集群最大负载增加量 > predictThd * 100，则使用线性预测值替代当前值
        double loadDelta = currentLoad - lastMaxLoad_;
        if (loadDelta > predictThd_ * 100.0 && historyCount_ >= 2) {
            // 负载突增超过阈值，使用线性预测值
            double predicted = linearPredict();
            if (predicted > currentLoad) {
                currentLoad = predicted;
            }
        }
        // 更新 lastMaxLoad_
        if (currentLoad > lastMaxLoad_) {
            lastMaxLoad_ = currentLoad;
        }

        // 3. 动态 AI EMA（融合 UTIL_EST_FASTUP / WALT 风格）
        if (historyCount_ == 0) {
            emaValue_ = currentLoad;
        } else {
            const double delta = std::abs(currentLoad - emaValue_);
            const double dynamicAlpha = std::clamp(
                alpha_ + (delta / 100.0) * 0.35,
                0.05,
                0.95
            );

            const double useAlpha =
                currentLoad > emaValue_
                    ? std::max(alphaUp_, dynamicAlpha)
                    : std::min(alphaDown_, dynamicAlpha);

            emaValue_ = useAlpha * currentLoad +
                        (1.0 - useAlpha) * emaValue_;
        }

        // 4. CUSUM 变点检测
        double target = emaValue_; // 基准 = 当前 EMA
        double deviation = cusumK_; // 允许偏差

        cusumHigh_ = std::max(0.0, cusumHigh_ + (currentLoad - target - deviation));
        cusumLow_ = std::min(0.0, cusumLow_ + (currentLoad - target + deviation));

        if (cusumHigh_ > cusumH_) {
            // 持续向上偏移 detected -> 直接升级到 HEAVY
            cusumTriggered_ = true;
            cusumHigh_ = 0.0;
        }
        if (cusumLow_ < -cusumH_) {
            // 持续向下偏移 detected -> 直接降级到 IDLE
            cusumLow_ = 0.0;
        }

        // 5. 记录历史采样
        history_[historyIndex_] = currentLoad;
        historyIndex_ = (historyIndex_ + 1) % HISTORY_SIZE;
        if (historyCount_ < HISTORY_SIZE) {
            ++historyCount_;
        }

        // 6. 更新自适应统计
        adaptiveSum_ += currentLoad;
        ++adaptiveCount_;
        if (adaptiveCount_ >= ADAPTIVE_WINDOW) {
            adaptThresholds();
            adaptiveSum_ = 0.0;
            adaptiveCount_ = 0;
        }

        // 7. 基于EMA值判断负载级别
        LoadLevel newLevel = classifyLoad(emaValue_);

        // 8. 阈值缓冲机制：防止因瞬时抖动导致频繁切换
        bool changed = false;
        if (newLevel != currentLevel_) {
            if (newLevel != pendingLevel_) {
                // 检测到新的待切换级别，重置缓冲计数
                pendingLevel_ = newLevel;
                levelBufferCount_ = 1;
            } else {
                // 连续检测到同一待切换级别，递增缓冲计数
                ++levelBufferCount_;
            }

            if (levelBufferCount_ >= BUFFER_SIZE) {
                // 缓冲计数达到阈值，确认切换
                currentLevel_ = pendingLevel_;
                levelBufferCount_ = 0;
                changed = true;
                // CUSUM 触发后在一次级别切换后重置
                cusumTriggered_ = false;
            }
        } else {
            // 级别未变化，重置缓冲
            pendingLevel_ = currentLevel_;
            levelBufferCount_ = 0;
        }

        // 9. 线性趋势预测
        double predicted = linearPredict();

        // 10. 填充返回结果
        result.level = currentLevel_;
        result.emaValue = emaValue_;
        result.predictedLoad = predicted;
        result.levelChanged = changed;

        return result;
    }


    enum class WorkloadType {
        IDLE, UI, VIDEO, GAME, HEAVY_CPU, HEAVY_GPU
    };

    WorkloadType detectWorkload(double cpuLoad, double gpuLoad) const {
        if (cpuLoad < 15.0 && gpuLoad < 15.0) return WorkloadType::IDLE;
        if (gpuLoad > 80.0 && cpuLoad < 50.0) return WorkloadType::HEAVY_GPU;
        if (cpuLoad > 80.0 && gpuLoad < 40.0) return WorkloadType::HEAVY_CPU;
        if (gpuLoad > 60.0 && cpuLoad > 60.0) return WorkloadType::GAME;
        if (gpuLoad > 35.0) return WorkloadType::VIDEO;
        return WorkloadType::UI;
    }

    // 获取当前EMA值
    double getEma() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return emaValue_;
    }

    // 获取当前负载级别
    LoadLevel getLevel() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentLevel_;
    }

    // 重置预测器
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        emaValue_ = 0.0;
        historyIndex_ = 0;
        historyCount_ = 0;
        currentLevel_ = LoadLevel::NORMAL;
        levelBufferCount_ = 0;
        pendingLevel_ = LoadLevel::NORMAL;
        adaptiveSum_ = 0.0;
        adaptiveCount_ = 0;
        lastMaxLoad_ = 0.0;
        cusumHigh_ = 0.0;
        cusumLow_ = 0.0;
        cusumTriggered_ = false;
        for (int i = 0; i < HISTORY_SIZE; ++i) {
            history_[i] = 0.0;
        }
    }

    // 设置EMA平滑系数
    void setAlpha(double alpha) {
        std::lock_guard<std::mutex> lock(mutex_);
        // alpha 必须在 (0, 1] 范围内
        alpha_ = std::clamp(alpha, 0.01, 1.0);
    }

    // 设置预测阈值（参考 uperf v3）
    void setPredictThd(double thd) {
        std::lock_guard<std::mutex> lock(mutex_);
        predictThd_ = std::clamp(thd, 0.0, 1.0);
    }

    // 设置非对称 EMA 系数（参考 Linux 内核 UTIL_EST_FASTUP）
    void setAsymmetricAlpha(double up, double down) {
        std::lock_guard<std::mutex> lock(mutex_);
        alphaUp_ = std::clamp(up, 0.01, 1.0);
        alphaDown_ = std::clamp(down, 0.01, 1.0);
    }

    // 设置 CUSUM 变点检测参数
    void setCusumParams(double k, double h) {
        std::lock_guard<std::mutex> lock(mutex_);
        cusumK_ = std::clamp(k, 0.0, 50.0);
        cusumH_ = std::clamp(h, 0.0, 200.0);
    }

    // 简单线性趋势预测
    double predictTrend() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return linearPredict();
    }

private:
    // 内部：判断负载级别（基于动态自适应阈值 + CUSUM 结果）
    LoadLevel classifyLoad(double value) const {
        // CUSUM 触发突增检测且当前级别低于 HEAVY，直接返回 HEAVY
        if (cusumTriggered_ && value < normalThreshold_) {
            return LoadLevel::HEAVY;
        }

        if (value < idleThreshold_) {
            return LoadLevel::IDLE;
        } else if (value < normalThreshold_) {
            return LoadLevel::NORMAL;
        } else if (value < criticalThreshold_) {
            return LoadLevel::HEAVY;
        } else {
            return LoadLevel::CRITICAL;
        }
    }

    // 内部：线性回归预测
    //    使用最近 historyCount_ 个历史采样点做简单线性回归，
    //    预测下一个时间步的负载值。
    double linearPredict() const {
        if (historyCount_ < 2) {
            // 数据不足，返回当前EMA值
            return emaValue_;
        }

        int n = historyCount_;

        // 计算均值
        double sumX = 0.0;
        double sumY = 0.0;
        for (int i = 0; i < n; ++i) {
            sumX += static_cast<double>(i);
            sumY += history_[i];
        }
        double meanX = sumX / static_cast<double>(n);
        double meanY = sumY / static_cast<double>(n);

        // 计算斜率 (b) 和截距 (a): y = a + b*x
        double sumXY = 0.0;
        double sumXX = 0.0;
        for (int i = 0; i < n; ++i) {
            double dx = static_cast<double>(i) - meanX;
            double dy = history_[i] - meanY;
            sumXY += dx * dy;
            sumXX += dx * dx;
        }

        if (sumXX < 1e-12) {
            // 所有 x 值相同（不应发生），返回均值
            return meanY;
        }

        double slope = sumXY / sumXX;

        // 预测下一个时间步 (x = n) 的值
        double predicted = meanY + slope * (static_cast<double>(n) - meanX);

        // 将预测值钳制到 [0, 100] 范围
        return std::clamp(predicted, 0.0, 100.0);
    }

    // 内部：动态阈值自适应
    //    根据最近 ADAPTIVE_WINDOW 个采样的统计特征微调阈值。
    //    策略：如果平均负载偏高，适当上调阈值以避免频繁进入高级别；
    //          如果平均负载偏低，适当下调阈值以更早进入低级别省电。
    void adaptThresholds() {
        if (adaptiveCount_ == 0) {
            return;
        }

        double avgLoad = adaptiveSum_ / static_cast<double>(adaptiveCount_);

        // 计算标准差，评估负载波动程度
        // 使用在线算法近似：这里简化处理，仅用均值做自适应
        double deviation = std::abs(avgLoad - 50.0) / 50.0;  // 0~1，偏离中心的程度

        // 自适应调整幅度：偏离越大，调整越明显，但幅度受限
        constexpr double MAX_ADJUST = 5.0;   // 最大调整幅度（百分比）
        constexpr double MIN_ADJUST = 0.0;
        double adjustAmount = std::clamp(deviation * 3.0, MIN_ADJUST, MAX_ADJUST);

        if (avgLoad > 60.0) {
            // 长期高负载：上调阈值，避免过于敏感
            idleThreshold_ = std::clamp(
                IDLE_THRESHOLD + adjustAmount * 0.5,
                10.0, 40.0
            );
            normalThreshold_ = std::clamp(
                NORMAL_THRESHOLD + adjustAmount,
                50.0, 85.0
            );
            criticalThreshold_ = std::clamp(
                CRITICAL_THRESHOLD + adjustAmount * 0.3,
                90.0, 99.0
            );
        } else if (avgLoad < 30.0) {
            // 长期低负载：下调阈值，更早进入省电模式
            idleThreshold_ = std::clamp(
                IDLE_THRESHOLD - adjustAmount * 0.3,
                10.0, 40.0
            );
            normalThreshold_ = std::clamp(
                NORMAL_THRESHOLD - adjustAmount * 0.5,
                50.0, 85.0
            );
            criticalThreshold_ = std::clamp(
                CRITICAL_THRESHOLD - adjustAmount * 0.2,
                90.0, 99.0
            );
        } else {
            // 负载适中：逐步回归默认阈值
            constexpr double DECAY = 0.3;
            idleThreshold_ += (IDLE_THRESHOLD - idleThreshold_) * DECAY;
            normalThreshold_ += (NORMAL_THRESHOLD - normalThreshold_) * DECAY;
            criticalThreshold_ += (CRITICAL_THRESHOLD - criticalThreshold_) * DECAY;
        }
    }
};
