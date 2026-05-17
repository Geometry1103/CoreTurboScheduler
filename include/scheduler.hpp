#pragma once

#include "LibUtils.hpp"
#include "Function.hpp"
#include "SceneDetector.hpp"
#include <sys/inotify.h>
#include <linux/limits.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <atomic>

class Schedule {
private:
    static constexpr const char* configPath = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* jsonPath = "/sdcard/Android/CTS/config.json";
    static constexpr const char* cpusetEventPath = "/dev/cpuset/top-app";
    static constexpr const char* onlinePath = "/sys/devices/system/cpu/cpu%d/online";
    static constexpr const char* SchedParamPath = "/sys/devices/system/cpu/cpufreq/policy%d/%s/%s";
    static constexpr const char* GovernorPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_governor";
    static constexpr const char* MinFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_min_freq";
    static constexpr const char* MaxFreqPath = "/sys/devices/system/cpu/cpufreq/policy%d/scaling_max_freq";

    std::vector<thread> threads;

    Function function;
    JsonConfig conf;
    Logger logger;
    Utils utils;
    SceneDetector scene_;

    bool cpuBoost = false;
    bool netlinkScreenOk = false;    // Netlink uevent 是否成功初始化
    std::string lastTopApp;          // 上一次前台应用包名（用于画像切换去重）

    // [v4.1 dedup] 缓存上一次写入的 (min,max,gov)，相同则跳过 sysfs 写
    // 避免 None→Touch→None / 重复 applyScene 等抖动场景重复刷盘
    string_t lastMinFreq[4];
    string_t lastMaxFreq[4];
    string_t lastGovernor[4];

    // [Fix v4.3] 原版 char temp[256] 是 class 成员，多线程并发写同一 buffer 会乱
    //            把它从成员里删掉，FreqWriter / applyAppProfile / online 改用栈上 buffer
public:
    Schedule& operator=(Schedule&&) = delete;

    Schedule() {
        Init();
        threads.emplace_back(thread(&Schedule::configTriggerTask, this));
        threads.emplace_back(thread(&Schedule::jsonTriggerTask, this));
        // [Fix v4.3] 去掉每秒一次的 configPollingTask —— 与 inotify 重复，纯费电
        threads.emplace_back(thread(&Schedule::cpuSetTriggerTask, this));

        // v4.1 场景识别线程组
        if (Config::SceneCfg::enable) {
            threads.emplace_back(thread(&Schedule::sceneTickTask, this));
            // v4.2: 优先使用 Netlink uevent 监听屏幕，失败则 fallback 到轮询
            threads.emplace_back(thread(&Schedule::screenStateTask, this));
            threads.emplace_back(thread(&Schedule::loadSamplingTask, this));
            if (Config::SceneCfg::touch_enable) {
                threads.emplace_back(thread(&Schedule::touchDetectTask, this));
            }
        }

        // v4.2 应用画像线程（独立于 LaunchBoost，通过 dumpsys 获取前台包名）
        if (Config::AppProfile::enable) {
            threads.emplace_back(thread(&Schedule::appProfileTask, this));
        }
    }

