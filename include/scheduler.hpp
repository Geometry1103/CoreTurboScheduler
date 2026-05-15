#pragma once

#include "LibUtils.hpp"
#include "Function.hpp"
#include "ConfigManager.hpp"
#include "WatchDog.hpp"
#include "AIScheduler.hpp"
#include "SysfsBatchWriter.hpp"
#include "UclampController.hpp"

#include <sys/inotify.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include <queue>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <string>

// ============================================================
//  事件类型枚举
// ============================================================
enum class EventType {
    LOAD_CHANGE,       // 负载级别变化
    WORKLOAD_CHANGE,   // 工作负载场景变化
    THERMAL_EVENT,     // 温控事件
    BOOST_EVENT,       // SmartBoost 触发
    CONFIG_UPDATE,     // 配置变更
    TIMER_TICK,        // 定时调度
    APP_SWITCH,        // 前台应用切换
    AI_DECISION,       // AI 调度决策
    SHUTDOWN
};

// ============================================================
//  事件结构
// ============================================================
struct Event {
    EventType type;
    int data;
    std::string payload;
    std::chrono::steady_clock::time_point timestamp;

    Event(EventType t = EventType::TIMER_TICK, int d = 0, std::string p = "")
        : type(t), data(d), payload(std::move(p))
        , timestamp(std::chrono::steady_clock::now()) {}
};

// ============================================================
//  Schedule -- 事件驱动 + 三线程 + AI 框架
// ============================================================
class Schedule {
private:
    static constexpr const char* configPath     = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* jsonPath       = "/sdcard/Android/CTS/config.json";
    static constexpr const char* cpusetEventPath= "/dev/cpuset/top-app";
    static constexpr const char* onlinePath     = "/sys/devices/system/cpu/cpu%d/online";
    static constexpr const char* SchedParamPath = "/sys/devices/system/cpu/cpufreq/policy%d/%s/%s";
    static constexpr const char* GovernorPath   = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor";
    static constexpr const char* MinFreqPath    = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq";
    static constexpr const char* MaxFreqPath    = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq";

    enum class EventPriority { LOW=0, NORMAL=1, HIGH=2, CRITICAL=3 };
    struct PrioritizedEvent {
        Event event;
        EventPriority priority;
        std::uint64_t timestamp;
        bool operator<(const PrioritizedEvent& other) const {
            if (priority == other.priority) return timestamp > other.timestamp;
            return static_cast<int>(priority) < static_cast<int>(other.priority);
        }
    };

    std::priority_queue<PrioritizedEvent> eventQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::atomic<bool> running_{true};

    // === 三线程 ===
    std::thread monitorThread_;
    std::thread policyThread_;
    std::thread dispatchThread_;

    // === 心跳 ===
    std::atomic<bool> monitorHeartbeat_{false};
    std::atomic<bool> policyHeartbeat_{false};
    std::atomic<bool> dispatchHeartbeat_{false};

    // === 核心组件 ===
    SysfsBatchWriter batchWriter_;
    Function function;
    ConfigManager conf;
    Logger logger;
    Utils utils;
    WatchDog watchDog_;
    AIScheduler ai_;
    UclampController uclamp_{&batchWriter_};

    // === 状态 ===
    bool cpuBoost = false;
    char temp[256];

    // 上次决策（用于 dirty-flag 抑制）
    LoadPredictor::LoadLevel lastLoadLevel_ = LoadPredictor::LoadLevel::NORMAL;
    WorkloadClassifier::WorkloadType lastWorkload_ = WorkloadClassifier::WorkloadType::UI;
    ThermalEngine::Action lastThermal_ = ThermalEngine::Action::NORMAL;

    // LaunchBoost
    bool boostActive_ = false;
    std::chrono::steady_clock::time_point boostExpireTime_;

