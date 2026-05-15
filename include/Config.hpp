//By Ktwo
#pragma once 
#include <iostream>
#include <fstream>
#include <string>
#include "Json/string.hpp"

using string_t = qlib::string_t;
using bool_t = qlib::bool_t;

using std::ifstream;

namespace Config {
    namespace Meta {
        string_t name;
        int version = -1;
        string_t author;
        string_t loglevel;
    }
    
    namespace Policy {
        int CpuPolicy [4] = { -1, -1, -1, -1 }; 
    }

    namespace Cpuset {
        bool enable = false;
        string_t top_app;
        string_t foreground;
        string_t background;
        string_t system_background;
        string_t restricted;
    }

    namespace LaunchBoost {
        bool enable = false;
        int boost_rate_limit_ms;
        int boost_duration_ms = 2000;  // Boost 持续时间（毫秒），超时后自动恢复
        string_t BoostFreq[4];
    }

    namespace OfficialMode{
        bool enable = false;
    } 

    namespace LoadBanlace {
        bool enable = false;
    }

    namespace Scheduler {
        bool enable = false;
        bool Sched_energy_aware = false;
        bool Sched_schedstats = false;
        string_t Sched_latency_ns;
        string_t Sched_migration_cost_ns;
        string_t Sched_min_granularity_ns;
        string_t Sched_wakeup_granularity_ns;
        string_t Sched_nr_migrate;
        string_t Sched_util_clamp_min;
        string_t Sched_util_clamp_max;
        // 新增CFS调度参数
        string_t Sched_child_runs_first;      // 子进程优先运行
        string_t Sched_tunable_scaling;       // 调度器缩放
        string_t Sched_sched_compat_yield;    // 兼容yield
        string_t Sched_wakeup_load_threshold; // 唤醒负载阈值
        string_t Sched_migration_cost;        // 迁移成本
        // NUMA相关（如果支持）
        bool Sched_numa_balancing = false;
        string_t Sched_numa_preferred_nid;
    }

    namespace Stune {
        bool enable = false;
        string_t top_app_boost;
        string_t foreground_boost;
        string_t background_boost;
        string_t top_app_prefer_idle;
        string_t foreground_prefer_idle;
        // 新增Uclamp参数（现代Android内核支持）
        string_t top_app_uclamp_min;
        string_t top_app_uclamp_max;
        string_t foreground_uclamp_min;
        string_t foreground_uclamp_max;
        string_t background_uclamp_min;
        string_t background_uclamp_max;
        // 新增prefer_idle
        string_t background_prefer_idle;
        string_t system_background_prefer_idle;
        string_t restricted_prefer_idle;
    }

    namespace IOScheduler {
        bool enable = false;
        string_t scheduler;
        string_t read_ahead_kb;
        string_t nr_requests;
    }

    namespace GpuFreq {
        bool enable = false;
        string_t min_freq;
        string_t max_freq;
    }

    namespace LoadPredict {
        bool enable = false;
        // EMA 平滑系数 (0.0 ~ 1.0，越大越敏感)
        double alpha = 0.2;
        // 采样间隔（毫秒，参考 uperf 使用 100ms 高频采样）
        int sample_interval_ms = 100;
        // 负载级别阈值
        double idle_threshold = 20.0;       // < 20% 视为 IDLE
        double heavy_threshold = 70.0;      // > 70% 视为 HEAVY
        double critical_threshold = 95.0;   // > 95% 视为 CRITICAL
        // 阈值缓冲次数（连续 N 次确认后才切换级别，100ms x 10 = 1秒确认时间）
        int buffer_count = 10;
        // 预测阈值（参考 uperf v3）：负载增量 > predictThd * 100 时使用预测值
        double predict_thd = 0.3;
        // 每个核心的相对性能效率值（用于加权负载计算）
        int efficiency[8] = {120, 120, 120, 120, 220, 220, 220, 240};
        // 非对称 EMA
        double alpha_up = 1.0;       // 上升系数（1.0 = 即时响应）
        double alpha_down = 0.15;     // 下降系数（0.15 = 缓慢衰减）
        // CUSUM 变点检测
        double cusum_k = 5.0;        // 允许偏差
        double cusum_h = 25.0;       // 决策阈值
    }

    namespace ProcessPriority {
        bool enable = false;
        // 前台应用主线程优先级提升
        string_t foreground_nice;        // nice值，如 "-10"
        string_t foreground_oom_score_adj; // OOM分数调整
        // 后台应用限制
        string_t background_nice;
        string_t background_oom_score_adj;
        // 系统关键进程保护
        string_t system_nice;
    }

    namespace Memory {
        bool enable = false;
        string_t swappiness;           // 交换倾向 0-100
        string_t page_cluster;         // 页面聚类
        string_t dirty_ratio;          // 脏页比例
        string_t dirty_background_ratio; // 后台脏页比例
        string_t vfs_cache_pressure;   // VFS缓存压力
        // LMK参数（如果内核支持）
        string_t lmk_minfree_levels;   // LMK最小释放级别
    }

    namespace SchedDomain {
        bool enable = false;
        // 调度域层级设置
        string_t sd0_flags;  // CPU核心内部
        string_t sd1_flags;  // 集群内部
        string_t sd2_flags;  // 跨集群
    }

    namespace Performances {
        int Online[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        string_t MinFreq[4];
        string_t MaxFreq[4];
        string_t CpuGovernor[4];

        // 场景频率覆盖（空=继承模式基础）
        string_t IdleMinFreq[4];
        string_t IdleMaxFreq[4];
        string_t HeavyMinFreq[4];
        string_t HeavyMaxFreq[4];
        string_t CriticalMaxFreq[4];  // critical只要MaxFreq（拉满）
    }

    struct SchedParam {
        string_t Name[24];
        string_t Value[24];
    };
}; 
