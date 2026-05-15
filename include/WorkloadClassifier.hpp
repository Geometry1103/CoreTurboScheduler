#pragma once

#include <chrono>
#include <atomic>
#include <mutex>
#include <cstring>

/**
 * WorkloadClassifier -- 场景识别（AI 调度核心）
 *
 * 输入：
 *   - cpuLoad   (0~100)        系统加权 CPU 负载
 *   - gpuLoad   (0~100)        GPU busy 百分比
 *   - frameJankPct (0~100)     最近窗口内的卡顿比例
 *   - ioBusyPct (0~100, 可选)  IO 繁忙度
 *
 * 输出：
 *   - WorkloadType: IDLE / UI / VIDEO / GAME / HEAVY_CPU / HEAVY_GPU / CAMERA / INSTALL
 *   - 切换带滞回与置信度，避免抖动
 */
class WorkloadClassifier {
public:
    enum class WorkloadType {
        IDLE,
        UI,
        VIDEO,
        GAME,
        HEAVY_CPU,
        HEAVY_GPU,
        CAMERA,
        INSTALL
    };

    struct Signal {
        int cpuLoad = 0;
        int gpuLoad = 0;
        int frameJankPct = 0;
        int ioBusyPct = 0;
        bool cameraActive = false;
        bool installActive = false;
    };

    WorkloadClassifier() = default;

    WorkloadType classify(const Signal& s) {
        WorkloadType raw = rawClassify(s);

        std::lock_guard<std::mutex> lock(mu_);
        if (raw == pending_) {
            ++pendingCount_;
        } else {
            pending_ = raw;
            pendingCount_ = 1;
        }

        // 切换到高优先级类型时降低门槛（更快响应）
        const int needed = highPriority(raw) ? 1 : hysteresisCount_;
        if (pendingCount_ >= needed && raw != current_) {
            current_ = raw;
            lastSwitchAt_ = std::chrono::steady_clock::now();
        }
        return current_;
    }

    WorkloadType current() const {
        std::lock_guard<std::mutex> lock(mu_);
        return current_;
    }

    static const char* name(WorkloadType t) {
        switch (t) {
            case WorkloadType::IDLE:      return "IDLE";
            case WorkloadType::UI:        return "UI";
            case WorkloadType::VIDEO:     return "VIDEO";
            case WorkloadType::GAME:      return "GAME";
            case WorkloadType::HEAVY_CPU: return "HEAVY_CPU";
            case WorkloadType::HEAVY_GPU: return "HEAVY_GPU";
            case WorkloadType::CAMERA:    return "CAMERA";
            case WorkloadType::INSTALL:   return "INSTALL";
        }
        return "UNKNOWN";
    }

    void setHysteresis(int n) {
        std::lock_guard<std::mutex> lock(mu_);
        hysteresisCount_ = (n < 1) ? 1 : n;
    }

private:
    static bool highPriority(WorkloadType t) {
        // 这些场景出现时立即响应（不等滞回确认）
        return t == WorkloadType::GAME ||
               t == WorkloadType::CAMERA ||
               t == WorkloadType::INSTALL;
    }

    WorkloadType rawClassify(const Signal& s) const {
        // 显式信号优先
        if (s.cameraActive) return WorkloadType::CAMERA;
        if (s.installActive) return WorkloadType::INSTALL;

        const int cpu = s.cpuLoad;
        const int gpu = s.gpuLoad;
        const int jank = s.frameJankPct;
        const int io = s.ioBusyPct;

        // 系统极闲
        if (cpu < 12 && gpu < 12 && io < 20) return WorkloadType::IDLE;

        // 安装/解压：IO 高、CPU 中
        if (io > 70 && cpu > 30) return WorkloadType::INSTALL;

        // 游戏：CPU + GPU 都高，或卡顿明显且 GPU 不低
        if ((cpu > 55 && gpu > 55) ||
            (jank > 25 && gpu > 40)) {
            return WorkloadType::GAME;
        }

        // 重 GPU（如重型 3D 但单线程瓶颈不大）
        if (gpu > 70 && cpu < 50) return WorkloadType::HEAVY_GPU;

        // 重 CPU（编译、解压、加密、AI 推理）
        if (cpu > 75 && gpu < 35) return WorkloadType::HEAVY_CPU;

        // 视频播放：GPU 中等稳定，CPU 低
        if (gpu > 25 && cpu < 45 && jank < 8) return WorkloadType::VIDEO;

        // 默认 UI
        return WorkloadType::UI;
    }

    mutable std::mutex mu_;
    WorkloadType current_ = WorkloadType::UI;
    WorkloadType pending_ = WorkloadType::UI;
    int pendingCount_ = 0;
    int hysteresisCount_ = 3;  // 连续 N 次确认才切换
    std::chrono::steady_clock::time_point lastSwitchAt_ = std::chrono::steady_clock::now();
};