    // ============================================================
    //  事件操作
    // ============================================================
    void pushEvent(const Event& event, EventPriority p = EventPriority::NORMAL) {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            eventQueue_.push({event, p,
                static_cast<std::uint64_t>(
                    std::chrono::steady_clock::now()
                        .time_since_epoch().count())});
        }
        queueCv_.notify_one();
    }

    bool popEvent(Event& event, int timeoutMs) {
        std::unique_lock<std::mutex> lock(queueMutex_);
        if (queueCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                [this]() { return !eventQueue_.empty() || !running_.load(); })) {
            if (!running_.load() && eventQueue_.empty()) return false;
            if (eventQueue_.empty()) return false;
            event = eventQueue_.top().event;
            eventQueue_.pop();
            return true;
        }
        return false;
    }

    // ============================================================
    //  MonitorThread
    // ============================================================
    void monitorLoop() {
        sleep(2);

        std::thread cpusetWatcher;
        if (LaunchBoost::enable) {
            cpusetWatcher = std::thread([this]() {
                monitorCpusetSwitch();
            });
        }

        const char* watchDir = "/sdcard/Android/CTS";
        int inotifyFd = -1;
        int dirWd = -1;

        auto setupInotify = [&]() -> bool {
            if (inotifyFd >= 0) {
                inotify_rm_watch(inotifyFd, dirWd);
                close(inotifyFd);
                inotifyFd = -1;
                dirWd = -1;
            }
            inotifyFd = inotify_init();
            if (inotifyFd < 0) {
                logger.Error("inotify_init 失败: %s", strerror(errno));
                return false;
            }
            dirWd = inotify_add_watch(inotifyFd, watchDir,
                                       IN_CLOSE_WRITE | IN_MOVED_TO);
            if (dirWd < 0) {
                logger.Error("inotify_add_watch 失败 %s: %s",
                             watchDir, strerror(errno));
                close(inotifyFd);
                inotifyFd = -1;
                return false;
            }
            return true;
        };

        FileState modeState = getFileState(configPath);
        FileState jsonState = getFileState(jsonPath);
        auto lastPollTime = std::chrono::steady_clock::now();

        while (running_.load()) {
            WatchDog::feed(monitorHeartbeat_);

            if (inotifyFd >= 0) {
                constexpr int buflen = sizeof(struct inotify_event) + NAME_MAX + 1;
                char buf[buflen];
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(inotifyFd, &readfds);
                struct timeval tv = {1, 0};
                int ret = select(inotifyFd + 1, &readfds, nullptr, nullptr, &tv);
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    setupInotify();
                    utils.sleep_ms(100);
                    continue;
                }
                if (ret > 0) {
                    int len = read(inotifyFd, buf, buflen);
                    if (len > 0) {
                        for (char* ptr = buf; ptr < buf + len; ) {
                            struct inotify_event* ev =
                                reinterpret_cast<struct inotify_event*>(ptr);
                            if (ev->len > 0) {
                                if (strcmp(ev->name, "mode.txt") == 0) {
                                    logger.Info("inotify: mode.txt 变化");
                                    pushEvent(Event(EventType::CONFIG_UPDATE, 0, "mode"),
                                              EventPriority::HIGH);
                                } else if (strcmp(ev->name, "config.json") == 0) {
                                    logger.Info("inotify: config.json 变化");
                                    pushEvent(Event(EventType::CONFIG_UPDATE, 1, "json"),
                                              EventPriority::HIGH);
                                }
                            }
                            ptr += sizeof(struct inotify_event) + ev->len;
                        }
                    } else {
                        setupInotify();
                    }
                }
            } else {
                setupInotify();
                utils.sleep_ms(500);
            }

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                now - lastPollTime).count();
            if (elapsed >= 3) {
                lastPollTime = now;
                bool modeChanged = fileStateChanged(modeState, configPath);
                bool jsonChanged = fileStateChanged(jsonState, jsonPath);
                if (modeChanged) {
                    pushEvent(Event(EventType::CONFIG_UPDATE, 0, "mode_poll"),
                              EventPriority::HIGH);
                }
                if (jsonChanged) {
                    pushEvent(Event(EventType::CONFIG_UPDATE, 1, "json_poll"),
                              EventPriority::HIGH);
                }
            }

            pushEvent(Event(EventType::TIMER_TICK));
        }

        if (inotifyFd >= 0) {
            inotify_rm_watch(inotifyFd, dirWd);
            close(inotifyFd);
        }
        if (cpusetWatcher.joinable()) cpusetWatcher.join();
        logger.Info("MonitorThread 已退出");
    }

    void monitorCpusetSwitch() {
        if (!LaunchBoost::enable) return;
        constexpr int TRIGGER_BUF_SIZE = 8192;
        sleep(1);

        while (running_.load()) {
            const int fd = inotify_init();
            if (fd < 0) {
                fprintf(stderr, "同步事件: 0xB1 (1/3)失败: [%d]:[%s]",
                        errno, strerror(errno));
                exit(-1);
            }
            const int wd = inotify_add_watch(fd, cpusetEventPath, IN_ALL_EVENTS);
            if (wd < 0) {
                fprintf(stderr, "同步事件: 0xB1 (2/3)失败: [%d]:[%s]",
                        errno, strerror(errno));
                exit(-1);
            }
            logger.Info("监听顶层应用切换事件成功");

            char buf[TRIGGER_BUF_SIZE];
            while (running_.load()) {
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(fd, &readfds);
                struct timeval tv = {1, 0};
                int ret = select(fd + 1, &readfds, nullptr, nullptr, &tv);
                if (ret < 0) { if (errno == EINTR) continue; break; }
                if (ret == 0) continue;
                int len = read(fd, buf, TRIGGER_BUF_SIZE);
                if (len <= 0) break;
                cpuBoost = true;
                pushEvent(Event(EventType::APP_SWITCH), EventPriority::HIGH);
                ai_.boost().trigger(SmartBoost::Kind::Launch, 800,
                                    LaunchBoost::boost_duration_ms);
                logger.Debug("前台进程已切换 已触发 LaunchBoost");
                utils.sleep_ms(500);
            }
            inotify_rm_watch(fd, wd);
            close(fd);
        }
        logger.Info("CpusetSwitch 监控已退出");
    }

    // ============================================================
    //  PolicyThread (AI 决策)
    // ============================================================
    void policyLoop() {
        int tickCount = 0;

        while (running_.load()) {
            Event event;
            if (!popEvent(event, 1000)) {
                WatchDog::feed(policyHeartbeat_);
                continue;
            }
            WatchDog::feed(policyHeartbeat_);

            switch (event.type) {
            case EventType::TIMER_TICK: {
                ++tickCount;
                checkBoostTimeout();
                int sampleTicks = std::max(1, LoadPredict::sample_interval_ms / 1000);
                if (LoadPredict::enable && tickCount % sampleTicks == 0) {
                    runAiTick();
                }
                break;
            }
            case EventType::CONFIG_UPDATE:
            case EventType::SHUTDOWN:
                if (event.type == EventType::SHUTDOWN) return;
                break;
            default:
                break;
            }
        }
        logger.Info("PolicyThread 已退出");
    }

    // ============================================================
    //  DispatchThread (执行)
    // ============================================================
    void dispatchLoop() {
        while (running_.load()) {
            Event event;
            if (!popEvent(event, 2000)) {
                WatchDog::feed(dispatchHeartbeat_);
                continue;
            }
            WatchDog::feed(dispatchHeartbeat_);

            switch (event.type) {
            case EventType::CONFIG_UPDATE: {
                bool jsonChanged = (event.data == 1);
                reloadRuntimeConfig(
                    event.payload.empty() ? "event" : event.payload.c_str(),
                    jsonChanged);
                lastLoadLevel_ = LoadPredictor::LoadLevel::NORMAL;
                lastWorkload_ = WorkloadClassifier::WorkloadType::UI;
                lastThermal_ = ThermalEngine::Action::NORMAL;
                break;
            }
            case EventType::APP_SWITCH:
                release();
                lastLoadLevel_ = LoadPredictor::LoadLevel::NORMAL;
                logger.Debug("DispatchThread: 前台切换已处理");
                break;
            case EventType::AI_DECISION:
                applyAiDecision();
                break;
            case EventType::LOAD_CHANGE:
                handleLoadChange(event.data);
                break;
            case EventType::SHUTDOWN:
                return;
            default:
                break;
            }
        }
        logger.Info("DispatchThread 已退出");
    }

    // ============================================================
    //  AI 主循环：采样 -> 决策 -> 派发
    // ============================================================
    void runAiTick() {
        // 同步 LoadPredictor 配置（支持热更新）
        ai_.predictor().setAlpha(LoadPredict::alpha);
        ai_.predictor().setPredictThd(LoadPredict::predict_thd);
        ai_.predictor().setAsymmetricAlpha(LoadPredict::alpha_up,
                                           LoadPredict::alpha_down);
        ai_.predictor().setCusumParams(LoadPredict::cusum_k,
                                       LoadPredict::cusum_h);

        AIScheduler::Signal sig{};
        double cpu = readCpuLoad();
        if (cpu < 0) cpu = 0;
        sig.cpuLoad = static_cast<int>(cpu);
        sig.gpuLoad = std::max(0, ai_.gpu().busyPercent());
        sig.currentTempC = ThermalEngine::readMaxSocTempC();
        if (sig.currentTempC < 0) sig.currentTempC = 35;
        auto frameSample = ai_.frame().sample();
        sig.frameJankPct = frameSample.frameJankPct;
        sig.ioBusyPct = readIoBusyPct();

        auto d = ai_.tick(sig);

        bool changed = (d.cpuLevel != lastLoadLevel_) ||
                       (d.workload != lastWorkload_) ||
                       (d.thermal != lastThermal_);

        if (changed) {
            logger.Info("AI: cpu=%d%% gpu=%d%% T=%dC level=%d workload=%s thermal=%s reason=%s",
                        sig.cpuLoad, sig.gpuLoad, sig.currentTempC,
                        static_cast<int>(d.cpuLevel),
                        WorkloadClassifier::name(d.workload),
                        ThermalEngine::actionName(d.thermal),
                        d.reason);
            lastLoadLevel_ = d.cpuLevel;
            lastWorkload_ = d.workload;
            lastThermal_ = d.thermal;
            pushEvent(Event(EventType::AI_DECISION), EventPriority::HIGH);
        } else if (d.boostStrength > 0) {
            // 即使没有级别变化，Boost 也要立刻派发
            pushEvent(Event(EventType::AI_DECISION), EventPriority::HIGH);
        }

        // 缓存最新决策给 dispatch 使用
        latestDecision_ = d;
    }

    AIScheduler::Decision latestDecision_{};

    // ============================================================
    //  AI 决策落地
    // ============================================================
    void applyAiDecision() {
        const auto d = latestDecision_;

        // CPU 频率：以模式基线频率乘以 scale，写入 batchWriter
        if (ai_.overrideMgr().aiCanControl(OverrideManager::Domain::CPU)) {
            for (int i = 0; i <= 3; ++i) {
                if (Policy::CpuPolicy[i] == -1) continue;
                int baseMin = Fastatoi(Performances::MinFreq[i].c_str());
                int baseMax = Fastatoi(Performances::MaxFreq[i].c_str());
                if (baseMax <= 0) continue;

                int newMin = static_cast<int>(baseMin * d.cpuMinScale);
                int newMax = static_cast<int>(baseMax * d.cpuMaxScale);
                if (newMin < 0) newMin = 0;
                if (newMax < newMin) newMax = newMin;

                char path[160];
                FastSnprintf(path, sizeof(path), MinFreqPath, Policy::CpuPolicy[i]);
                batchWriter_.queue(path, std::to_string(newMin));
                FastSnprintf(path, sizeof(path), MaxFreqPath, Policy::CpuPolicy[i]);
                batchWriter_.queue(path, std::to_string(newMax));
            }
        }

        // GPU：只调整 latest 缓存值；真正的写入仍由 Function::gpuFreqGuard 周期化执行
        if (ai_.overrideMgr().aiCanControl(OverrideManager::Domain::GPU)) {
            int baseMax = Fastatoi(GpuFreq::max_freq.c_str());
            int baseMin = Fastatoi(GpuFreq::min_freq.c_str());
            if (baseMax > 0) {
                int newMax = static_cast<int>(baseMax * d.gpuMaxScale);
                function.latestGpuMaxMhz.store(newMax);
            }
            if (baseMin > 0) {
                int newMin = static_cast<int>(baseMin * d.gpuMinScale);
                function.latestGpuMinMhz.store(newMin);
            }
        }

        // Uclamp 按工作负载预设 + AI 推荐合并
        if (ai_.overrideMgr().aiCanControl(OverrideManager::Domain::UCLAMP)) {
            using WT = WorkloadClassifier::WorkloadType;
            switch (d.workload) {
                case WT::GAME:      uclamp_.presetGame(); break;
                case WT::UI:        uclamp_.presetUI(); break;
                case WT::VIDEO:     uclamp_.presetVideo(); break;
                case WT::IDLE:      uclamp_.presetIdle(); break;
                case WT::HEAVY_CPU: uclamp_.presetHeavyCpu(); break;
                case WT::CAMERA:    uclamp_.presetCamera(); break;
                case WT::INSTALL:   uclamp_.presetInstall(); break;
                case WT::HEAVY_GPU: uclamp_.presetUI(); break;
            }
            // Boost 强度直接合到 top-app uclamp.min
            if (d.boostStrength > 0) {
                uclamp_.apply("top-app", d.boostStrength, 1024);
            }
            uclamp_.applyGlobal(d.uclampMin, d.uclampMax);
        }

        batchWriter_.flush();
    }

    // ============================================================
    //  传统 LoadChange 处理（保留兼容）
    // ============================================================
    void handleLoadChange(int level) {
        using LL = LoadPredictor::LoadLevel;
        auto loadLevel = static_cast<LL>(level);
        if (loadLevel == lastLoadLevel_) return;
        lastLoadLevel_ = loadLevel;

        switch (loadLevel) {
        case LL::IDLE:
            for (int i = 0; i <= 3; i++) {
                if (Policy::CpuPolicy[i] == -1) continue;
                string_t minF = Performances::IdleMinFreq[i].empty()
                    ? Performances::MinFreq[i] : Performances::IdleMinFreq[i];
                string_t maxF = Performances::IdleMaxFreq[i].empty()
                    ? Performances::MaxFreq[i] : Performances::IdleMaxFreq[i];
                FreqWriter(Policy::CpuPolicy[i], minF, maxF, Performances::CpuGovernor[i]);
            }
            break;
        case LL::NORMAL:
            Release();
            break;
        case LL::HEAVY:
            for (int i = 0; i <= 3; i++) {
                if (Policy::CpuPolicy[i] == -1) continue;
                string_t minF = Performances::HeavyMinFreq[i].empty()
                    ? Performances::MinFreq[i] : Performances::HeavyMinFreq[i];
                string_t maxF = Performances::HeavyMaxFreq[i].empty()
                    ? Performances::MaxFreq[i] : Performances::HeavyMaxFreq[i];
                FreqWriter(Policy::CpuPolicy[i], minF, maxF, Performances::CpuGovernor[i]);
            }
            break;
        case LL::CRITICAL:
            for (int i = 0; i <= 3; i++) {
                if (Policy::CpuPolicy[i] == -1) continue;
                string_t minF = Performances::MinFreq[i];
                string_t maxF = Performances::CriticalMaxFreq[i].empty()
                    ? Performances::MaxFreq[i] : Performances::CriticalMaxFreq[i];
                FreqWriter(Policy::CpuPolicy[i], minF, maxF, Performances::CpuGovernor[i]);
            }
            break;
        }
    }

    // ============================================================
    //  CPU 负载读取
    // ============================================================
    double readCpuLoad() {
        int cpuCount = 0;
        for (int i = 0; i < 8; i++) {
            char path[64];
            FastSnprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);
            if (access(path, F_OK) == 0) cpuCount = i + 1;
        }
        if (cpuCount == 0) return -1.0;

        static unsigned long long prevUser[8] = {};
        static unsigned long long prevTotal[8] = {};
        static bool firstRead = true;

        double totalWeightedLoad = 0.0;
        double weightSum = 0.0;

        for (int i = 0; i < cpuCount && i < 8; i++) {
            char statPath[64];
            FastSnprintf(statPath, sizeof(statPath),
                         "/sys/devices/system/cpu/cpu%d/stat", i);
            FILE* fp = fopen(statPath, "r");
            if (!fp) continue;
            char line[256];
            if (!fgets(line, sizeof(line), fp)) { fclose(fp); continue; }
            fclose(fp);

            char* p = line;
            while (*p == ' ') p++;
            if (*p == 'c') { p++; while (*p >= '0' && *p <= '9') p++; }
            while (*p == ' ') p++;

            unsigned long long user=0, nice=0, sys=0, idle=0;
            unsigned long long iowait=0, irq=0, softirq=0, steal=0;
            sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &user,&nice,&sys,&idle,&iowait,&irq,&softirq,&steal);

            unsigned long long total = user+nice+sys+idle+iowait+irq+softirq+steal;
            if (firstRead) {
                prevUser[i] = user+nice+sys;
                prevTotal[i] = total;
                continue;
            }
            unsigned long long diffBusy = (user+nice+sys) - prevUser[i];
            unsigned long long diffTotal = total - prevTotal[i];
            prevUser[i] = user+nice+sys;
            prevTotal[i] = total;
            if (diffTotal == 0) continue;

            double loadPct = 100.0 *
                static_cast<double>(diffBusy) / static_cast<double>(diffTotal);

            double eff = static_cast<double>(LoadPredict::efficiency[i]);
            totalWeightedLoad += eff * loadPct;
            weightSum += eff;
        }

        if (firstRead) { firstRead = false; return 0.0; }
        if (weightSum < 1e-6) return 0.0;
        return totalWeightedLoad / weightSum;  // 规范化到 0~100
    }

    int readIoBusyPct() {
        // 取 sda / mmcblk0 的 iostat 第10列 (io_ticks)
        const char* devs[] = {
            "/sys/block/sda/stat",
            "/sys/block/mmcblk0/stat",
            nullptr
        };
        static unsigned long long prevTicks = 0;
        static auto prevTime = std::chrono::steady_clock::now();
        unsigned long long ticks = 0;
        for (int i = 0; devs[i]; ++i) {
            FILE* fp = fopen(devs[i], "r");
            if (!fp) continue;
            unsigned long long v[11] = {0};
            int n = fscanf(fp, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                           &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10]);
            fclose(fp);
            if (n >= 10) ticks += v[9];
        }
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - prevTime).count();
        unsigned long long delta = (ticks > prevTicks) ? (ticks - prevTicks) : 0;
        prevTicks = ticks;
        prevTime = now;
        if (ms <= 0) return 0;
        int pct = static_cast<int>((delta * 100) / static_cast<unsigned long long>(ms));
        if (pct > 100) pct = 100;
        if (pct < 0) pct = 0;
        return pct;
    }

    // ============================================================
    //  传统频率控制
    // ============================================================
    void FreqWriter(const int Policy, const string_t MinFreq,
                    const string_t MaxFreq, const string_t Governor) {
        FastSnprintf(temp, sizeof(temp), MinFreqPath, Policy);
        utils.FileWrite(temp, MinFreq);
        FastSnprintf(temp, sizeof(temp), MaxFreqPath, Policy);
        utils.FileWrite(temp, MaxFreq);
        FastSnprintf(temp, sizeof(temp), GovernorPath, Policy);
        utils.FileWrite(temp, Governor);
        logger.Debug("CPU簇:%d min:%s max:%s gov:%s",
                     Policy, MinFreq.c_str(), MaxFreq.c_str(), Governor.c_str());
    }

    void Boost() {
        boostActive_ = true;
        boostExpireTime_ = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(LaunchBoost::boost_duration_ms);
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i],
                       LaunchBoost::BoostFreq[i], Performances::CpuGovernor[i]);
            utils.sleep_ms(LaunchBoost::boost_rate_limit_ms);
        }
    }

    void checkBoostTimeout() {
        if (!boostActive_) return;
        if (std::chrono::steady_clock::now() < boostExpireTime_) return;
        boostActive_ = false;
        logger.Info("LaunchBoost 超时，恢复基础频率");
        Release();
    }

    void Release() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i],
                       Performances::MaxFreq[i], Performances::CpuGovernor[i]);
        }
        function.FeasFunc(false);
    }

    void Reset() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647",
                       function.checkQcom() ? "walt" : "sugov_ext");
        }
        function.FeasFunc(true);
    }

    void release() {
        if (conf.mode.empty()) {
            logger.Warn("情景模式为空，跳过应用配置");
            return;
        }
        logger.Info("情景模式: %s 已启用", conf.mode.c_str());
        if (cpuBoost) {
            Boost();
            cpuBoost = false;
        } else if (conf.mode == "fast" && OfficialMode::enable) {
            Reset();
        } else {
            Release();
        }
    }

    void online() {
        for (int i = 0; i <= 7; i++) {
            FastSnprintf(temp, sizeof(temp), onlinePath, i);
            utils.WriteInt(temp, Performances::Online[i]);
            logger.Debug("核心: %d %s", i, Performances::Online[i] ? "开启" : "关闭");
        }
    }

    void SchedParam() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (Performances::CpuGovernor[i].empty()) continue;
            for (int j = 1; j <= 12; j++) {
                if (conf.schedParam[i].Name[j].empty()) continue;
                FastSnprintf(temp, sizeof(temp), SchedParamPath,
                             Policy::CpuPolicy[i],
                             Performances::CpuGovernor[i].c_str(),
                             conf.schedParam[i].Name[j].c_str());
                utils.FileWrite(temp, conf.schedParam[i].Value[j].c_str());
            }
        }
    }

    void applyAllConfig() {
        release();
        SchedParam();
        online();
        function.gpuFreqControl();
    }

    // ============================================================
    //  配置监控辅助
    // ============================================================
    struct FileState { time_t mtime; off_t size; };

    FileState getFileState(const char* path) {
        struct stat st {};
        if (stat(path, &st) != 0) return {0, 0};
        return {st.st_mtime, st.st_size};
    }

    bool fileStateChanged(FileState& oldState, const char* path) {
        FileState now = getFileState(path);
        if (now.mtime != oldState.mtime || now.size != oldState.size) {
            oldState = now;
            return true;
        }
        return false;
    }

    void reloadRuntimeConfig(const char* reason, bool jsonChanged) {
        if (conf.readConfig()) {
            if (jsonChanged) {
                logger.setLogLevel(Meta::loglevel);
                function.AllFunC();
            }
            logger.Info("配置已重新加载: %s", reason);
            applyAllConfig();
            // 配置变化后让 batchWriter 强制下次重写
            batchWriter_.invalidateAll();
        } else {
            logger.Warn("配置重新加载失败: %s", reason);
        }
    }

    // ============================================================
    //  初始化
    // ============================================================
    void Init() {
        const int myPid = getpid();
        const char* checkNames[] = {
            "CoreTurboScheduler", "CpuTurboScheduler", "MW_CpuSpeedController"
        };

        for (const char* name : checkNames) {
            char cmd[64], buf[256] = {0};
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) kill(pid, SIGTERM);
                    token = strtok(nullptr, " \t\n");
                }
            }
        }
        sleep(1);

        for (const char* name : checkNames) {
            char cmd[64], buf[256] = {0};
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) kill(pid, SIGKILL);
                    token = strtok(nullptr, " \t\n");
                }
            }
        }
        sleep(1);

        for (const char* name : checkNames) {
            char cmd[64], buf[256] = {0};
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        printf("\n!!! CTS调度已经在运行(%s pid:%d) 当前(pid:%d)即将退出\n",
                               name, pid, myPid);
                        exit(-1);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }

        logger.clear_log();
        bool configOk = conf.readConfig();
        logger.setLogLevel(Meta::loglevel);
        logger.Info("名称: %s", Meta::name.c_str());
        logger.Info("版本: %d", Meta::version);
        logger.Info("作者: %s", Meta::author.c_str());
        logger.Info("Android AI Scheduling Framework 启动");

        function.stopGuards();
        function.AllFunC();
        if (configOk) {
            release();
            online();
            SchedParam();
            function.gpuFreqControl();
        } else {
            logger.Warn("初始配置加载失败，尝试使用默认配置");
        }

        // 启动 SysfsBatchWriter 与 EAS 探测
        batchWriter_.start(16);
        ai_.eas().scan();
        if (ai_.eas().easSupported()) {
            logger.Info("EAS Energy Model 已检测，clusters=%zu",
                        ai_.eas().models().size());
        }
    }

