#pragma once

#include "Json/json.h"
#include "Config.hpp"
#include "Logger.hpp"
#include <stdexcept>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <dirent.h>
#include <sys/system_properties.h>

using namespace Config;
using namespace qlib;

class ConfigManager {
public:
    // 配置版本信息
    struct ConfigVersion {
        int version = 0;
        std::string source;     // "json", "ini", "json+ini"
        std::string timestamp;  // 加载时间
    };

    SchedParam schedParam[4];
    std::string mode;

private:
    static constexpr const char* configPath = "/sdcard/Android/CTS/config.json";
    static constexpr const char* modePath = "/sdcard/Android/CTS/mode.txt";
    static constexpr const char* backupPath = "/sdcard/Android/CTS/config.bak.json";
    static constexpr const char* iniDir = "/sdcard/Android/CTS/conf";

    mutable Logger logger;
    json_view_t json;

    char buff[256];
    char cluster[64];

    // 版本控制
    ConfigVersion currentVersion_;
    ConfigVersion previousVersion_;  // 用于回滚
    std::string lastJsonSnapshot_;   // 上一次成功的 JSON 快照（用于回滚）

    // INI 解析结果缓存
    struct IniSection {
        std::string name;
        std::vector<std::pair<std::string, std::string>> entries;
    };
    std::vector<IniSection> iniSections_;

public:
    ConfigManager() = default;

    // === 兼容原 JsonConfig 的公共接口 ===

    void LoadConfig() {
        ifstream file;
        std::string temp;
        file.open(modePath);
        if (!file.is_open()) {
            fprintf(stderr, "无法打开配置文件: %s\n", modePath);
            return;
        }

        getline(file, temp);
        // Trim trailing whitespace and newlines
        size_t end = temp.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) {
            temp.erase(end + 1);
        } else {
            temp.clear();
        }
        mode = std::move(temp);

        file.close();
    }

    bool switchConfig() const {
        if (mode == "powersave" || mode == "balance" || mode == "performance" || mode == "fast") return true;

        return false;
    }

    bool readConfig() {
        // 1. 保存当前快照（用于回滚）
        saveSnapshot();

        // 2. 清理所有配置到安全空状态
        clearAllConfig();

        // 3. 读取 config.json 文本
        ifstream ifs(configPath, std::ios::binary);
        if (!ifs) {
            logger.Error("无法打开json配置文件");
            return false;
        }

        std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        // 4. 解析 JSON
        int result = json::parse(&json, text.data(), text.data() + text.size());
        if (result != 0) {
            logger.Error("解析json配置文件失败 错误: %d", result);
            // 尝试回滚
            logger.Warn("JSON解析失败，尝试回滚到上次配置...");
            if (!lastJsonSnapshot_.empty()) {
                bool rb = rollback();
                if (rb) return true;
            }
            return false;
        }

        // 5. 解析各节点（与原 JsonConfig 完全一致的逻辑）
        if (!parseMeta()) {
            logger.Warn("Meta节点解析异常，使用默认值继续");
            // parseMeta 异常不阻断流程，与原 JsonConfig 一致
        }

        if (!parseFunction()) {
            logger.Warn("Function节点解析异常，使用默认值继续");
            // parseFunction 异常不阻断流程，与原 JsonConfig 一致
        }

        // 6. 加载 mode.txt
        LoadConfig();

        if (mode.empty()) {
            logger.Error("情景模式为空 无法读取数据");
            return false;
        }

        if (!switchConfig()) {
            logger.Error("情景模式异常 当前情景模式: '%s'", mode.c_str());
            return false;
        }

        // 7. 解析 Switch 节点
        if (!parseSwitch()) {
            logger.Error("Switch节点解析失败");
            // 尝试回滚
            logger.Warn("Switch节点解析失败，尝试回滚到上次配置...");
            if (!lastJsonSnapshot_.empty()) {
                bool rb = rollback();
                if (rb) return true;
            }
            return false;
        }

        // 8. 检查是否有匹配的 INI 文件可合并
        bool iniMerged = false;
        if (autoImportIni()) {
            iniMerged = true;
        }

        // 9. 记录版本信息
        currentVersion_.version = Meta::version;
        currentVersion_.source = iniMerged ? "json+ini" : "json";
        currentVersion_.timestamp = getCurrentTimestamp();

        logger.Info("配置加载成功 来源: %s 版本: %d 时间: %s",
            currentVersion_.source.c_str(),
            currentVersion_.version,
            currentVersion_.timestamp.c_str());

        return true;
    }

    // === 新增功能 ===

    // 从 INI 文件导入预设并合并到当前 JSON 配置
    bool importIni(const char* iniFilePath) {
        if (!iniFilePath || iniFilePath[0] == '\0') {
            logger.Error("INI文件路径为空");
            return false;
        }

        logger.Info("开始导入INI预设文件: %s", iniFilePath);

        // 解析 INI 文件
        if (!parseIniFile(iniFilePath)) {
            logger.Error("INI文件解析失败: %s", iniFilePath);
            return false;
        }

        // 将 INI 数据合并到 JSON
        mergeIniToConfig();

        logger.Info("INI预设文件导入成功: %s", iniFilePath);
        return true;
    }

    // 自动检测设备 SoC 并导入匹配的 INI 预设
    bool autoImportIni() {
        std::string socModel = detectSocModel();
        if (socModel.empty()) {
            logger.Debug("无法检测SoC型号，跳过INI自动导入");
            return false;
        }

        logger.Info("检测到SoC型号: %s", socModel.c_str());

        // 在 iniDir 中查找匹配的 INI 文件
        std::string matchedIni = findMatchingIni(socModel);
        if (matchedIni.empty()) {
            logger.Debug("未找到匹配的INI文件: %s", socModel.c_str());
            return false;
        }

        return importIni(matchedIni.c_str());
    }

    // 获取当前配置版本
    const ConfigVersion& getVersion() const {
        return currentVersion_;
    }

    // 回滚到上一次成功的配置
    bool rollback() {
        if (lastJsonSnapshot_.empty()) {
            logger.Error("没有可用的回滚快照");
            return false;
        }

        logger.Warn("正在回滚到上次配置...");

        // 将快照写回 config.json
        std::ofstream ofs(backupPath, std::ios::binary);
        if (!ofs) {
            logger.Error("无法创建备份文件: %s", backupPath);
            return false;
        }
        ofs << lastJsonSnapshot_;
        ofs.close();

        // 将快照写回 config.json
        std::ofstream cfgOfs(configPath, std::ios::binary | std::ios::trunc);
        if (!cfgOfs) {
            logger.Error("无法写回配置文件: %s", configPath);
            return false;
        }
        cfgOfs << lastJsonSnapshot_;
        cfgOfs.close();

        // 重新加载
        previousVersion_ = currentVersion_;
        clearAllConfig();

        ifstream ifs(configPath, std::ios::binary);
        if (!ifs) {
            logger.Error("回滚后无法打开配置文件");
            return false;
        }

        std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

        int result = json::parse(&json, text.data(), text.data() + text.size());
        if (result != 0) {
            logger.Error("回滚后JSON解析失败 错误: %d", result);
            return false;
        }

        parseMeta();
        parseFunction();
        LoadConfig();

        if (mode.empty() || !switchConfig()) {
            logger.Error("回滚后情景模式异常");
            return false;
        }

        if (!parseSwitch()) {
            logger.Error("回滚后Switch节点解析失败");
            return false;
        }

        currentVersion_.version = Meta::version;
        currentVersion_.source = "rollback";
        currentVersion_.timestamp = getCurrentTimestamp();

        logger.Info("回滚成功 版本: %d 时间: %s",
            currentVersion_.version,
            currentVersion_.timestamp.c_str());

        return true;
    }

    // 获取配置快照（用于调试/导出）
    std::string getSnapshot() const {
        try {
            string_t out(4096u);
            json.to(out);
            return std::string(out.data(), out.size());
        } catch (const qlib::exception& e) {
            logger.Error("获取快照失败: %s", e.what());
            return "";
        }
    }

