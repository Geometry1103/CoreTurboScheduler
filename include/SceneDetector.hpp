#pragma once

// ============================================================
//  SceneDetector — 场景识别状态机（v4.1）
//
//  设计参考：
//    - yc9559/uperf 的 Hint 系统（None/Tap/HeavyLoad/Standby/AmSwitch）
//    - imacte/yumi 的 ModeChange + ScreenStateChange + Doze
//
//  5 个场景（优先级从高到低）：
//    Standby   — 屏幕熄屏（最高优先级，覆盖一切）
//    HeavyLoad — CPU 负载超阈值，连续 N 次确认
//    AmSwitch  — 前台应用切换（短时 1.5s）
//    Touch     — 用户在触摸/滑动屏幕（短时 1s）
//    None      — 空闲常规态（默认）
//
//  关键省电设计（参考 uperf 的 heavyLoad/idleLoad/requestBurstSlack 三阶段）：
//    1. HeavyLoad 需连续 N 次采样超阈值才进入（防误触）
//    2. 离开 HeavyLoad 后进入 requestBurstSlack 冷却期，期间不再响应新 HL 请求
//    3. 所有非 None 场景都有 maxDuration，超时自动回 None
//    4. Standby 优先级最高，进入后阻断其他场景
// ============================================================

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstring>
#include "Config.hpp"

class SceneDetector {
public:
    enum class Scene {
        None = 0,
        Touch,
        AmSwitch,
        HeavyLoad,
        Standby,
        COUNT
    };

    static const char* name(Scene s) {
        switch (s) {
            case Scene::None:      return "None";
            case Scene::Touch:     return "Touch";
            case Scene::AmSwitch:  return "AmSwitch";
            case Scene::HeavyLoad: return "HeavyLoad";
            case Scene::Standby:   return "Standby";
            default:               return "Unknown";
        }
    }

    static Scene parse(const char* s) {
        if (!s) return Scene::None;
        if (strcmp(s, "Touch") == 0)     return Scene::Touch;
        if (strcmp(s, "AmSwitch") == 0)  return Scene::AmSwitch;
        if (strcmp(s, "HeavyLoad") == 0) return Scene::HeavyLoad;
        if (strcmp(s, "Standby") == 0)   return Scene::Standby;
        return Scene::None;
    }

    SceneDetector() {
        current_.store(Scene::None);
        sceneStart_ = std::chrono::steady_clock::now();
        lastHeavyExit_ = std::chrono::steady_clock::now()
                       - std::chrono::milliseconds(10000);
    }

    Scene current() const { return current_.load(); }

    // ------------------------------------------------------------
    //  触发接口（线程安全）
    //  每个 trigger* 返回 true 表示场景**确实切换了**，调用方应触发 applyScene
    // ------------------------------------------------------------

    // 屏幕状态变化（true=亮, false=灭）
    bool triggerScreen(bool screenOn) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!screenOn) {
            // 进入 Standby — 强制覆盖一切
            return transitTo(Scene::Standby);
        } else {
            // 亮屏 — 若当前是 Standby 则回 None
            if (current_.load() == Scene::Standby) {
                return transitTo(Scene::None);
            }
            return false;
        }
    }

    // 前台应用切换
    bool triggerAmSwitch() {
        std::lock_guard<std::mutex> lk(mtx_);
        Scene cur = current_.load();
        // Standby/HeavyLoad 不被 AmSwitch 覆盖
        if (cur == Scene::Standby) return false;
        if (cur == Scene::HeavyLoad) return false;
        return transitTo(Scene::AmSwitch);
    }

    // 触摸事件
    bool triggerTouch() {
        std::lock_guard<std::mutex> lk(mtx_);
        Scene cur = current_.load();
        // Standby/HeavyLoad/AmSwitch 不被 Touch 覆盖
        if (cur == Scene::Standby) return false;
        if (cur == Scene::HeavyLoad) return false;
        if (cur == Scene::AmSwitch) return false;
        return transitTo(Scene::Touch);
    }

    // 负载采样反馈（cpuLoad 0~100）
    // 返回 true 表示场景切换
    bool feedCpuLoad(int cpuLoad) {
        std::lock_guard<std::mutex> lk(mtx_);
        Scene cur = current_.load();

        // Standby 状态下不切换
        if (cur == Scene::Standby) return false;

        // 重负载检测
        if (cpuLoad >= Config::SceneCfg::heavy_load_thd) {
            heavyHits_++;
            // 冷却期检查
            auto now = std::chrono::steady_clock::now();
            auto sinceExit = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastHeavyExit_).count();
            if (cur != Scene::HeavyLoad &&
                heavyHits_ >= Config::SceneCfg::heavy_confirm_count &&
                sinceExit >= Config::SceneCfg::request_burst_slack_ms) {
                return transitTo(Scene::HeavyLoad);
            }
        } else if (cpuLoad <= Config::SceneCfg::idle_load_thd) {
            heavyHits_ = 0;
            // 当前是 HeavyLoad 且负载已经回落 → 退出
            if (cur == Scene::HeavyLoad) {
                lastHeavyExit_ = std::chrono::steady_clock::now();
                return transitTo(Scene::None);
            }
        }
        return false;
    }

    // 周期 tick — 处理 maxDuration 超时
    // 返回 true 表示场景切换（超时回 None）
    bool tickTimeout() {
        std::lock_guard<std::mutex> lk(mtx_);
        Scene cur = current_.load();
        if (cur == Scene::None || cur == Scene::Standby) return false;

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - sceneStart_).count();

        int maxDur = 0;
        switch (cur) {
            case Scene::Touch:     maxDur = Config::SceneCfg::touch_duration_ms; break;
            case Scene::AmSwitch:  maxDur = Config::SceneCfg::am_switch_duration_ms; break;
            case Scene::HeavyLoad: maxDur = Config::SceneCfg::heavy_max_duration_ms; break;
            default: return false;
        }
        if (elapsed >= maxDur) {
            if (cur == Scene::HeavyLoad) {
                lastHeavyExit_ = now;
            }
            return transitTo(Scene::None);
        }
        return false;
    }

private:
    // 必须在持锁状态下调用
    bool transitTo(Scene next) {
        Scene cur = current_.load();
        if (cur == next) return false;
        current_.store(next);
        sceneStart_ = std::chrono::steady_clock::now();
        heavyHits_ = 0;
        return true;
    }

    std::atomic<Scene> current_;
    std::chrono::steady_clock::time_point sceneStart_;
    std::chrono::steady_clock::time_point lastHeavyExit_;
    int heavyHits_ = 0;
    std::mutex mtx_;
};
