#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include "LoadPredictor.hpp"
#include "WorkloadClassifier.hpp"
#include "GPUTracker.hpp"
#include "ThermalEngine.hpp"
#include "FrameAnalyzer.hpp"
#include "UclampController.hpp"
#include "EASPolicy.hpp"
#include "SmartBoost.hpp"
#include "OverrideManager.hpp"
#include "SysfsBatchWriter.hpp"

/**
 * AIScheduler -- 自适应调度协调器
 *
 * 设计思路：
 *   - 不直接拥有线程；上层 Schedule 把采样后的 Signal 喂入。
 *   - tick(signal) 返回一个 Decision，Schedule 据此调用 Function 写入。
 *   - Override / Boost / Workload 的优先级在这里集中仲裁。
 */
class AIScheduler {
public:
    enum class Mode {
        AUTO,
        BATTERY,
        BALANCED,
        PERFORMANCE,
        GAMING
    };

    struct Signal {
        int cpuLoad = 0;        // 0~100
        int gpuLoad = 0;        // 0~100
        int frameJankPct = 0;   // 0~100
        int ioBusyPct = 0;
        int currentTempC = 0;
        bool cameraActive = false;
        bool installActive = false;
    };

    struct Decision {
        WorkloadClassifier::WorkloadType workload =
            WorkloadClassifier::WorkloadType::UI;
        ThermalEngine::Action thermal = ThermalEngine::Action::NORMAL;
        LoadPredictor::LoadLevel cpuLevel = LoadPredictor::LoadLevel::NORMAL;

        // 频率建议（相对于模式基线的乘子）
        double cpuMaxScale = 1.0;   // 1.0 = 不变
        double cpuMinScale = 1.0;
        double gpuMaxScale = 1.0;
        double gpuMinScale = 1.0;

        int uclampMin = -1;        // -1 = 不调整
        int uclampMax = -1;
        int boostStrength = 0;
        const char* boostKind = nullptr;

        // 解释字段
        bool levelChanged = false;
        const char* reason = "";
    };

    AIScheduler()
        : predictor_(),
          classifier_(),
          gpu_(),
          thermal_(),
          frame_(),
          eas_(),
          boost_(),
          override_() {}

    void setMode(Mode m) { mode_.store(m); }
    Mode getMode() const { return mode_.load(); }

    LoadPredictor& predictor() { return predictor_; }
    WorkloadClassifier& classifier() { return classifier_; }
    GPUTracker& gpu() { return gpu_; }
    ThermalEngine& thermal() { return thermal_; }
    FrameAnalyzer& frame() { return frame_; }
    EASPolicy& eas() { return eas_; }
    SmartBoost& boost() { return boost_; }
    OverrideManager& overrideMgr() { return override_; }

    /// 一次完整的决策循环。线程安全。
    Decision tick(const Signal& s) {
        Decision d{};

        // 1. 负载预测
        auto pred = predictor_.update(s.cpuLoad);
        d.cpuLevel = pred.level;
        d.levelChanged = pred.levelChanged;

        // 2. 帧信号兜底
        frame_.feedCpuJankSignal(s.cpuLoad);

        // 3. 工作负载分类
        WorkloadClassifier::Signal ws{};
        ws.cpuLoad = s.cpuLoad;
        ws.gpuLoad = s.gpuLoad;
        ws.frameJankPct = s.frameJankPct;
        ws.ioBusyPct = s.ioBusyPct;
        ws.cameraActive = s.cameraActive;
        ws.installActive = s.installActive;
        d.workload = classifier_.classify(ws);

        // 4. 热预测
        auto thermSnap = thermal_.push(s.currentTempC);
        d.thermal = thermSnap.action;

        // 5. 模式 + 场景 -> 频率建议
        applyModePreset(d, s);
        applyWorkloadOverlay(d, s);

        // 6. 热保护反向限频
        applyThermalLimits(d, thermSnap);

        // 7. SmartBoost 叠加
        int strength = boost_.currentStrength();
        d.boostStrength = strength;
        d.boostKind = boost_.dominantKindName();
        if (strength > 0 && d.uclampMin >= 0 && strength > d.uclampMin) {
            d.uclampMin = strength;
        } else if (strength > 0 && d.uclampMin < 0) {
            d.uclampMin = strength;
        }

        return d;
    }

private:
    void applyModePreset(Decision& d, const Signal& /*s*/) const {
        switch (mode_.load()) {
            case Mode::BATTERY:
                d.cpuMaxScale = 0.65;
                d.gpuMaxScale = 0.7;
                d.uclampMin = 0;
                d.uclampMax = 768;
                d.reason = "BATTERY";
                break;
            case Mode::BALANCED:
                d.cpuMaxScale = 0.9;
                d.gpuMaxScale = 0.9;
                d.uclampMin = 200;
                d.uclampMax = 1024;
                d.reason = "BALANCED";
                break;
            case Mode::PERFORMANCE:
                d.cpuMaxScale = 1.0;
                d.gpuMaxScale = 1.0;
                d.uclampMin = 384;
                d.uclampMax = 1024;
                d.reason = "PERFORMANCE";
                break;
            case Mode::GAMING:
                d.cpuMinScale = 1.05;
                d.cpuMaxScale = 1.0;
                d.gpuMinScale = 1.05;
                d.gpuMaxScale = 1.0;
                d.uclampMin = 768;
                d.uclampMax = 1024;
                d.reason = "GAMING";
                break;
            case Mode::AUTO:
            default:
                d.cpuMaxScale = 1.0;
                d.gpuMaxScale = 1.0;
                d.uclampMin = 256;
                d.uclampMax = 1024;
                d.reason = "AUTO";
                break;
        }
    }