public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        Init();

        watchDog_.registerThread("monitor", &monitorHeartbeat_,
                                 std::chrono::seconds(10), nullptr);
        watchDog_.registerThread("policy", &policyHeartbeat_,
                                 std::chrono::seconds(10), nullptr);
        watchDog_.registerThread("dispatch", &dispatchHeartbeat_,
                                 std::chrono::seconds(10), nullptr);
        watchDog_.setGlobalRecovery([this]() {
            logger.Error("WatchDog: 检测到线程异常，执行全局恢复");
            Reset();
        });
        watchDog_.start();

        monitorThread_  = std::thread(&Schedule::monitorLoop, this);
        policyThread_   = std::thread(&Schedule::policyLoop, this);
        dispatchThread_ = std::thread(&Schedule::dispatchLoop, this);
    }

    ~Schedule() {
        running_.store(false, std::memory_order_release);
        pushEvent(Event(EventType::SHUTDOWN));
        queueCv_.notify_all();

        if (monitorThread_.joinable())  monitorThread_.join();
        if (policyThread_.joinable())   policyThread_.join();
        if (dispatchThread_.joinable()) dispatchThread_.join();

        watchDog_.stop();
        function.stopGuards();
        batchWriter_.stop();

        logger.Info("Schedule 已销毁");
    }
};