    void FreqWriter(const int Policy, const string_t MinFreq, const string_t MaxFreq, const string_t Governor) {
        // 找到 Policy 对应的簇索引（c0..c3）做缓存比较
        int cluster = -1;
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == Policy) { cluster = i; break; }
        }
        if (cluster >= 0 &&
            lastMinFreq[cluster]  == MinFreq &&
            lastMaxFreq[cluster]  == MaxFreq &&
            lastGovernor[cluster] == Governor) {
            // 频率/调速器与上一次完全相同 — 跳过 sysfs 写
            return;
        }

        char path[256];
        FastSnprintf(path, sizeof(path), MinFreqPath, Policy);
        utils.FileWrite(path, MinFreq);
        logger.Debug("CPU簇: %d 最小频率: %s", Policy, MinFreq.c_str());

        FastSnprintf(path, sizeof(path), MaxFreqPath, Policy);
        utils.FileWrite(path, MaxFreq);
        logger.Debug("CPU簇: %d 最大频率: %s", Policy, MaxFreq.c_str());

        FastSnprintf(path, sizeof(path), GovernorPath, Policy);
        utils.FileWrite(path, Governor);
        logger.Debug("CPU簇: %d 调速器: %s", Policy, Governor.c_str());

        if (cluster >= 0) {
            lastMinFreq[cluster]  = MinFreq;
            lastMaxFreq[cluster]  = MaxFreq;
            lastGovernor[cluster] = Governor;
        }
    }

    void Boost() {
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], Performances::MinFreq[i], 
                    LaunchBoost::BoostFreq[i], Performances::CpuGovernor[i]);
            utils.sleep_ms(LaunchBoost::boost_rate_limit_ms);
        }
    }

    // ============================================================
    //  v4.1 按场景写入频率
    //  - 场景配置任一字段为空 → 继承 Performances 基础值
    //  - scene=None 时等同于原 Release() 行为
    // ============================================================
    void applyScene(SceneDetector::Scene scene) {
        const int idx = static_cast<int>(scene);
        if (idx < 0 || idx >= SceneFreq::SCENE_COUNT) {
            Release();
            return;
        }
        if (!SceneFreq::enable) {
            Release();
            return;
        }
        // [v4.1] None 场景 = 基础模型频率（不再走 SceneFreq 查表，逻辑等价于 Release）
        // 这样 None 不会因为 SceneFreq::MinFreq[None] 为空 vs 非空的细微差异多写一次
        if (scene == SceneDetector::Scene::None) {
            Release();
            return;
        }
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            // 缺省值回退到 Performances 基础值
            const string_t& minF = SceneFreq::MinFreq[idx][i].empty()
                                 ? Performances::MinFreq[i]
                                 : SceneFreq::MinFreq[idx][i];
            const string_t& maxF = SceneFreq::MaxFreq[idx][i].empty()
                                 ? Performances::MaxFreq[i]
                                 : SceneFreq::MaxFreq[idx][i];
            const string_t& gov  = SceneFreq::Governor[idx][i].empty()
                                 ? Performances::CpuGovernor[i]
                                 : SceneFreq::Governor[idx][i];
            FreqWriter(Policy::CpuPolicy[i], minF, maxF, gov);
        }
        logger.Info("场景已切换: %s", SceneDetector::name(scene));
    }

    // ============================================================
    //  v4.2 应用画像应用 — 场景分类画像
    //
    //  参考 Way_Balance 设计：
    //  - 按场景分类定义策略（如 game_heavy, daily_lite）
    //  - 支持通配符匹配包名（* 匹配任意序列，? 匹配单个字符）
    //  - 按 models 数组顺序匹配，第一个匹配的场景生效
    //
    //  优先级：AppProfile > SceneFreq > Performances
    //  - AppProfile 中非空字段覆盖场景频率
    //  - AppProfile 中空字段回退到 SceneFreq → Performances
    //  - GPU 频率和核心开关仅在画像中定义时才覆盖
    // ============================================================
    void applyAppProfile(int modelIdx, SceneDetector::Scene scene) {
        if (modelIdx < 0 || modelIdx >= Config::AppProfile::modelCount) {
            // 无匹配画像，使用场景频率
            applyScene(scene);
            return;
        }

        const auto& model = Config::AppProfile::Models[modelIdx];
        const int sIdx = static_cast<int>(scene);

        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;

            // 三级回退：AppProfile -> SceneFreq -> Performances
            const string_t& minF = !model.MinFreq[i].empty() ? model.MinFreq[i]
                                 : (!SceneFreq::MinFreq[sIdx][i].empty() ? SceneFreq::MinFreq[sIdx][i]
                                 : Performances::MinFreq[i]);
            const string_t& maxF = !model.MaxFreq[i].empty() ? model.MaxFreq[i]
                                 : (!SceneFreq::MaxFreq[sIdx][i].empty() ? SceneFreq::MaxFreq[sIdx][i]
                                 : Performances::MaxFreq[i]);
            const string_t& gov  = !model.Governor[i].empty() ? model.Governor[i]
                                 : (!SceneFreq::Governor[sIdx][i].empty() ? SceneFreq::Governor[sIdx][i]
                                 : Performances::CpuGovernor[i]);
            FreqWriter(Policy::CpuPolicy[i], minF, maxF, gov);
        }

        // GPU 频率（仅在画像中定义时覆盖）
        if (!model.GpuMinFreq.empty() || !model.GpuMaxFreq.empty()) {
            const string_t& gpuMin = !model.GpuMinFreq.empty() ? model.GpuMinFreq : GpuFreq::min_freq;
            const string_t& gpuMax = !model.GpuMaxFreq.empty() ? model.GpuMaxFreq : GpuFreq::max_freq;
            function.gpuFreqControlCustom(gpuMin, gpuMax);
        }

        // 核心开关（仅在画像中定义了有效值时覆盖）
        bool hasCustomOnline = false;
        for (int i = 0; i <= 7; i++) {
            if (model.Online[i] != -1) { hasCustomOnline = true; break; }
        }
        if (hasCustomOnline) {
            char path[256];
            for (int i = 0; i <= 7; i++) {
                if (model.Online[i] == -1) continue;
                FastSnprintf(path, sizeof(path), onlinePath, i);
                utils.WriteInt(path, model.Online[i]);
                logger.Debug("画像核心: %d %s", i, model.Online[i] ? "开启" : "关闭");
            }
        }

        // 调速器自定义参数 SchedParam（仅在画像中定义时覆盖）
        char spPath[256];
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (model.SchedParamCount[i] == 0) continue;
            // 使用画像的 governor，回退到场景/基础
            const string_t& gov = !model.Governor[i].empty() ? model.Governor[i]
                                 : (!SceneFreq::Governor[sIdx][i].empty() ? SceneFreq::Governor[sIdx][i]
                                 : Performances::CpuGovernor[i]);
            if (gov.empty()) continue;
            for (int j = 0; j < model.SchedParamCount[i]; j++) {
                if (model.SchedParamName[i][j].empty()) continue;
                FastSnprintf(spPath, sizeof(spPath), SchedParamPath,
                             Policy::CpuPolicy[i], gov.c_str(),
                             model.SchedParamName[i][j].c_str());
                utils.FileWrite(spPath, model.SchedParamValue[i][j].c_str());
                logger.Debug("画像调速器参数: c%d/%s = %s",
                             i, model.SchedParamName[i][j].c_str(),
                             model.SchedParamValue[i][j].c_str());
            }
        }

        logger.Info("场景画像已应用: %s (isGame=%d) 场景: %s",
                    model.modelName.c_str(),
                    model.isGame ? 1 : 0,
                    SceneDetector::name(scene));
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
        // Reset 是"强制覆盖"路径，绕过去重缓存
        invalidateFreqCache();
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            FreqWriter(Policy::CpuPolicy[i], "0", "2147483647",
                    function.checkQcom() ? "walt" : "sugov_ext");
        }
        function.FeasFunc(true);
    }

    // 清空 FreqWriter 去重缓存（强制下次写入）
    void invalidateFreqCache() {
        for (int i = 0; i <= 3; i++) {
            lastMinFreq[i].clear();
            lastMaxFreq[i].clear();
            lastGovernor[i].clear();
        }
    }

    void release() {
        if (conf.mode.empty()) {
            logger.Warn("情景模式为空，跳过应用配置");
            return;
        }
        logger.Info("情景模式: %s 已启用", conf.mode.c_str());
        if (cpuBoost) {
            // 旧 LaunchBoost 路径 — 触发 AmSwitch 场景
            cpuBoost = false;
            if (Config::SceneCfg::enable) {
                if (scene_.triggerAmSwitch()) {
                    applyWithProfile(scene_.current());
                    return;
                }
            } else {
                Boost();
                return;
            }
        }
        if (conf.mode == "fast" && OfficialMode::enable) {
            Reset();
            return;
        }
        // 普通路径 — 按当前场景 + 应用画像写入
        applyWithProfile(scene_.current());
    }

    // 统一的频率应用入口：自动判断是否使用应用画像
    // [Fix v4.1] currentMatch 现在是 atomic，必须 .load()
    // [Fix v4.1] 当从"匹配画像"切到"无匹配"时，需要恢复基础模型的 CoreOnline
    //            否则游戏开了所有核心后退出回桌面，大核继续在线一直耗电
    void applyWithProfile(SceneDetector::Scene scene) {
        int matchIdx = Config::AppProfile::currentMatch.load();
        if (Config::AppProfile::enable && matchIdx >= 0) {
            applyAppProfile(matchIdx, scene);
        } else if (Config::SceneCfg::enable) {
            applyScene(scene);
            // 恢复基础模型的核心开关（仅当 Performances::Online 有定义时才写）
            restoreBaseCoresIfNeeded();
        } else {
            Release();
            restoreBaseCoresIfNeeded();
        }
    }

    // 将核心开关恢复为基础模型的设置（Performances::Online）
    // 仅在至少有一个 Online[i] != -1 时才动作，避免无谓的 sysfs 写
    void restoreBaseCoresIfNeeded() {
        bool anyDefined = false;
        for (int i = 0; i <= 7; i++) {
            if (Performances::Online[i] != -1) { anyDefined = true; break; }
        }
        if (anyDefined) online();
    }

    void online() {
        char path[256];
        for (int i = 0; i <= 7; i++) {
            if (Performances::Online[i] == -1) continue; // [Fix v4.3] -1 = 跳过，不再误关核心
            FastSnprintf(path, sizeof(path), onlinePath, i);
            utils.WriteInt(path, Performances::Online[i]);
            logger.Debug("核心: %d %s", i, Performances::Online[i] ? "开启" : "关闭");
        }
    }

    void SchedParam() {
        char path[256];
        for (int i = 0; i <= 3; i++) {
            if (Policy::CpuPolicy[i] == -1) continue;
            if (Performances::CpuGovernor[i].empty()) continue;
            for (int j = 1; j <= 16; j++) {
                if (conf.schedParam[i].Name[j].empty()) continue;
                FastSnprintf(path, sizeof(path), SchedParamPath, Policy::CpuPolicy[i], Performances::CpuGovernor[i].c_str(), conf.schedParam[i].Name[j].c_str());
                utils.FileWrite(path, conf.schedParam[i].Value[j].c_str());
                logger.Debug("CPU簇: %d 调速器参数: %s 值: %s", Policy::CpuPolicy[i], conf.schedParam[i].Name[j].c_str(), conf.schedParam[i].Value[j].c_str());
            }
        }
    }

    /**
     * 完整应用所有配置
     * release(频率) + SchedParam(调速器参数) + online(核心开关) + gpuFreqControl(GPU)
     */
    void applyAllConfig() {
        release();
        SchedParam();
        online();
        function.gpuFreqControl();
    }

    // [Fix v4.1] 删除了 FileState / getFileState / fileStateChanged / reloadRuntimeConfig 等
    //            polling 时代的 dead code（已由 inotify 覆盖）

    /**
     * 监控目录 /sdcard/Android/CTS/ 下的 mode.txt 文件变化
     * /sdcard/ 是 FUSE 文件系统，inotify 监控文件可能失败，改为监控目录
     */
    void configTriggerTask() {
        sleep(2);
        const char* watchDir = "/sdcard/Android/CTS";
        const char* targetFile = "mode.txt";
        while (true) {
            if (!watchFileInDir(watchDir, targetFile, IN_CLOSE_WRITE)) {
                sleep(5); continue;
            }
            if (conf.readConfig()) {
                applyAllConfig();
            } else {
                logger.Warn("配置重载失败，跳过应用");
            }
        }
    }

    /**
     * 监控目录 /sdcard/Android/CTS/ 下的 config.json 文件变化
     */
    void jsonTriggerTask() {
        sleep(2);
        const char* watchDir = "/sdcard/Android/CTS";
        const char* targetFile = "config.json";
        while (true) {
            if (!watchFileInDir(watchDir, targetFile, IN_CLOSE_WRITE)) {
                sleep(5); continue;
            }
            if (conf.readConfig()) {
                logger.setLogLevel(Meta::loglevel);
                function.ReloadFunC();   // [Fix v4.1] 用 ReloadFunC 而非 AllFunC（不重启守护线程）
                applyAllConfig();
            } else {
                logger.Warn("JSON 配置重载失败，跳过应用");
            }
        }
    }

    /**
     * 通用目录监控函数 — 监控目录下的特定文件
     * FUSE 文件系统上 inotify 不能监控文件，只能监控目录
     */
    bool watchFileInDir(const char* dirPath, const char* targetFile, uint32_t mask) {
        int fd = inotify_init();
        if (fd < 0) {
            logger.Error("inotify_init 失败: %s", strerror(errno));
            return false;
        }
        int wd = inotify_add_watch(fd, dirPath, mask | IN_MOVED_TO);
        if (wd < 0) {
            logger.Error("inotify_add_watch 失败 %s: %s", dirPath, strerror(errno));
            close(fd);
            return false;
        }

        constexpr int buflen = sizeof(struct inotify_event) + NAME_MAX + 1;
        char buf[buflen];
        fd_set readfds;
        bool triggered = false;
        while (!triggered) {
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);
            struct timeval tv = {5, 0}; // 5s timeout to re-create watch
            int ret = select(fd + 1, &readfds, nullptr, nullptr, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) continue; // timeout, re-create watch
            int len = read(fd, buf, buflen);
            if (len < 0) break;
            // Check events for target file
            for (char* ptr = buf; ptr < buf + len; ) {
                struct inotify_event* ev = reinterpret_cast<struct inotify_event*>(ptr);
                if (ev->len > 0 && strcmp(ev->name, targetFile) == 0) {
                    triggered = true; break;
                }
                ptr += sizeof(struct inotify_event) + ev->len;
            }
        }
        inotify_rm_watch(fd, wd);
        close(fd);
        return triggered;
    }

    void cpuSetTriggerTask() {
        if (!LaunchBoost::enable) return;
        constexpr int TRIGGER_BUF_SIZE = 8192;
        sleep(1);

        while (true) {
            const int fd = inotify_init();
            if (fd < 0) {
                fprintf(stderr, "同步事件: 0xB1 (1/3)失败: [%d]:[%s]", errno, strerror(errno));
                exit(-1);
            }

            const int wd = inotify_add_watch(fd, cpusetEventPath, IN_ALL_EVENTS);
            
            if (wd < 0) {
                fprintf(stderr, "同步事件: 0xB1 (2/3)失败: [%d]:[%s]", errno, strerror(errno));
                exit(-1);
            }

            logger.Info("监听顶层应用切换事件成功");

            char buf[TRIGGER_BUF_SIZE];
            while (read(fd, buf, TRIGGER_BUF_SIZE) > 0) {
                cpuBoost = true;
                release();
                logger.Debug("前台进程已切换 已触发LaunchBoost");
                utils.sleep_ms(500); // 防抖
            }

            inotify_rm_watch(fd, wd);
            close(fd);

        }
    }

    // ============================================================
    //  v4.1 场景识别线程组
    // ============================================================

    // 周期 tick — 处理场景超时（每 500ms 一次）
    void sceneTickTask() {
        sleep(2);
        while (true) {
            utils.sleep_ms(500);
            if (scene_.tickTimeout()) {
                applyWithProfile(scene_.current());
                logger.Info("场景超时回退: %s", SceneDetector::name(scene_.current()));
            }
        }
    }

    // 屏幕状态检测
    //   v4.2: 优先使用 Netlink uevent（零轮询），失败时 fallback 到 property 轮询
    void screenStateTask() {
        sleep(3);

        // v4.2: 尝试 Netlink uevent 方案
        if (Config::NetlinkCfg::enable) {
            netlinkScreenOk = netlinkScreenLoop();
            if (netlinkScreenOk) {
                logger.Info("Netlink uevent 屏幕监听已启动，替代轮询方案");
                return; // Netlink 线程已接管，不再进入轮询
            }
            logger.Warn("Netlink uevent 初始化失败，fallback 到轮询方案");
        }

        // Fallback: 轮询 debug.tracing.screen_state property
        int lastState = -2;
        while (true) {
            int s = utils.getScreenProperty();
            int normalized = (s == 0) ? 0 : 1;     // -1 视为亮（保守）
            if (normalized != lastState) {
                lastState = normalized;
                bool changed = scene_.triggerScreen(normalized == 1);
                if (changed) {
                    applyWithProfile(scene_.current());
                    logger.Info("屏幕状态(poll): %s -> 场景: %s",
                                normalized ? "亮" : "灭",
                                SceneDetector::name(scene_.current()));
                }
            }
            // 熄屏时 poll 间隔加倍（少做轮询、省电）
            int interval = (normalized == 0) ? Config::SceneCfg::screen_poll_interval_ms * 2
                                              : Config::SceneCfg::screen_poll_interval_ms;
            utils.sleep_ms(interval);
        }
    }

    // CPU 负载采样 — 触发 HeavyLoad
    //   读 /proc/stat 全核累计 jiffies，与上次差值算 CPU 利用率
    void loadSamplingTask() {
        sleep(4);
        unsigned long long lastTotal = 0, lastIdle = 0;
        bool first = true;

        while (true) {
            // 熄屏时整线程低频运行
            if (scene_.current() == SceneDetector::Scene::Standby) {
                utils.sleep_ms(5000);
                continue;
            }

            unsigned long long total = 0, idle = 0;
            if (readProcStat(total, idle)) {
                if (!first) {
                    unsigned long long dTotal = total - lastTotal;
                    unsigned long long dIdle  = idle - lastIdle;
                    if (dTotal > 0) {
                        int loadPct = (int)((dTotal - dIdle) * 100 / dTotal);
                        if (loadPct < 0) loadPct = 0;
                        if (loadPct > 100) loadPct = 100;
                        if (scene_.feedCpuLoad(loadPct)) {
                            applyWithProfile(scene_.current());
                            logger.Info("负载 %d%% -> 场景: %s", loadPct,
                                        SceneDetector::name(scene_.current()));
                        }
                    }
                }
                lastTotal = total;
                lastIdle  = idle;
                first = false;
            }
            utils.sleep_ms(Config::SceneCfg::load_sample_interval_ms);
        }
    }

    bool readProcStat(unsigned long long& outTotal, unsigned long long& outIdle) {
        int fd = open("/proc/stat", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        char buf[256] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return false;
        // 行首 "cpu  user nice system idle iowait irq softirq steal"
        unsigned long long u=0, ni=0, sy=0, id=0, io=0, ir=0, so=0, st=0;
        // 跳过 "cpu " 4 个字符
        const char* p = strchr(buf, ' ');
        if (!p) return false;
        if (sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu",
                   &u, &ni, &sy, &id, &io, &ir, &so, &st) < 4) return false;
        outIdle = id + io;
        outTotal = u + ni + sy + id + io + ir + so + st;
        return true;
    }

    // 触摸事件检测（参考 uperf 的 Touch hint）
    //   读 /dev/input/event* 上的 EV_KEY/BTN_TOUCH 和 EV_ABS/ABS_MT_*
    //   原理：手指接触屏幕时会产生 input_event，select() 直接阻塞，零轮询
    //   去抖：连续触摸只在场景没切换时刷新（triggerTouch 内部判断）
    void touchDetectTask() {
        sleep(5);

        while (true) {
            // 熄屏时关闭触摸监听（手指碰屏幕也不应触发频率提升）
            while (scene_.current() == SceneDetector::Scene::Standby) {
                sleep(2);
            }

            std::vector<int> fds = openTouchDevices();
            if (fds.empty()) {
                logger.Warn("未找到触摸输入设备 — 触摸场景禁用");
                return;
            }

            fd_set rfds;
            int maxFd = 0;
            for (int fd : fds) if (fd > maxFd) maxFd = fd;

            // select 循环
            while (true) {
                if (scene_.current() == SceneDetector::Scene::Standby) break;

                FD_ZERO(&rfds);
                for (int fd : fds) FD_SET(fd, &rfds);
                struct timeval tv = {2, 0};
                int ret = select(maxFd + 1, &rfds, nullptr, nullptr, &tv);
                if (ret < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (ret == 0) continue;

                bool touched = false;
                struct input_event ev[16];
                for (int fd : fds) {
                    if (!FD_ISSET(fd, &rfds)) continue;
                    ssize_t bytes = read(fd, ev, sizeof(ev));
                    if (bytes < (ssize_t)sizeof(struct input_event)) continue;
                    int count = bytes / sizeof(struct input_event);
                    for (int i = 0; i < count; i++) {
                        // BTN_TOUCH / ABS_MT_TRACKING_ID / ABS_MT_POSITION_*
                        if (ev[i].type == EV_KEY && ev[i].code == BTN_TOUCH) { touched = true; break; }
                        if (ev[i].type == EV_ABS &&
                            (ev[i].code == ABS_MT_TRACKING_ID ||
                             ev[i].code == ABS_MT_POSITION_X ||
                             ev[i].code == ABS_MT_POSITION_Y)) { touched = true; break; }
                    }
                    if (touched) break;
                }

                if (touched) {
                    if (scene_.triggerTouch()) {
                        applyWithProfile(scene_.current());
                        logger.Debug("触摸 -> 场景: Touch");
                    }
                    // 触摸节流 — 200ms 内不再处理
                    utils.sleep_ms(200);
                }
            }

            // 退出 select 循环 — 关 fd 并稍后重试（可能是熄屏）
            for (int fd : fds) close(fd);
            sleep(1);
        }
    }

    std::vector<int> openTouchDevices() {
        std::vector<int> fds;
        for (int i = 0; i < Config::SceneCfg::input_dev_max; i++) {
            char path[64];
            FastSnprintf(path, sizeof(path), "/dev/input/event%d", i);
            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            // 探测是否为触摸设备：查 EVIOCGBIT
            unsigned long evbit[(EV_MAX + 7) / 8 / sizeof(long) + 1] = {0};
            if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
                close(fd); continue;
            }
            // 只要支持 EV_ABS 或 EV_KEY 之一即纳入（绝大多数触摸屏走 EV_ABS）
            bool hasAbs = (evbit[EV_ABS / (8 * sizeof(long))] >> (EV_ABS % (8 * sizeof(long)))) & 1;
            bool hasKey = (evbit[EV_KEY / (8 * sizeof(long))] >> (EV_KEY % (8 * sizeof(long)))) & 1;
            if (!hasAbs && !hasKey) { close(fd); continue; }
            // 进一步过滤：必须支持 ABS_MT_POSITION_X 或 BTN_TOUCH
            unsigned long keybit[(KEY_MAX + 7) / 8 / sizeof(long) + 1] = {0};
            unsigned long absbit[(ABS_MAX + 7) / 8 / sizeof(long) + 1] = {0};
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit);
            bool isTouch =
                ((keybit[BTN_TOUCH / (8 * sizeof(long))] >> (BTN_TOUCH % (8 * sizeof(long)))) & 1) ||
                ((absbit[ABS_MT_POSITION_X / (8 * sizeof(long))] >> (ABS_MT_POSITION_X % (8 * sizeof(long)))) & 1);
            if (!isTouch) { close(fd); continue; }
            fds.push_back(fd);
            logger.Info("触摸设备已打开: %s", path);
        }
        return fds;
    }

    // ============================================================
    //  v4.2 Netlink uevent 屏幕状态监听
    //
    //  原理：监听 NETLINK_KOBJECT_UEVENT，过滤包含 "display"、"panel"、
    //        "lcd"、"backlight" 等关键字的 uevent 事件
    //  屏幕状态判断：
    //    - 收到 uevent 后读一次 debug.tracing.screen_state 确认
    //    - 或通过 uevent 内容中的 POWER_STATE=ON/OFF 判断
    //  优势：零轮询，屏幕状态变化时内核主动推送
    // ============================================================
    bool netlinkScreenLoop() {
        int sockFd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
        if (sockFd < 0) {
            logger.Error("Netlink socket 创建失败: %s", strerror(errno));
            return false;
        }

        // 绑定到 NETLINK_KOBJECT_UEVENT
        struct sockaddr_nl addr;
        memset(&addr, 0, sizeof(addr));
        addr.nl_family = AF_NETLINK;
        addr.nl_groups = 1;  // 接收内核广播
        addr.nl_pid = getpid();

        if (bind(sockFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            logger.Error("Netlink bind 失败: %s", strerror(errno));
            close(sockFd);
            return false;
        }

        // 设置接收缓冲区
        int rcvbuf = 256 * 1024;
        setsockopt(sockFd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        logger.Info("Netlink uevent socket 已创建，开始监听屏幕事件");

        constexpr int UEVENT_BUF_SIZE = 4096;
        char buf[UEVENT_BUF_SIZE];
        int lastScreenState = -2;
        struct timeval tv;

        while (true) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(sockFd, &rfds);
            tv.tv_sec = 5;  // 5s 超时，定期检查
            tv.tv_usec = 0;

            int ret = select(sockFd + 1, &rfds, nullptr, nullptr, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                logger.Error("Netlink select 错误: %s", strerror(errno));
                break;
            }
            if (ret == 0) continue; // 超时，继续等待

            ssize_t len = recv(sockFd, buf, UEVENT_BUF_SIZE - 1, 0);
            if (len <= 0) continue;
            buf[len] = '\0';

            // 过滤：只处理包含屏幕相关关键字的 uevent
            // 常见路径模式：
            //   change@/devices/platform/soc/<mdss>/dsi-display-*/display0
            //   change@/devices/virtual/panel/panel0
            //   change@/devices/platform/<soc>/backlight/...
            if (!strstr(buf, "display") && !strstr(buf, "panel") &&
                !strstr(buf, "lcd") && !strstr(buf, "backlight") &&
                !strstr(buf, "drm")) {
                continue;
            }

            // 收到屏幕相关 uevent，读取当前屏幕状态确认
            int s = utils.getScreenProperty();
            int normalized = (s == 0) ? 0 : 1;

            if (normalized != lastScreenState) {
                lastScreenState = normalized;
                bool changed = scene_.triggerScreen(normalized == 1);
                if (changed) {
                    applyWithProfile(scene_.current());
                    logger.Info("屏幕状态(netlink): %s -> 场景: %s",
                                normalized ? "亮" : "灭",
                                SceneDetector::name(scene_.current()));
                }
            }
        }

        close(sockFd);
        return false;
    }

    // ============================================================
    //  v4.2 应用画像监控线程 — 场景分类画像
    //
    //  参考 Way_Balance 设计：
    //  - 通过 dumpsys window 获取当前前台应用包名
    //  - 使用通配符匹配场景画像（* 匹配任意序列，? 匹配单个字符）
    //  - 按 models 数组顺序匹配，第一个匹配的场景生效
    //
    //  与 cpuSetTriggerTask（inotify top-app cgroup）互补：
    //    - cpuSetTriggerTask: 快速检测应用切换（触发 AmSwitch 场景）
    //    - appProfileTask: 获取包名并匹配场景画像（频率策略）
    // ============================================================
    void appProfileTask() {
        sleep(5); // 等待系统启动完成

        logger.Info("场景画像监控线程已启动 (dumpsys 间隔 3s, 支持通配符匹配)");

        while (true) {
            // 熄屏时降低检测频率
            if (scene_.current() == SceneDetector::Scene::Standby) {
                utils.sleep_ms(8000);   // v4.3: 5s -> 8s
                continue;
            }

            // [Fix v4.3] 用 cached 版本，避免短时间内连续 dumpsys（cpuSetTriggerTask 触发刚 dumpsys 完，这里又一遍很浪费）
            std::string pkg = utils.getTopAppCached(2500);
            if (pkg.empty() || pkg == "null") {
                utils.sleep_ms(3000);
                continue;
            }

            // 包名未变化，跳过
            if (pkg == lastTopApp) {
                utils.sleep_ms(3000);   // v4.3: 2s -> 3s (省电)
                continue;
            }

            // 包名变化，使用通配符查找匹配的场景画像
            lastTopApp = pkg;
            int newMatch = Config::AppProfile::findMatchingModel(pkg.c_str());

            // [Fix v4.1] currentMatch 是 atomic
            int oldMatch = Config::AppProfile::currentMatch.load();
            if (newMatch != oldMatch) {
                Config::AppProfile::currentMatch.store(newMatch);
                if (newMatch >= 0) {
                    logger.Info("前台应用切换: %s -> 匹配场景 '%s' (isGame=%d)",
                                pkg.c_str(),
                                Config::AppProfile::Models[newMatch].modelName.c_str(),
                                Config::AppProfile::Models[newMatch].isGame ? 1 : 0);
                } else {
                    logger.Info("前台应用切换: %s -> 无匹配场景画像", pkg.c_str());
                }
                // 立即应用新的频率策略
                applyWithProfile(scene_.current());
                // 注意：核心开关已在 applyAppProfile / restoreBaseCoresIfNeeded 内部处理
            }

            utils.sleep_ms(3000);   // v4.3: 2s -> 3s (省电)
        }
    }

    void Init() {
        const int myPid = getpid();
        const char* checkNames[] = {"CoreTurboScheduler", "CpuTurboScheduler", "MW_CpuSpeedController"};

        // Step 1: Graceful termination (SIGTERM) -- exclude current PID
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        kill(pid, SIGTERM);
                        logger.Debug("发送 SIGTERM 到 %s (pid: %d)", name, pid);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }
        sleep(1); // Allow graceful shutdown

        // Step 2: Force kill (SIGKILL) -- exclude current PID
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        kill(pid, SIGKILL);
                        logger.Debug("发送 SIGKILL 到 %s (pid: %d)", name, pid);
                    }
                    token = strtok(nullptr, " \t\n");
                }
            }
        }
        sleep(1); // Allow force kill to take effect

        // Step 3: Final check -- if any other instance still running, exit
        for (const char* name : checkNames) {
            char cmd[64];
            char buf[256] = { 0 };
            FastSnprintf(cmd, sizeof(cmd), "pidof %s", name);
            size_t pidLen = utils.popenRead(cmd, buf, sizeof(buf));
            if (pidLen > 0) {
                char* token = strtok(buf, " \t\n");
                while (token != nullptr) {
                    int pid = atoi(token);
                    if (pid > 0 && pid != myPid) {
                        logger.Error("CTS调度已经在运行(%s pid: %d), 当前进程(pid:%d)即将退出", name, pid, myPid);
                        printf("\n!!! \n!!! CTS调度已经在运行(%s pid: %d), 当前进程(pid:%d)即将退出 \n!!!\n\n", name, pid, myPid);
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
        logger.Info("日志等级: %s", Meta::loglevel.c_str());
        function.AllFunC();   // [Fix v4.3] AllFunC -> startGuards() 已经会启 GPU 守护线程
        if (configOk) {
            release();
            online();
            SchedParam();
            function.gpuFreqControl();
        } else {
            logger.Warn("初始配置加载失败，尝试使用默认配置");
        }
        // [Fix v4.3] 删掉这里第二次启动 gpuFreqGuard 的代码（与 AllFunC 重复，会跑两条守护线程）
    }
};