    void applyWorkloadOverlay(Decision& d, const Signal& s) const {
        using WT = WorkloadClassifier::WorkloadType;
        switch (d.workload) {
            case WT::IDLE:
                d.cpuMaxScale *= 0.65;
                d.gpuMaxScale *= 0.6;
                d.uclampMin = 0;
                if (d.uclampMax > 768) d.uclampMax = 768;
                break;
            case WT::UI:
                if (d.uclampMin < 384) d.uclampMin = 384;
                break;
            case WT::VIDEO:
                d.cpuMaxScale *= 0.85;
                if (d.uclampMin < 256) d.uclampMin = 256;
                break;
            case WT::GAME:
                d.cpuMinScale = std::max(d.cpuMinScale, 1.10);
                d.gpuMinScale = std::max(d.gpuMinScale, 1.10);
                if (d.uclampMin < 700) d.uclampMin = 700;
                d.uclampMax = 1024;
                break;
            case WT::HEAVY_CPU:
                if (d.uclampMin < 600) d.uclampMin = 600;
                d.uclampMax = 1024;
                break;
            case WT::HEAVY_GPU:
                d.gpuMinScale = std::max(d.gpuMinScale, 1.15);
                if (d.uclampMin < 384) d.uclampMin = 384;
                break;
            case WT::CAMERA:
                if (d.uclampMin < 600) d.uclampMin = 600;
                d.uclampMax = 1024;
                break;
            case WT::INSTALL:
                if (d.uclampMin < 512) d.uclampMin = 512;
                break;
        }
        // 高负载等级强制提升
        switch (d.cpuLevel) {
            case LoadPredictor::LoadLevel::CRITICAL:
                d.cpuMinScale = std::max(d.cpuMinScale, 1.15);
                d.uclampMin = std::max(d.uclampMin, 800);
                break;
            case LoadPredictor::LoadLevel::HEAVY:
                d.cpuMinScale = std::max(d.cpuMinScale, 1.05);
                d.uclampMin = std::max(d.uclampMin, 512);
                break;
            case LoadPredictor::LoadLevel::IDLE:
                d.cpuMaxScale = std::min(d.cpuMaxScale, 0.7);
                break;
            case LoadPredictor::LoadLevel::NORMAL:
            default:
                break;
        }
        (void)s;
    }

    void applyThermalLimits(Decision& d, const ThermalEngine::Snapshot& t) const {
        switch (t.action) {
            case ThermalEngine::Action::COOL:
                d.cpuMaxScale = std::min(d.cpuMaxScale, 0.55);
                d.gpuMaxScale = std::min(d.gpuMaxScale, 0.55);
                d.cpuMinScale = std::min(d.cpuMinScale, 0.85);
                if (d.uclampMin > 256) d.uclampMin = 256;
                if (d.uclampMax > 700) d.uclampMax = 700;
                d.reason = "THERMAL_COOL";
                break;
            case ThermalEngine::Action::LIMIT:
                d.cpuMaxScale = std::min(d.cpuMaxScale, 0.8);
                d.gpuMaxScale = std::min(d.gpuMaxScale, 0.85);
                if (d.uclampMax > 850) d.uclampMax = 850;
                d.reason = "THERMAL_LIMIT";
                break;
            case ThermalEngine::Action::BURST:
                d.cpuMaxScale *= 1.05;
                d.gpuMaxScale *= 1.05;
                break;
            case ThermalEngine::Action::NORMAL:
            default:
                break;
        }
    }

    std::atomic<Mode> mode_{Mode::AUTO};
    LoadPredictor predictor_;
    WorkloadClassifier classifier_;
    GPUTracker gpu_;
    ThermalEngine thermal_;
    FrameAnalyzer frame_;
    EASPolicy eas_;
    SmartBoost boost_;
    OverrideManager override_;
};
