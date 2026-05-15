#pragma once

#include <atomic>
#include <chrono>
#include <mutex>

/**
 * OverrideManager -- 控制权仲裁
 *
 * 控制权来源（按优先级）：
 *   THERMAL  > USER > SYSTEM > AI
 *
 * 例：
 *   - 热保护降频时（Owner = THERMAL）AI 不再上调频率
 *   - 用户在 WebUI 锁了大核（Owner = USER）AI 不再修改 CPU 但可继续控制 GPU/Sched
 *   - 系统进入 CRITICAL 模式（Owner = SYSTEM）暂时停用 AI 控制
 */
class OverrideManager {
public:
    enum class Owner : int {
        AI       = 0,
        SYSTEM   = 1,
        USER     = 2,
        THERMAL  = 3
    };

    enum class Domain {
        CPU, GPU, SCHED, UCLAMP
    };

    struct Lock {
        bool active = false;
        Owner owner = Owner::AI;
        std::chrono::steady_clock::time_point expireAt{};
    };

    OverrideManager() = default;

    void claim(Domain d, Owner who, int durationMs = 0) {
        std::lock_guard<std::mutex> lock(mu_);
        Lock& l = pick(d);
        if (l.active && static_cast<int>(l.owner) > static_cast<int>(who)) {
            // 已经有更高优先级 owner 持有，忽略低优先级 claim
            return;
        }
        l.active = true;
        l.owner = who;
        l.expireAt = (durationMs > 0)
            ? std::chrono::steady_clock::now() +
              std::chrono::milliseconds(durationMs)
            : std::chrono::steady_clock::time_point::max();
    }

    void release(Domain d, Owner who) {
        std::lock_guard<std::mutex> lock(mu_);
        Lock& l = pick(d);
        if (l.active && l.owner == who) {
            l.active = false;
        }
    }

    /// AI 是否仍可控制此域
    bool aiCanControl(Domain d) {
        std::lock_guard<std::mutex> lock(mu_);
        Lock& l = pick(d);
        if (!l.active) return true;
        if (std::chrono::steady_clock::now() >= l.expireAt) {
            l.active = false;
            return true;
        }
        return l.owner == Owner::AI;
    }

    Owner currentOwner(Domain d) {
        std::lock_guard<std::mutex> lock(mu_);
        Lock& l = pick(d);
        if (!l.active || std::chrono::steady_clock::now() >= l.expireAt) {
            l.active = false;
            return Owner::AI;
        }
        return l.owner;
    }

    static const char* ownerName(Owner o) {
        switch (o) {
            case Owner::AI:      return "AI";
            case Owner::SYSTEM:  return "SYSTEM";
            case Owner::USER:    return "USER";
            case Owner::THERMAL: return "THERMAL";
        }
        return "?";
    }

private:
    Lock& pick(Domain d) {
        switch (d) {
            case Domain::CPU:    return cpu_;
            case Domain::GPU:    return gpu_;
            case Domain::SCHED:  return sched_;
            case Domain::UCLAMP: return uclamp_;
        }
        return cpu_;
    }

    Lock cpu_;
    Lock gpu_;
    Lock sched_;
    Lock uclamp_;
    std::mutex mu_;
};