private:
    // === JSON 解析（保留原有逻辑，与 JsonConfig 完全一致） ===

    bool parseJson(const std::string& text) {
        int result = json::parse(&json, text.data(), text.data() + text.size());
        if (result != 0) {
            logger.Error("解析json配置文件失败 错误: %d", result);
            return false;
        }
        return true;
    }

    bool parseMeta() {
        try {
            Meta::name = json["meta"]["name"].get<string_t>();
            Meta::version = json["meta"]["version"].get<int>();
            Meta::author = json["meta"]["author"].get<string_t>();
            Meta::loglevel = json["meta"]["loglevel"].get<string_t>();

            #if DEBUG_DURATION
                logger.Debug("---------源信息---------");
                logger.Info("名称: %s", Meta::name.c_str());
                logger.Info("版本: %d", Meta::version);
                logger.Info("作者: %s", Meta::author.c_str());
                logger.Info("日志等级: %s", Meta::loglevel.c_str());
                logger.Info("---------CPU簇---------");
            #endif

            for (int i = 0; i <= 3; i++) {
                FastSnprintf(buff, sizeof(buff), "c%d", i);
                if (json["Policy"][buff].get<int>() == 255) continue;
                Policy::CpuPolicy[i] = json["Policy"][buff].get<int>();
                #if DEBUG_DURATION
                    logger.Debug("CPU簇 %d 的值为: %d", i, Policy::CpuPolicy[i]);
                #endif
            }
        } catch (const qlib::exception& e) {
            logger.Error("Meta节点异常 错误消息: %s", e.what());
            return false;
        }
        return true;
    }

    bool parseFunction() {
        try {
            #if DEBUG_DURATION
                logger.Debug("---------附加功能---------");
            #endif

            auto& Cpuset = json["Function"]["Cpuset"];
            Cpuset::enable = Cpuset["enable"].get<bool>();
            Cpuset::top_app = Cpuset["top_app"].get<string_t>();
            Cpuset::foreground = Cpuset["foreground"].get<string_t>();
            Cpuset::background = Cpuset["background"].get<string_t>();
            Cpuset::system_background = Cpuset["system_background"].get<string_t>();
            Cpuset::restricted = Cpuset["restricted"].get<string_t>();

            #if DEBUG_DURATION
                logger.Debug("Cpuset 开关: %s", Cpuset::enable ? "开启" : "关闭");
                logger.Debug("top_app: %s", Cpuset::top_app.c_str());
                logger.Debug("foreground: %s", Cpuset::foreground.c_str());
                logger.Debug("background: %s", Cpuset::background.c_str());
                logger.Debug("system_background: %s", Cpuset::system_background.c_str());
                logger.Debug("restricted: %s", Cpuset::restricted.c_str());
            #endif

            auto& LaunchBoost = json["Function"]["LaunchBoost"];
            LaunchBoost::enable = LaunchBoost["enable"].get<bool>();
            LaunchBoost::boost_rate_limit_ms = LaunchBoost["boost_rate_limit_ms"].get<int>();
            try { LaunchBoost::boost_duration_ms = LaunchBoost["boost_duration_ms"].get<int>(); } catch (...) {}
            for (int i = 0; i <= 3; i++) {
                FastSnprintf(buff, sizeof(buff), "c%d", i);
                auto& BoostFreq = LaunchBoost::BoostFreq[i] = LaunchBoost["BoostFreq"][buff].get<string_t>();
                if (BoostFreq.empty()) continue;

                #if DEBUG_DURATION
                    logger.Debug("LaunchBoost开关: %s", LaunchBoost::enable ? "开启" : "关闭");
                    logger.Debug("LaunchBoost升频持续时间: %d", LaunchBoost::boost_rate_limit_ms);
                    logger.Debug("LaunchBoost频率: %s", LaunchBoost::BoostFreq[i].c_str());
                #endif
            }

            auto& officialMode = json["Function"]["OfficialMode"];
            OfficialMode::enable = officialMode["enable"].get<bool>();
            #if DEBUG_DURATION
                logger.Debug("OfficialMode 开关: %s", OfficialMode::enable ? "开启" : "关闭");
            #endif

            auto& LoadBalancing = json["Function"]["LoadBalancing"];
            LoadBanlace::enable = LoadBalancing["enable"].get<bool>();

            #if DEBUG_DURATION
                logger.Debug("LoadBalancing 开关: %s", LoadBanlace::enable ? "开启" : "关闭");
            #endif

            auto& Scheduler = json["Function"]["Scheduler"];
            Scheduler::enable = Scheduler["enable"].get<bool>();
            Scheduler::Sched_energy_aware = Scheduler["sched_energy_aware"].get<bool>();
            Scheduler::Sched_schedstats = Scheduler["sched_schedstats"].get<bool>();
            Scheduler::Sched_latency_ns = Scheduler["sched_latency_ns"].get<string_t>();
            Scheduler::Sched_migration_cost_ns = Scheduler["sched_migration_cost_ns"].get<string_t>();
            Scheduler::Sched_min_granularity_ns = Scheduler["sched_min_granularity_ns"].get<string_t>();
            Scheduler::Sched_wakeup_granularity_ns = Scheduler["sched_wakeup_granularity_ns"].get<string_t>();
            Scheduler::Sched_nr_migrate = Scheduler["sched_nr_migrate"].get<string_t>();
            Scheduler::Sched_util_clamp_min = Scheduler["sched_util_clamp_min"].get<string_t>();
            Scheduler::Sched_util_clamp_max = Scheduler["sched_util_clamp_max"].get<string_t>();
            // 新增CFS调度参数解析
            try { Scheduler::Sched_child_runs_first = Scheduler["sched_child_runs_first"].get<string_t>(); } catch (...) {}
            try { Scheduler::Sched_tunable_scaling = Scheduler["sched_tunable_scaling"].get<string_t>(); } catch (...) {}
            try { Scheduler::Sched_sched_compat_yield = Scheduler["sched_sched_compat_yield"].get<string_t>(); } catch (...) {}
            try { Scheduler::Sched_wakeup_load_threshold = Scheduler["sched_wakeup_load_threshold"].get<string_t>(); } catch (...) {}
            try { Scheduler::Sched_migration_cost = Scheduler["sched_migration_cost"].get<string_t>(); } catch (...) {}
            try { Scheduler::Sched_numa_balancing = Scheduler["sched_numa_balancing"].get<bool>(); } catch (...) {}
            try { Scheduler::Sched_numa_preferred_nid = Scheduler["sched_numa_preferred_nid"].get<string_t>(); } catch (...) {}

            #if DEBUG_DURATION
                logger.Debug("Scheduler 总开关: %s", Scheduler::enable ? "开启" : "关闭");
                logger.Debug("Sched_energy_aware: %s", Scheduler::Sched_energy_aware ? "true" : "false");
                logger.Debug("Sched_schedstats: %s", Scheduler::Sched_schedstats ? "true" : "false");
                logger.Debug("Sched_latency_ns: %s", Scheduler::Sched_latency_ns.c_str());
                logger.Debug("Sched_migration_cost_ns: %s", Scheduler::Sched_migration_cost_ns.c_str());
                logger.Debug("Sched_min_granularity_ns: %s", Scheduler::Sched_min_granularity_ns.c_str());
                logger.Debug("Sched_wakeup_granularity_ns: %s", Scheduler::Sched_wakeup_granularity_ns.c_str());
                logger.Debug("Sched_nr_migrate: %s", Scheduler::Sched_nr_migrate.c_str());
                logger.Debug("Sched_util_clamp_min: %s", Scheduler::Sched_util_clamp_min.c_str());
                logger.Debug("Sched_util_clamp_max: %s", Scheduler::Sched_util_clamp_max.c_str());
            #endif

            auto& IOSched = json["Function"]["IOScheduler"];
            IOScheduler::enable = IOSched["enable"].get<bool>();
            IOScheduler::scheduler = IOSched["scheduler"].get<string_t>();
            IOScheduler::read_ahead_kb = IOSched["read_ahead_kb"].get<string_t>();
            IOScheduler::nr_requests = IOSched["nr_requests"].get<string_t>();

            #if DEBUG_DURATION
                logger.Debug("IOScheduler 总开关: %s", IOScheduler::enable ? "开启" : "关闭");
                logger.Debug("IOScheduler scheduler: %s", IOScheduler::scheduler.c_str());
                logger.Debug("IOScheduler read_ahead_kb: %s", IOScheduler::read_ahead_kb.c_str());
                logger.Debug("IOScheduler nr_requests: %s", IOScheduler::nr_requests.c_str());
            #endif

            auto& GpuFreq = json["Function"]["GpuFreq"];
            GpuFreq::enable = GpuFreq["enable"].get<bool>();

            #if DEBUG_DURATION
                logger.Debug("GpuFreq 总开关: %s", GpuFreq::enable ? "开启" : "关闭");
            #endif

            auto& StuneConfig = json["Function"]["Stune"];
            Stune::enable = StuneConfig["enable"].get<bool>();
            Stune::top_app_boost = StuneConfig["top_app_boost"].get<string_t>();
            Stune::foreground_boost = StuneConfig["foreground_boost"].get<string_t>();
            Stune::background_boost = StuneConfig["background_boost"].get<string_t>();
            Stune::top_app_prefer_idle = StuneConfig["top_app_prefer_idle"].get<string_t>();
            Stune::foreground_prefer_idle = StuneConfig["foreground_prefer_idle"].get<string_t>();
            // Uclamp参数解析
            try { Stune::top_app_uclamp_min = StuneConfig["top_app_uclamp_min"].get<string_t>(); } catch (...) {}
            try { Stune::top_app_uclamp_max = StuneConfig["top_app_uclamp_max"].get<string_t>(); } catch (...) {}
            try { Stune::foreground_uclamp_min = StuneConfig["foreground_uclamp_min"].get<string_t>(); } catch (...) {}
            try { Stune::foreground_uclamp_max = StuneConfig["foreground_uclamp_max"].get<string_t>(); } catch (...) {}
            try { Stune::background_uclamp_min = StuneConfig["background_uclamp_min"].get<string_t>(); } catch (...) {}
            try { Stune::background_uclamp_max = StuneConfig["background_uclamp_max"].get<string_t>(); } catch (...) {}
            // 补充prefer_idle解析
            try { Stune::background_prefer_idle = StuneConfig["background_prefer_idle"].get<string_t>(); } catch (...) {}
            try { Stune::system_background_prefer_idle = StuneConfig["system_background_prefer_idle"].get<string_t>(); } catch (...) {}
            try { Stune::restricted_prefer_idle = StuneConfig["restricted_prefer_idle"].get<string_t>(); } catch (...) {}

            #if DEBUG_DURATION
                logger.Debug("Stune 总开关: %s", Stune::enable ? "开启" : "关闭");
                logger.Debug("Stune top_app_boost: %s", Stune::top_app_boost.c_str());
                logger.Debug("Stune foreground_boost: %s", Stune::foreground_boost.c_str());
                logger.Debug("Stune background_boost: %s", Stune::background_boost.c_str());
                logger.Debug("Stune top_app_prefer_idle: %s", Stune::top_app_prefer_idle.c_str());
                logger.Debug("Stune foreground_prefer_idle: %s", Stune::foreground_prefer_idle.c_str());
            #endif

            // === LoadPredict 负载预测（全局参数） ===
            auto& LpConfig = json["Function"]["LoadPredict"];
            LoadPredict::enable = LpConfig["enable"].get<bool>();
            LoadPredict::alpha = LpConfig["alpha"].get<double>();
            LoadPredict::sample_interval_ms = LpConfig["sample_interval_ms"].get<int>();
            LoadPredict::idle_threshold = LpConfig["idle_threshold"].get<double>();
            LoadPredict::heavy_threshold = LpConfig["heavy_threshold"].get<double>();
            LoadPredict::critical_threshold = LpConfig["critical_threshold"].get<double>();
            LoadPredict::buffer_count = LpConfig["buffer_count"].get<int>();
            LoadPredict::predict_thd = LpConfig["predict_thd"].get<double>();
            auto& effArray = LpConfig["efficiency"];
            for (int i = 0; i < 8; i++) {
                FastSnprintf(buff, sizeof(buff), "%d", i);
                try { LoadPredict::efficiency[i] = effArray[buff].get<int>(); } catch (...) {}
            }
            LoadPredict::alpha_up = LpConfig["alpha_up"].get<double>();
            LoadPredict::alpha_down = LpConfig["alpha_down"].get<double>();
            LoadPredict::cusum_k = LpConfig["cusum_k"].get<double>();
            LoadPredict::cusum_h = LpConfig["cusum_h"].get<double>();

            #if DEBUG_DURATION
                logger.Debug("LoadPredict 总开关: %s", LoadPredict::enable ? "开启" : "关闭");
                logger.Debug("LoadPredict alpha: %.2f", LoadPredict::alpha);
                logger.Debug("LoadPredict sample_interval_ms: %d", LoadPredict::sample_interval_ms);
                logger.Debug("LoadPredict idle_threshold: %.1f%%", LoadPredict::idle_threshold);
                logger.Debug("LoadPredict heavy_threshold: %.1f%%", LoadPredict::heavy_threshold);
                logger.Debug("LoadPredict critical_threshold: %.1f%%", LoadPredict::critical_threshold);
                logger.Debug("LoadPredict buffer_count: %d", LoadPredict::buffer_count);
            #endif

            // === ProcessPriority 进程优先级控制 ===
            auto& PrioConfig = json["Function"]["ProcessPriority"];
            ProcessPriority::enable = PrioConfig["enable"].get<bool>();
            ProcessPriority::foreground_nice = PrioConfig["foreground_nice"].get<string_t>();
            ProcessPriority::foreground_oom_score_adj = PrioConfig["foreground_oom_score_adj"].get<string_t>();
            ProcessPriority::background_nice = PrioConfig["background_nice"].get<string_t>();
            ProcessPriority::background_oom_score_adj = PrioConfig["background_oom_score_adj"].get<string_t>();
            ProcessPriority::system_nice = PrioConfig["system_nice"].get<string_t>();

            #if DEBUG_DURATION
                logger.Debug("ProcessPriority 总开关: %s", ProcessPriority::enable ? "开启" : "关闭");
                logger.Debug("ProcessPriority foreground_nice: %s", ProcessPriority::foreground_nice.c_str());
                logger.Debug("ProcessPriority background_nice: %s", ProcessPriority::background_nice.c_str());
            #endif

            // === Memory 内存优化 ===
            auto& MemConfig = json["Function"]["Memory"];
            Memory::enable = MemConfig["enable"].get<bool>();
            Memory::swappiness = MemConfig["swappiness"].get<string_t>();
            Memory::page_cluster = MemConfig["page_cluster"].get<string_t>();
            Memory::dirty_ratio = MemConfig["dirty_ratio"].get<string_t>();
            Memory::dirty_background_ratio = MemConfig["dirty_background_ratio"].get<string_t>();
            Memory::vfs_cache_pressure = MemConfig["vfs_cache_pressure"].get<string_t>();
            Memory::lmk_minfree_levels = MemConfig["lmk_minfree_levels"].get<string_t>();

            #if DEBUG_DURATION
                logger.Debug("Memory 总开关: %s", Memory::enable ? "开启" : "关闭");
                logger.Debug("Memory swappiness: %s", Memory::swappiness.c_str());
                logger.Debug("Memory dirty_ratio: %s", Memory::dirty_ratio.c_str());
            #endif

            // === SchedDomain 调度域优化 ===
            auto& SdConfig = json["Function"]["SchedDomain"];
            SchedDomain::enable = SdConfig["enable"].get<bool>();
            SchedDomain::sd0_flags = SdConfig["sd0_flags"].get<string_t>();
            SchedDomain::sd1_flags = SdConfig["sd1_flags"].get<string_t>();
            SchedDomain::sd2_flags = SdConfig["sd2_flags"].get<string_t>();

            #if DEBUG_DURATION
                logger.Debug("SchedDomain 总开关: %s", SchedDomain::enable ? "开启" : "关闭");
            #endif
        } catch (const qlib::exception& e) {
            logger.Error("Function节点异常: %s", e.what());
            return false;
        }
        return true;
    }

    bool parseSwitch() {
        try {
            logger.Info("当前性能模式: %s", mode.c_str());

            auto& Switch = json["Switch"][mode.c_str()];
            for (int i = 0; i <= 3; i++) {
                FastSnprintf(buff, sizeof(buff), "c%d", i);
                auto& MinFreq = Performances::MinFreq[i] = Switch["MinFreq"][buff].get<string_t>();
                auto& MaxFreq = Performances::MaxFreq[i] = Switch["MaxFreq"][buff].get<string_t>();
                auto& CpuGovernor = Performances::CpuGovernor[i] = Switch["governor"][buff].get<string_t>();
                if (MinFreq.empty() || MaxFreq.empty() || CpuGovernor.empty()) continue;
                logger.Info("CPU簇: %d 最小频率: %s 最大频率: %s 调速器: %s",
                    Policy::CpuPolicy[i], Performances::MinFreq[i].c_str(),
                        Performances::MaxFreq[i].c_str(), Performances::CpuGovernor[i].c_str());
            }

            for (int i = 0; i <= 7; i++) {
                FastSnprintf(buff, sizeof(buff), "Core%d", i);
                Performances::Online[i] = Switch["CoreOnline"][buff].get<int>();
                logger.Debug("核心: %d %s", i, Performances::Online[i] ? "开启" : "关闭");
            }

            for (int i = 0; i <= 3; i++) {
                FastSnprintf(cluster, sizeof(cluster), "c%d", i);
                try {
                    auto& sp = Switch["SchedParam"][cluster];
                    for (int j = 1; j <= 12; j++) {
                        FastSnprintf(buff, sizeof(buff), "Path%d", j);
                        auto name = sp[buff].get<string_t>();
                        if (name.empty()) continue;

                        FastSnprintf(buff, sizeof(buff), "value%d", j);
                        auto value = sp[buff].get<string_t>();
                        if (value.empty()) continue;

                        schedParam[i].Name[j] = name;
                        schedParam[i].Value[j] = value;
                    }
                } catch (...) {
                    // SchedParam cluster not found, skip
                }
            }

            auto& GpuFreqSwitch = Switch["GpuFreq"];
            GpuFreq::min_freq = GpuFreqSwitch["min_freq"].get<string_t>();
            GpuFreq::max_freq = GpuFreqSwitch["max_freq"].get<string_t>();

            logger.Info("GPU min_freq: %s  max_freq: %s",
                GpuFreq::min_freq.c_str(), GpuFreq::max_freq.c_str());

            // === 场景频率覆盖配置（idle/heavy/critical） ===
            // 先清空场景配置
            for (int i = 0; i <= 3; i++) {
                Performances::IdleMinFreq[i] = "";
                Performances::IdleMaxFreq[i] = "";
                Performances::HeavyMinFreq[i] = "";
                Performances::HeavyMaxFreq[i] = "";
                Performances::CriticalMaxFreq[i] = "";
            }

            // idle 场景（MinFreq + MaxFreq）
            try {
                auto& IdleSc = Switch["idle"];
                for (int i = 0; i <= 3; i++) {
                    FastSnprintf(buff, sizeof(buff), "c%d", i);
                    try { Performances::IdleMinFreq[i] = IdleSc["MinFreq"][buff].get<string_t>(); } catch (...) {}
                    try { Performances::IdleMaxFreq[i] = IdleSc["MaxFreq"][buff].get<string_t>(); } catch (...) {}
                }
            } catch (...) {
                logger.Debug("当前模式无 idle 场景配置，使用模式基础配置");
            }

            // heavy 场景（MinFreq + MaxFreq）
            try {
                auto& HeavySc = Switch["heavy"];
                for (int i = 0; i <= 3; i++) {
                    FastSnprintf(buff, sizeof(buff), "c%d", i);
                    try { Performances::HeavyMinFreq[i] = HeavySc["MinFreq"][buff].get<string_t>(); } catch (...) {}
                    try { Performances::HeavyMaxFreq[i] = HeavySc["MaxFreq"][buff].get<string_t>(); } catch (...) {}
                }
            } catch (...) {
                logger.Debug("当前模式无 heavy 场景配置，使用模式基础配置");
            }

            // critical 场景（只要 MaxFreq，拉满最大频率）
            try {
                auto& CritSc = Switch["critical"];
                for (int i = 0; i <= 3; i++) {
                    FastSnprintf(buff, sizeof(buff), "c%d", i);
                    try { Performances::CriticalMaxFreq[i] = CritSc["MaxFreq"][buff].get<string_t>(); } catch (...) {}
                }
            } catch (...) {
                logger.Debug("当前模式无 critical 场景配置，使用模式基础配置");
            }
        } catch (const qlib::exception& e) {
            logger.Error("情景模式异常: %s", e.what());
            return false;
        }

        return true;
    }

    // === INI 解析 ===

    // 去除字符串两端空白
    static std::string trim(const std::string& str) {
        size_t start = str.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = str.find_last_not_of(" \t\r\n");
        return str.substr(start, end - start + 1);
    }

    // 去除值两端的引号
    static std::string unquote(const std::string& str) {
        std::string trimmed = trim(str);
        if (trimmed.size() >= 2) {
            if ((trimmed.front() == '"' && trimmed.back() == '"') ||
                (trimmed.front() == '\'' && trimmed.back() == '\'')) {
                return trimmed.substr(1, trimmed.size() - 2);
            }
        }
        return trimmed;
    }

    bool parseIniFile(const char* filePath) {
        ifstream ifs(filePath);
        if (!ifs.is_open()) {
            logger.Error("无法打开INI文件: %s", filePath);
            return false;
        }

        iniSections_.clear();
        IniSection* currentSection = nullptr;

        std::string line;
        int lineNum = 0;
        while (std::getline(ifs, line)) {
            lineNum++;
            std::string trimmed = trim(line);

            // 跳过空行和注释
            if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
                continue;
            }

            // 检查是否是段头 [section]
            if (trimmed.front() == '[' && trimmed.back() == ']') {
                IniSection section;
                section.name = trim(trimmed.substr(1, trimmed.size() - 2));
                iniSections_.push_back(std::move(section));
                currentSection = &iniSections_.back();
                continue;
            }

            // 解析 key = value
            size_t eqPos = trimmed.find('=');
            if (eqPos != std::string::npos) {
                if (currentSection == nullptr) {
                    // INI 文件在第一个段之前有键值对，创建一个默认段
                    IniSection section;
                    section.name = "default";
                    iniSections_.push_back(std::move(section));
                    currentSection = &iniSections_.back();
                }

                std::string key = trim(trimmed.substr(0, eqPos));
                std::string value = unquote(trimmed.substr(eqPos + 1));

                if (!key.empty()) {
                    currentSection->entries.emplace_back(std::move(key), std::move(value));
                }
            }
        }

        ifs.close();
        logger.Debug("INI文件解析完成: %s 共 %zu 个段", filePath, iniSections_.size());
        return true;
    }

    // === INI -> JSON 合并 ===

    // 在 iniSections_ 中按名称查找段
    const IniSection* findIniSection(const std::string& name) const {
        for (const auto& sec : iniSections_) {
            if (sec.name == name) return &sec;
        }
        return nullptr;
    }

    // 在段中按 key 查找 value
    static std::string getIniValue(const IniSection* section, const std::string& key, const std::string& defaultVal = "") {
        if (!section) return defaultVal;
        for (const auto& entry : section->entries) {
            if (entry.first == key) return entry.second;
        }
        return defaultVal;
    }

    // 将 INI 中的字符串值转为整数
    static int iniToInt(const std::string& val, int defaultVal = 0) {
        if (val.empty()) return defaultVal;
        try {
            return std::stoi(val);
        } catch (...) {
            return defaultVal;
        }
    }

    // 将 INI 中的字符串值转为布尔
    static bool iniToBool(const std::string& val, bool defaultVal = false) {
        if (val.empty()) return defaultVal;
        if (val == "true" || val == "1" || val == "True" || val == "TRUE") return true;
        if (val == "false" || val == "0" || val == "False" || val == "FALSE") return false;
        return defaultVal;
    }

    // 将 INI 中的整数转为字符串
    static std::string intToStr(int val) {
        return std::to_string(val);
    }

    // 合并 INI 数据到 Config:: 全局变量（直接写入，不修改 json_view_t）
    void mergeIniToConfig() {
        // === 合并 [meta] 段 ===
        const IniSection* metaSec = findIniSection("meta");
        if (metaSec) {
            std::string name = getIniValue(metaSec, "name");
            if (!name.empty()) Meta::name = name.c_str();

            std::string author = getIniValue(metaSec, "author");
            if (!author.empty()) Meta::author = author.c_str();

            int configVersion = iniToInt(getIniValue(metaSec, "configVersion"), -1);
            if (configVersion >= 0) Meta::version = configVersion;

            std::string loglevel = getIniValue(metaSec, "loglevel");
            if (!loglevel.empty()) Meta::loglevel = loglevel.c_str();
        }

        // === 合并 [function] 段到 Function 全局变量 ===
        const IniSection* funcSec = findIniSection("function");
        if (funcSec) {
            bool cpuset = iniToBool(getIniValue(funcSec, "cpuset"));
            if (cpuset) Cpuset::enable = cpuset;

            bool launchBoost = iniToBool(getIniValue(funcSec, "LaunchBoost"));
            if (launchBoost) LaunchBoost::enable = launchBoost;

            bool easScheduler = iniToBool(getIniValue(funcSec, "EasScheduler"));
            if (easScheduler) Scheduler::enable = easScheduler;

            bool loadBalancing = iniToBool(getIniValue(funcSec, "LoadBalancing"));
            if (loadBalancing) LoadBanlace::enable = loadBalancing;

            bool disableQcomGpu = iniToBool(getIniValue(funcSec, "DisableQcomGpu"));
            GpuFreq::enable = !disableQcomGpu;

            bool adjIOSched = iniToBool(getIniValue(funcSec, "AdjIOScheduler"));
            if (adjIOSched) IOScheduler::enable = adjIOSched;
        }

        // === 合并 [Cpuset] 段 ===
        const IniSection* cpusetSec = findIniSection("Cpuset");
        if (cpusetSec) {
            std::string v;
            v = getIniValue(cpusetSec, "top_app");          if (!v.empty()) Cpuset::top_app = v.c_str();
            v = getIniValue(cpusetSec, "foreground");       if (!v.empty()) Cpuset::foreground = v.c_str();
            v = getIniValue(cpusetSec, "background");       if (!v.empty()) Cpuset::background = v.c_str();
            v = getIniValue(cpusetSec, "system_background");if (!v.empty()) Cpuset::system_background = v.c_str();
            v = getIniValue(cpusetSec, "restricted");       if (!v.empty()) Cpuset::restricted = v.c_str();
        }

        // === 合并 [IO_Settings] 段 ===
        const IniSection* ioSec = findIniSection("IO_Settings");
        if (ioSec) {
            std::string scheduler = getIniValue(ioSec, "Scheduler");
            if (!scheduler.empty()) IOScheduler::scheduler = scheduler.c_str();
        }

        // === 合并 [EasSchedulerVaule] 段到 Scheduler ===
        const IniSection* easSec = findIniSection("EasSchedulerVaule");
        if (easSec) {
            std::string v;
            v = getIniValue(easSec, "sched_min_granularity_ns");    if (!v.empty()) Scheduler::Sched_min_granularity_ns = v.c_str();
            v = getIniValue(easSec, "sched_nr_migrate");             if (!v.empty()) Scheduler::Sched_nr_migrate = v.c_str();
            v = getIniValue(easSec, "sched_wakeup_granularity_ns");  if (!v.empty()) Scheduler::Sched_wakeup_granularity_ns = v.c_str();
            v = getIniValue(easSec, "sched_schedstats");
            if (!v.empty()) Scheduler::Sched_schedstats = iniToBool(v);
        }

        // === 合并 [ParamSchedPath] 段 ===
        const IniSection* schedPathSec = findIniSection("ParamSchedPath");
        if (schedPathSec) {
            // 读取 ParamSchedPath1~12 的值，写入所有簇的 schedParam
            for (int i = 0; i <= 3; i++) {
                for (int j = 1; j <= 12; j++) {
                    char key[32];
                    snprintf(key, sizeof(key), "ParamSchedPath%d", j);
                    std::string pathName = getIniValue(schedPathSec, key);
                    if (!pathName.empty()) {
                        schedParam[i].Name[j] = pathName.c_str();
                    }
                }
            }
        }

        // === 合并 [powersave]/[balance]/[performance]/[fast] 段到 Performances ===
        // 注意：INI 中的频率值只覆盖当前 mode 对应的配置
        const IniSection* switchSec = findIniSection(mode.c_str());
        if (switchSec) {
            // scaling_governor -> 所有簇的 CpuGovernor
            std::string governor = getIniValue(switchSec, "scaling_governor");
            if (!governor.empty()) {
                for (int i = 0; i <= 3; i++) {
                    Performances::CpuGovernor[i] = governor.c_str();
                }
            }

            // 频率映射
            int smallMaxFreq = iniToInt(getIniValue(switchSec, "SmallCoreMaxFreq"), -1);
            if (smallMaxFreq > 0) Performances::MaxFreq[0] = intToStr(smallMaxFreq).c_str();

            int mediumMaxFreq = iniToInt(getIniValue(switchSec, "MediumCoreMaxFreq"), -1);
            if (mediumMaxFreq > 0) Performances::MaxFreq[1] = intToStr(mediumMaxFreq).c_str();

            int bigMaxFreq = iniToInt(getIniValue(switchSec, "BigCoreMaxFreq"), -1);
            if (bigMaxFreq > 0) Performances::MaxFreq[2] = intToStr(bigMaxFreq).c_str();

            int superBigMaxFreq = iniToInt(getIniValue(switchSec, "SuperBigCoreMaxFreq"), -1);
            if (superBigMaxFreq > 0) Performances::MaxFreq[3] = intToStr(superBigMaxFreq).c_str();

            // ParamSched1~12 -> SchedParam.c0.value1~12
            for (int j = 1; j <= 12; j++) {
                char key[32];
                snprintf(key, sizeof(key), "ParamSched%d", j);
                std::string val = getIniValue(switchSec, key);
                if (!val.empty()) {
                    schedParam[0].Value[j] = val.c_str();
                }
            }

            logger.Info("INI 预设已合并到当前模式: %s", mode.c_str());
        }
    }

    // === 版本控制 ===

    void saveSnapshot() {
        // 保存上一次成功的快照
        if (!lastJsonSnapshot_.empty()) {
            // 已经有快照了，不需要覆盖（保留最早的可用快照用于回滚）
            return;
        }

        // 读取当前 config.json 内容作为快照
        ifstream ifs(configPath, std::ios::binary);
        if (ifs) {
            std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
            if (!content.empty()) {
                lastJsonSnapshot_ = std::move(content);
                logger.Debug("已保存配置快照用于回滚 (%zu bytes)", lastJsonSnapshot_.size());
            }
        }
    }

    std::string getCurrentTimestamp() const {
        time_t now = time(nullptr);
        struct tm* local_time = localtime(&now);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", local_time);
        return std::string(buf);
    }

    // === SoC 检测 ===

    // 检测设备 SoC 型号
    std::string detectSocModel() const {
        // 方法1: 读取 ro.board.platform 系统属性
        char platform[PROP_VALUE_MAX] = {0};
        if (__system_property_get("ro.board.platform", platform) > 0) {
            std::string plat(platform);
            if (!plat.empty()) {
                logger.Debug("ro.board.platform: %s", plat.c_str());
                return normalizeSocName(plat);
            }
        }

        // 方法2: 读取 ro.mediatek.platform 或 ro.product.board
        char board[PROP_VALUE_MAX] = {0};
        if (__system_property_get("ro.product.board", board) > 0) {
            std::string brd(board);
            if (!brd.empty()) {
                logger.Debug("ro.product.board: %s", brd.c_str());
                return normalizeSocName(brd);
            }
        }

        // 方法3: 读取 /proc/cpuinfo 中的 Hardware 字段
        ifstream cpuinfo("/proc/cpuinfo");
        if (cpuinfo.is_open()) {
            std::string line;
            while (std::getline(cpuinfo, line)) {
                if (line.find("Hardware") != std::string::npos) {
                    size_t colonPos = line.find(':');
                    if (colonPos != std::string::npos) {
                        std::string hw = trim(line.substr(colonPos + 1));
                        if (!hw.empty()) {
                            logger.Debug("CPU Hardware: %s", hw.c_str());
                            cpuinfo.close();
                            return normalizeSocName(hw);
                        }
                    }
                }
            }
            cpuinfo.close();
        }

        return "";
    }

    // 将 SoC 型号名称标准化为 INI 文件名格式
    // 例如: "kona" -> "SDM8G2", "lahaina" -> "SDM888", "pineapple" -> "SDM8G3"
    std::string normalizeSocName(const std::string& raw) const {
        std::string lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        // Qualcomm 平台代号映射（按精确度从高到低排序，避免重复匹配）
        if (lower.find("pineapple") != std::string::npos) return "SDM8G3";
        if (lower.find("sm8650") != std::string::npos)    return "SDM8G3";
        if (lower.find("kalama") != std::string::npos)    return "SDM8G2";
        if (lower.find("sm8550") != std::string::npos)    return "SDM8G2";
        if (lower.find("cape") != std::string::npos)      return "SDM8G1+";
        if (lower.find("taro") != std::string::npos)      return "SDM8G1";
        if (lower.find("sm8450") != std::string::npos)    return "SDM8G1";
        if (lower.find("lahaina") != std::string::npos)   return "SDM888";
        if (lower.find("sm8350") != std::string::npos)    return "SDM888";
        if (lower.find("sdm870") != std::string::npos)    return "SDM870";
        if (lower.find("kona") != std::string::npos)      return "SDM865";
        if (lower.find("sm8250") != std::string::npos)    return "SDM865";
        if (lower.find("sm8150") != std::string::npos)    return "SDM855";
        if (lower.find("sdm855") != std::string::npos)    return "SDM855";
        if (lower.find("sm7325") != std::string::npos)    return "SDM7G2+";
        if (lower.find("sdm7g2") != std::string::npos)    return "SDM7G2+";
        if (lower.find("sdm7s") != std::string::npos)     return "SDM7sG2";

        // MTK 平台
        if (lower.find("mt6983") != std::string::npos || lower.find("dimensity 1200") != std::string::npos) return "MTK8100";
        if (lower.find("mt6893") != std::string::npos || lower.find("dimensity 1200") != std::string::npos) return "MTK8100";
        if (lower.find("mt6877") != std::string::npos || lower.find("dimensity 920") != std::string::npos) return "MTK720";
        if (lower.find("mt6853") != std::string::npos || lower.find("dimensity 720") != std::string::npos) return "MTK720";
        if (lower.find("mt6885") != std::string::npos || lower.find("dimensity 1000") != std::string::npos) return "MTK8100";

        // 如果 raw 本身已经像 INI 文件名格式（如 "SDM8G3"），直接返回
        if (raw.find("SDM") != std::string::npos || raw.find("MTK") != std::string::npos) {
            return raw;
        }

        // 无法识别，返回空
        return "";
    }

    // 在 iniDir 中查找匹配的 INI 文件
    std::string findMatchingIni(const std::string& socModel) const {
        DIR* dir = opendir(iniDir);
        if (!dir) {
            logger.Debug("无法打开INI目录: %s", iniDir);
            return "";
        }

        std::string bestMatch;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename(entry->d_name);
            // 检查是否是 .ini 文件
            if (filename.size() < 5 || filename.substr(filename.size() - 4) != ".ini") {
                continue;
            }

            // 去掉 .ini 后缀得到基础名
            std::string baseName = filename.substr(0, filename.size() - 4);

            // 精确匹配
            if (baseName == socModel) {
                bestMatch = std::string(iniDir) + "/" + filename;
                break;
            }

            // 前缀匹配（如 socModel="SDM8G2" 匹配 "SDM8G2+.ini"）
            if (!baseName.empty() && socModel.size() > 0 &&
                baseName.substr(0, socModel.size()) == socModel) {
                if (bestMatch.empty()) {
                    bestMatch = std::string(iniDir) + "/" + filename;
                }
            }
        }

        closedir(dir);
        return bestMatch;
    }

    // === 清理 ===

    void clearAllConfig() {
        for (int i = 0; i <= 3; i++) {
            for (int j = 0; j <= 12; j++) {
                this->schedParam[i].Name[j] = "";
                this->schedParam[i].Value[j] = "";
            }
        }
        Performances::MinFreq[0] = ""; Performances::MinFreq[1] = "";
        Performances::MinFreq[2] = ""; Performances::MinFreq[3] = "";
        Performances::MaxFreq[0] = ""; Performances::MaxFreq[1] = "";
        Performances::MaxFreq[2] = ""; Performances::MaxFreq[3] = "";
        Performances::CpuGovernor[0] = ""; Performances::CpuGovernor[1] = "";
        Performances::CpuGovernor[2] = ""; Performances::CpuGovernor[3] = "";
        LaunchBoost::BoostFreq[0] = ""; LaunchBoost::BoostFreq[1] = "";
        LaunchBoost::BoostFreq[2] = ""; LaunchBoost::BoostFreq[3] = "";
        Performances::Online[0] = 0; Performances::Online[1] = 0;
        Performances::Online[2] = 0; Performances::Online[3] = 0;
        Performances::Online[4] = 0; Performances::Online[5] = 0;
        Performances::Online[6] = 0; Performances::Online[7] = 0;
        GpuFreq::min_freq = "";
        GpuFreq::max_freq = "";
        Meta::name = "";
        Meta::author = "";
        Meta::loglevel = "";
        IOScheduler::scheduler = "";
    }
};
