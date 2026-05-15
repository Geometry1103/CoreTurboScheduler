#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>

/**
 * SmartBoost -- 场景化 Boost 管理
 *
 * 类型：
 *   - TouchBoost   : 触摸 / 滑动
 *   - LaunchBoost  : 应用启动
 *   - FrameBoost   : 帧卡顿
 *   - BinderBoost  : Binder 调用突发
 *   - IOBoost      : IO 写入突发
 *
 * 每种 Boost 有独立的：duration、强度（util 提升）、过期回调。
 * 多个并行 Boost 取最高强度（max-merge）。
 */
class SmartBoost {
public:
    enum class Kind {
        Touch, Launch, Frame, Binder, IO, Manual
    };

    struct Active {
        Kind kind;
        int strength;       // 0~1024 用于 uclamp.min
        std::chrono::steady_clock::time_point expireAt;
    };

    SmartBoost() = default;

    void trigger(Kind k, int strength, int durationMs) {
        if (strength < 0) strength = 0;
        if (strength > 1024) strength = 1024;
        Active a{};
        a.kind = k;
        a.strength = strength;
        a.expireAt = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(durationMs);
        std::lock_guard<std::mutex> lock(mu_);
        actives_[k] = a;
    }

    /// 当前生效的最大强度（用于驱动 uclamp.min / cluster freq min）。
    int currentStrength() {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        int best = 0;
        for (auto it = actives_.begin(); it != actives_.end();) {
            if (it->second.expireAt <= now) {
                it = actives_.erase(it);
                continue;
            }
            if (it->second.strength > best) best = it->second.strength;
            ++it;
        }
        return best;
    }

    /// 返回最强 Boost 的种类（无则返回 nullptr）
    const char* dominantKindName() {
        std::lock_guard<std::mutex> lock(mu_);
        auto now = std::chrono::steady_clock::now();
        Kind k = Kind::Manual;
        int best = -1;
        for (auto it = actives_.begin(); it != actives_.end();) {
            if (it->second.expireAt <= now) { it = actives_.erase(it); continue; }
            if (it->second.strength > best) {
                best = it->second.strength;
                k = it->first;
            }
            ++it;
        }
        if (best < 0) return nullptr;
        switch (k) {
            case Kind::Touch:  return "Touch";
            case Kind::Launch: return "Launch";
            case Kind::Frame:  return "Frame";
            case Kind::Binder: return "Binder";
            case Kind::IO:     return "IO";
            case Kind::Manual: return "Manual";
        }
        return "?";
    }

    bool active() {
        return currentStrength() > 0;
    }

    void cancel(Kind k) {
        std::lock_guard<std::mutex> lock(mu_);
        actives_.erase(k);
    }

    void cancelAll() {
        std::lock_guard<std::mutex> lock(mu_);
        actives_.clear();
    }

private:
    struct KindHash {
        size_t operator()(Kind k) const noexcept {
            return static_cast<size_t>(k);
        }
    };
    std::unordered_map<Kind, Active, KindHash> actives_;
    mutable std::mutex mu_;
};
