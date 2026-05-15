#pragma once

#include <cstring>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <thread>
#include <atomic>
#include "JsonConfig.hpp"
#include "Config.hpp"
#include "Utils.hpp"
#include "Logger.hpp"

using namespace Config;

class Function {
public:
    std::atomic<bool> gpuGuardShouldExit{false};
    std::atomic<int> latestGpuMaxMhz{-1};
    std::atomic<int> latestGpuMinMhz{-1};

private:
    static constexpr const char* qcomFeas = "/sys/module/perfmgr/parameters/perfmgr_enable";
    static constexpr const char* mtkFeas = "/sys/module/mtk_fpsgo/parameters/perfmgr_enable";
    static constexpr const char* ufsPath = "/sys/class/block/sda";
    static constexpr const char* cpusetPath = "/dev/cpuset/";
    static constexpr const char* cpuctlPath = "/dev/cpuctl/";
    static constexpr const char* qcomGpuPath = "/sys/class/kgsl/kgsl-3d0/";
    static constexpr const char* thermalPath = "/sys/devices/virtual/thermal/";
    static constexpr const char* easSchedPath = "/proc/sys/kernel/sched_energy_aware";
    static constexpr const char* stunePath = "/dev/stune/";

    Utils utils;
    Logger logger;

    std::thread gpuGuardThread;

    std::mutex gpuWriteMutex;

public:
    Function() = default;

    Function(const Function&) = delete;
    Function& operator=(const Function&) = delete;

    ~Function() {
        stopGuards();
    }

    void processPriorityControl() {
        if (!ProcessPriority::enable) return;

        // 通过/proc调整正在运行的前台应用
        // 实际实现可能需要遍历/proc/[pid]/oom_score_adj
        // 这里提供框架，具体实现根据需求
        logger.Info("进程优先级控制已%s", ProcessPriority::enable ? "启用" : "跳过");
    }

    void memoryOptimize() {
        if (!Memory::enable) return;

        if (!Memory::swappiness.empty()) {
            utils.FileWrite("/proc/sys/vm/swappiness", Memory::swappiness);
        }
        if (!Memory::page_cluster.empty()) {
            utils.FileWrite("/proc/sys/vm/page-cluster", Memory::page_cluster);
        }
        if (!Memory::dirty_ratio.empty()) {
            utils.FileWrite("/proc/sys/vm/dirty_ratio", Memory::dirty_ratio);
        }
        if (!Memory::dirty_background_ratio.empty()) {
            utils.FileWrite("/proc/sys/vm/dirty_background_ratio", Memory::dirty_background_ratio);
        }
        if (!Memory::vfs_cache_pressure.empty()) {
            utils.FileWrite("/proc/sys/vm/vfs_cache_pressure", Memory::vfs_cache_pressure);
        }
        if (!Memory::lmk_minfree_levels.empty()) {
            // LMK参数路径因内核版本而异
            utils.FileWrite("/sys/module/lowmemorykiller/parameters/minfree", Memory::lmk_minfree_levels);
        }

        logger.Info("内存优化已应用");
    }

    void schedDomainOptimize() {
        if (!SchedDomain::enable) return;

        // 调度域优化需要写入/sys/kernel/debug/sched/domains/
        // 或/sys/devices/system/cpu/cpufreq/policy*/domain/
        // 具体实现根据内核版本和设备特性
        logger.Info("调度域优化已%s", SchedDomain::enable ? "启用" : "跳过");
    }

    void AllFunC() {
        cpusetFunction();
        gpuFreqControl();
        LoadBanlance();
        CfsSchedOpt();
        stuneControl();
        ioSchedulerControl();
        processPriorityControl();   // 新增
        memoryOptimize();           // 新增
        schedDomainOptimize();      // 新增

        startGuards();
    }

    void startGuards() {
        gpuGuardShouldExit.store(false);

        if (GpuFreq::enable && !gpuGuardThread.joinable()) {
            gpuGuardThread = std::thread([this]() {
                gpuFreqGuard();
            });
            logger.Info("GPU频率守护线程已启动");
        }
    }

    void stopGuards() {
        gpuGuardShouldExit.store(true);

        if (gpuGuardThread.joinable()) {
            gpuGuardThread.join();
        }
    }

    void cpusetFunction() {
        if (!Cpuset::enable) return;

        utils.FileWrite("/dev/cpuset/top-app/cpus", Cpuset::top_app);
        utils.FileWrite("/dev/cpuset/foreground/cpus", Cpuset::foreground);
        utils.FileWrite("/dev/cpuset/background/cpus", Cpuset::background);
        utils.FileWrite("/dev/cpuset/system-background/cpus", Cpuset::system_background);
        utils.FileWrite("/dev/cpuset/restricted/cpus", Cpuset::restricted);

        logger.Info("Cpuset OK");
    }

    bool writeSysfsLocked(const char* path, const char* value, int valLen) {
        if (!path || !value || valLen <= 0) return false;

        int fd = open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) return false;

        char buf[64];

        if (valLen + 1 >= static_cast<int>(sizeof(buf))) {
            valLen = sizeof(buf) - 2;
        }

        memcpy(buf, value, valLen);
        buf[valLen] = '\n';

        int totalLen = valLen + 1;

        ssize_t n = write(fd, buf, totalLen);
        close(fd);

        return n == totalLen;
    }

    void setKgslGovernorPerformance() {
        const char* govPath = "/sys/class/kgsl/kgsl-3d0/devfreq/governor";

        if (access(govPath, F_OK) != 0) return;

        char curGov[64] = {0};

        int fd = open(govPath, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ssize_t n = read(fd, curGov, sizeof(curGov) - 1);
            close(fd);

            if (n > 0) {
                for (int i = 0; curGov[i]; i++) {
                    if (curGov[i] == '\n' || curGov[i] == '\r') {
                        curGov[i] = '\0';
                        break;
                    }
                }

                if (strstr(curGov, "performance")) {
                    return;
                }
            }
        }

        chmod(govPath, 0666);

        int fdw = open(govPath, O_WRONLY | O_CLOEXEC);
        if (fdw >= 0) {
            write(fdw, "performance\n", 12);
            close(fdw);
            logger.Info("GPU governor 已切换: performance");
        }
    }

    /**
     * 锁定所有已知的 GPU governor 路径为 performance
     * 防止系统 devfreq 自动调频覆盖我们的设置
     */
    void lockGpuGovernor() {
        // kgsl (高通 Adreno)
        setKgslGovernorPerformance();

        // 通用 devfreq governor 路径
        const char* govPaths[] = {
            "/sys/class/kgsl/kgsl-3d0/devfreq/governor",
            "/sys/class/kgsl/kgsl-3d0/devfreq/governor",
            "/sys/kernel/gpu/devfreq/governor",
            "/sys/devices/platform/soc/soc:qcom,kgsl-3d0/devfreq/governor",
            nullptr
        };

        for (int i = 0; govPaths[i]; i++) {
            if (access(govPaths[i], F_OK) != 0) continue;

            char curGov[64] = {0};
            int fd = open(govPaths[i], O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                ssize_t n = read(fd, curGov, sizeof(curGov) - 1);
                close(fd);

                if (n > 0) {
                    for (int j = 0; curGov[j]; j++) {
                        if (curGov[j] == '\n' || curGov[j] == '\r') {
                            curGov[j] = '\0';
                            break;
                        }
                    }
                    if (strstr(curGov, "performance")) continue;
                }
            }

            chmod(govPaths[i], 0666);
            int fdw = open(govPaths[i], O_WRONLY | O_CLOEXEC);
            if (fdw >= 0) {
                write(fdw, "performance\n", 12);
                close(fdw);
            }
        }

        // 尝试禁用 devfreq 的自动调频（部分设备支持）
        const char* disablePaths[] = {
            "/sys/class/kgsl/kgsl-3d0/devfreq/polling_interval",
            "/sys/class/kgsl/kgsl-3d0/devfreq/gov_polling_interval",
            nullptr
        };

        for (int i = 0; disablePaths[i]; i++) {
            if (access(disablePaths[i], F_OK) != 0) continue;
            int fdw = open(disablePaths[i], O_WRONLY | O_CLOEXEC);
            if (fdw >= 0) {
                write(fdw, "0\n", 2);
                close(fdw);
            }
        }
    }

    int readKgslAvailableFreqs(int freqsOut[]) {
        const char* path = "/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies";

        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;

        char buf[1024];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n <= 0) return 0;

        buf[n] = '\0';

        int count = 0;

        for (char* p = buf; *p && count < 64;) {
            while (*p == ' ' || *p == '\n' || *p == '\t') p++;

            if (*p == '\0') break;

            freqsOut[count++] = atoi(p);

            while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
        }

        return count;
    }

    int snapGpuFreq(int targetHz, const int availFreqs[], int count, bool isMax) {
        if (count <= 0) return targetHz;

        int best = -1;
        int bestDiff = 0x7FFFFFFF;

        for (int i = 0; i < count; i++) {
            int f = availFreqs[i];
            if (f <= 0) continue;

            if (isMax) {
                if (f <= targetHz) {
                    int diff = targetHz - f;
                    if (diff < bestDiff) {
                        bestDiff = diff;
                        best = f;
                    }
                }
            } else {
                if (f >= targetHz) {
                    int diff = f - targetHz;
                    if (diff < bestDiff) {
                        bestDiff = diff;
                        best = f;
                    }
                }
            }
        }

        if (best < 0) {
            bestDiff = 0x7FFFFFFF;

            for (int i = 0; i < count; i++) {
                if (availFreqs[i] <= 0) continue;

                int diff = availFreqs[i] > targetHz
                               ? availFreqs[i] - targetHz
                               : targetHz - availFreqs[i];

                if (diff < bestDiff) {
                    bestDiff = diff;
                    best = availFreqs[i];
                }
            }
        }

        return best > 0 ? best : targetHz;
    }

    bool tryGpuPath(const char* path, int mhzValue, bool isHz, bool needsGov, const char* label) {
        if (!path || !label) return false;
        if (mhzValue <= 0) return true;
        if (access(path, F_OK) != 0) return false;

        std::lock_guard<std::mutex> lock(gpuWriteMutex);

        if (needsGov) {
            setKgslGovernorPerformance();
        }

        long long valueToWriteLL = isHz
                                       ? static_cast<long long>(mhzValue) * 1000000LL
                                       : static_cast<long long>(mhzValue);

        if (valueToWriteLL <= 0 || valueToWriteLL > 2147483647LL) {
            logger.Warn("GPU频率数值异常: path=%s value=%lld", path, valueToWriteLL);
            return false;
        }

        int valueToWrite = static_cast<int>(valueToWriteLL);

        bool isMax = false;
        if (label[0] == 'm' && label[1] == 'a') {
            isMax = true;
        }

        if (isHz && strstr(path, "kgsl")) {
            int availFreqs[64];
            int availCount = readKgslAvailableFreqs(availFreqs);

            if (availCount > 0) {
                int snapped = snapGpuFreq(valueToWrite, availFreqs, availCount, isMax);

                if (snapped != valueToWrite) {
                    logger.Debug("GPU频率校准: %d -> %d, path=%s", valueToWrite, snapped, path);
                    valueToWrite = snapped;
                }
            }
        }

        char valStr[32];
        int len = FastSnprintf(valStr, sizeof(valStr), "%d", valueToWrite);

        if (len <= 0) return false;

        bool ok = writeSysfsLocked(path, valStr, len);

        if (ok) {
            logger.Debug("GPU频率写入成功: %s = %s", path, valStr);
        } else {
            logger.Debug("GPU频率写入失败: %s = %s", path, valStr);
        }

        return ok;
    }

    bool tryGpuMaxPaths(int mhzMax) {
        if (mhzMax <= 0) return true;

        if (tryGpuPath("/sys/kernel/gpu/gpu_max_clock", mhzMax, false, false, "max"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/devfreq/max_freq", mhzMax, true, true, "max"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/max_gpuclk", mhzMax, true, false, "max"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/max_clock_mhz", mhzMax, false, false, "max"))
            return true;

        if (tryGpuPath("/sys/kernel/gpu/gpuclk", mhzMax, false, false, "max"))
            return true;

        if (tryGpuPath("/sys/kernel/gpu/mali/clock", mhzMax, false, false, "max"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/gpu_max_freq", mhzMax, true, false, "max"))
            return true;

        return false;
    }

    bool tryGpuMinPaths(int mhzMin) {
        if (mhzMin <= 0) return true;

        if (tryGpuPath("/sys/kernel/gpu/gpu_min_clock", mhzMin, false, false, "min"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/devfreq/min_freq", mhzMin, true, true, "min"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/min_clock_mhz", mhzMin, false, false, "min"))
            return true;

        if (tryGpuPath("/sys/class/kgsl/kgsl-3d0/gpu_min_freq", mhzMin, true, false, "min"))
            return true;

        return false;
    }

    bool tryGpuPwrlevel(int mhzMax, int mhzMin) {
        const char* availPath  = "/sys/class/kgsl/kgsl-3d0/gpu_available_frequencies";
        const char* numPwrPath = "/sys/class/kgsl/kgsl-3d0/num_pwrlevels";
        const char* maxPwrPath = "/sys/class/kgsl/kgsl-3d0/max_pwrlevel";
        const char* minPwrPath = "/sys/class/kgsl/kgsl-3d0/min_pwrlevel";

        if (access(numPwrPath, F_OK) != 0 || access(availPath, F_OK) != 0) {
            return false;
        }

        int numPwrlevels = utils.readInt(numPwrPath);
        if (numPwrlevels <= 0) return false;

        int fd = open(availPath, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;

        char readBuf[1024];
        ssize_t n = read(fd, readBuf, sizeof(readBuf) - 1);
        close(fd);

        if (n <= 0) return false;

        readBuf[n] = '\0';

        int freqs[64];
        int freqCount = 0;

        for (char* p = readBuf; *p && freqCount < 64;) {
            while (*p == ' ' || *p == '\n' || *p == '\t') p++;

            if (*p == '\0') break;

            freqs[freqCount++] = atoi(p);

            while (*p && *p != ' ' && *p != '\n' && *p != '\t') p++;
        }

        if (freqCount <= 0) return false;

        bool success = false;

        if (mhzMax > 0) {
            long long targetHzLL = static_cast<long long>(mhzMax) * 1000000LL;
            if (targetHzLL > 2147483647LL) return false;

            int targetHz = static_cast<int>(targetHzLL);
            int maxPwr = 0;
            int bestDiff = 0x7FFFFFFF;

            for (int i = 0; i < freqCount; i++) {
                if (freqs[i] <= targetHz) {
                    int diff = targetHz - freqs[i];

                    if (diff < bestDiff) {
                        bestDiff = diff;
                        maxPwr = i;
                    }
                }
            }

            char pwrBuf[16];
            int pwrLen = FastSnprintf(pwrBuf, sizeof(pwrBuf), "%d", maxPwr);

            if (writeSysfsLocked(maxPwrPath, pwrBuf, pwrLen)) {
                success = true;
            }
        }

        if (mhzMin > 0) {
            long long targetHzLL = static_cast<long long>(mhzMin) * 1000000LL;
            if (targetHzLL > 2147483647LL) return false;

            int targetHz = static_cast<int>(targetHzLL);
            int minPwr = freqCount - 1;
            int bestDiff = 0x7FFFFFFF;

            for (int i = 0; i < freqCount; i++) {
                if (freqs[i] >= targetHz) {
                    int diff = freqs[i] - targetHz;

                    if (diff < bestDiff) {
                        bestDiff = diff;
                        minPwr = i;
                    }
                }
            }

            char pwrBuf[16];
            int pwrLen = FastSnprintf(pwrBuf, sizeof(pwrBuf), "%d", minPwr);

            if (writeSysfsLocked(minPwrPath, pwrBuf, pwrLen)) {
                success = true;
            }
        }

        if (!success) {
            const char* maliDvfsMax = "/sys/kernel/gpu/mali/dvfs_max";

            if (access(maliDvfsMax, F_OK) == 0 && mhzMax > 0) {
                char valStr[32];
                int len = FastSnprintf(valStr, sizeof(valStr), "%d", mhzMax);

                if (len > 0 && writeSysfsLocked(maliDvfsMax, valStr, len)) {
                    logger.Debug("GPU PwrLevel兜底: mali dvfs_max = %s", valStr);
                    success = true;
                }
            }
        }

        return success;
    }

    void gpuFreqControl() {
        if (!GpuFreq::enable) return;

        // 先锁定 governor 为 performance，防止 devfreq 覆盖
        lockGpuGovernor();

        int mhzMin = Fastatoi(GpuFreq::min_freq.c_str());
        int mhzMax = Fastatoi(GpuFreq::max_freq.c_str());

        if (mhzMin <= 0 && mhzMax <= 0) return;

        bool minOk = true;
        bool maxOk = true;

        if (mhzMin > 0) {
            minOk = tryGpuMinPaths(mhzMin);
            if (minOk) {
                latestGpuMinMhz.store(mhzMin);
            }
        }

        if (mhzMax > 0) {
            maxOk = tryGpuMaxPaths(mhzMax);
            if (maxOk) {
                latestGpuMaxMhz.store(mhzMax);
            }
        }

        if ((!minOk && mhzMin > 0) || (!maxOk && mhzMax > 0)) {
            if (tryGpuPwrlevel(mhzMax, mhzMin)) {
                if (!minOk) minOk = true;
                if (!maxOk) maxOk = true;
            }
        }

        if (minOk && maxOk) {
            logger.Info("GPU频率已设置: min=%dMHz max=%dMHz", mhzMin, mhzMax);
        } else {
            logger.Warn("GPU频率设置可能未完全生效: minOk=%d maxOk=%d",
                        minOk ? 1 : 0,
                        maxOk ? 1 : 0);
        }
    }

    void gpuFreqGuard() {
        if (!GpuFreq::enable) return;

        logger.Info("GPU守护已启动，周期: 3秒");

        while (!gpuGuardShouldExit.load()) {
            // 每次循环都锁定 governor
            lockGpuGovernor();

            int mhzMin = Fastatoi(GpuFreq::min_freq.c_str());
            int mhzMax = Fastatoi(GpuFreq::max_freq.c_str());

            bool minOk = true;
            bool maxOk = true;

            if (mhzMin > 0) {
                minOk = tryGpuMinPaths(mhzMin);
                if (minOk) {
                    latestGpuMinMhz.store(mhzMin);
                }
            }

            if (mhzMax > 0) {
                maxOk = tryGpuMaxPaths(mhzMax);
                if (maxOk) {
                    latestGpuMaxMhz.store(mhzMax);
                }
            }

            if ((!minOk && mhzMin > 0) || (!maxOk && mhzMax > 0)) {
                tryGpuPwrlevel(mhzMax, mhzMin);
            }

            for (int i = 0; i < 30 && !gpuGuardShouldExit.load(); i++) {
                utils.sleep_ms(100);
            }
        }

        logger.Info("GPU守护已停止");
    }

    void LoadBanlance() {
        if (!LoadBanlace::enable) return;

        if (!checkCpuset()) {
            logger.Warn("Cpuset不支持");
            return;
        }

        const char* groups[] = {
            "",
            "top-app",
            "foreground",
            "background",
            "system-background",
            "restricted"
        };

        for (const char* group : groups) {
            char path[256];

            if (group[0] == '\0') {
                FastSnprintf(path, sizeof(path), "/dev/cpuset/sched_load_balance");
            } else {
                FastSnprintf(path, sizeof(path), "/dev/cpuset/%s/sched_load_balance", group);
            }

            utils.FileWrite(path, "1");

            if (group[0] == '\0') {
                FastSnprintf(path, sizeof(path), "/dev/cpuset/sched_relax_domain_level");
            } else {
                FastSnprintf(path, sizeof(path), "/dev/cpuset/%s/sched_relax_domain_level", group);
            }

            utils.FileWrite(path, "0");

            if (group[0] == '\0') {
                FastSnprintf(path, sizeof(path), "/dev/cpuset/memory_migrate");
            } else {
                FastSnprintf(path, sizeof(path), "/dev/cpuset/%s/memory_migrate", group);
            }

            utils.FileWrite(path, "0");
        }

        logger.Info("LoadBalancing OK");
    }

    void CfsSchedOpt() {
        if (!Scheduler::enable) return;

        utils.FileWrite("/proc/sys/kernel/sched_schedstats", Scheduler::Sched_schedstats ? "1" : "0");
        utils.FileWrite("/proc/sys/kernel/sched_latency_ns", Scheduler::Sched_latency_ns);
        utils.FileWrite("/proc/sys/kernel/sched_migration_cost_ns", Scheduler::Sched_migration_cost_ns);
        utils.FileWrite("/proc/sys/kernel/sched_min_granularity_ns", Scheduler::Sched_min_granularity_ns);
        utils.FileWrite("/proc/sys/kernel/sched_wakeup_granularity_ns", Scheduler::Sched_wakeup_granularity_ns);
        utils.FileWrite("/proc/sys/kernel/sched_nr_migrate", Scheduler::Sched_nr_migrate);
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_min", Scheduler::Sched_util_clamp_min);
        utils.FileWrite("/proc/sys/kernel/sched_util_clamp_max", Scheduler::Sched_util_clamp_max);

        // 新增CFS调度参数写入
        if (!Scheduler::Sched_child_runs_first.empty()) {
            utils.FileWrite("/proc/sys/kernel/sched_child_runs_first", Scheduler::Sched_child_runs_first);
            logger.Debug("Sched_child_runs_first调整为: %s", Scheduler::Sched_child_runs_first.c_str());
        }
        if (!Scheduler::Sched_tunable_scaling.empty()) {
            utils.FileWrite("/proc/sys/kernel/sched_tunable_scaling", Scheduler::Sched_tunable_scaling);
            logger.Debug("Sched_tunable_scaling调整为: %s", Scheduler::Sched_tunable_scaling.c_str());
        }
        if (!Scheduler::Sched_sched_compat_yield.empty()) {
            utils.FileWrite("/proc/sys/kernel/sched_compat_yield", Scheduler::Sched_sched_compat_yield);
            logger.Debug("Sched_sched_compat_yield调整为: %s", Scheduler::Sched_sched_compat_yield.c_str());
        }
        if (!Scheduler::Sched_wakeup_load_threshold.empty()) {
            utils.FileWrite("/proc/sys/kernel/sched_wakeup_load_threshold", Scheduler::Sched_wakeup_load_threshold);
            logger.Debug("Sched_wakeup_load_threshold调整为: %s", Scheduler::Sched_wakeup_load_threshold.c_str());
        }
        if (!Scheduler::Sched_migration_cost.empty()) {
            utils.FileWrite("/proc/sys/kernel/sched_migration_cost", Scheduler::Sched_migration_cost);
            logger.Debug("Sched_migration_cost调整为: %s", Scheduler::Sched_migration_cost.c_str());
        }

        // NUMA相关参数
        if (access("/proc/sys/kernel/numa_balancing", F_OK) == 0) {
            utils.FileWrite("/proc/sys/kernel/numa_balancing", Scheduler::Sched_numa_balancing ? "1" : "0");
            logger.Debug("Sched_numa_balancing调整为: %s", Scheduler::Sched_numa_balancing ? "开启" : "关闭");
        }
        if (!Scheduler::Sched_numa_preferred_nid.empty()) {
            utils.FileWrite("/proc/sys/kernel/numa_preferred_nid", Scheduler::Sched_numa_preferred_nid);
            logger.Debug("Sched_numa_preferred_nid调整为: %s", Scheduler::Sched_numa_preferred_nid.c_str());
        }

        if (checkEasSched()) {
            utils.FileWrite("/proc/sys/kernel/sched_energy_aware", Scheduler::Sched_energy_aware ? "1" : "0");
            logger.Info(Scheduler::Sched_energy_aware ? "已开启EAS调度器" : "已关闭EAS调度器");
        } else {
            logger.Warn("您的设备并不支持EAS调度器");
        }

        logger.Debug("Sched_energy_aware调整为: %s", Scheduler::Sched_energy_aware ? "开启" : "关闭");
        logger.Debug("Sched_schedstats调整为: %s", Scheduler::Sched_schedstats ? "开启" : "关闭");
        logger.Debug("Sched_latency_ns调整为: %s", Scheduler::Sched_latency_ns.c_str());
        logger.Debug("Sched_migration_cost_ns调整为: %s", Scheduler::Sched_migration_cost_ns.c_str());
        logger.Debug("Sched_wakeup_granularity_ns调整为: %s", Scheduler::Sched_wakeup_granularity_ns.c_str());
        logger.Debug("Sched_nr_migrate调整为: %s", Scheduler::Sched_nr_migrate.c_str());
        logger.Debug("Sched_util_clamp_min调整为: %s", Scheduler::Sched_util_clamp_min.c_str());
        logger.Debug("Sched_util_clamp_max调整为: %s", Scheduler::Sched_util_clamp_max.c_str());

        logger.Info("CFS调度器已优化完毕");
    }

    void stuneControl() {
        if (!Stune::enable) return;

        if (access(stunePath, F_OK) != 0) {
            logger.Warn("Stune不支持");
            return;
        }

        int n = 0;

        if (!Stune::top_app_boost.empty()) {
            utils.FileWrite("/dev/stune/top-app/schedtune.boost", Stune::top_app_boost);
            n++;
        }

        if (!Stune::foreground_boost.empty()) {
            utils.FileWrite("/dev/stune/foreground/schedtune.boost", Stune::foreground_boost);
            n++;
        }

        if (!Stune::background_boost.empty()) {
            utils.FileWrite("/dev/stune/background/schedtune.boost", Stune::background_boost);
            n++;
        }

        if (!Stune::top_app_prefer_idle.empty()) {
            utils.FileWrite("/dev/stune/top-app/schedtune.prefer_idle", Stune::top_app_prefer_idle);
            n++;
        }

        if (!Stune::foreground_prefer_idle.empty()) {
            utils.FileWrite("/dev/stune/foreground/schedtune.prefer_idle", Stune::foreground_prefer_idle);
            n++;
        }

        // Uclamp参数（Android 10+内核支持）
        if (!Stune::top_app_uclamp_min.empty()) {
            utils.FileWrite("/dev/stune/top-app/uclamp.min", Stune::top_app_uclamp_min);
            n++;
        }
        if (!Stune::top_app_uclamp_max.empty()) {
            utils.FileWrite("/dev/stune/top-app/uclamp.max", Stune::top_app_uclamp_max);
            n++;
        }
        if (!Stune::foreground_uclamp_min.empty()) {
            utils.FileWrite("/dev/stune/foreground/uclamp.min", Stune::foreground_uclamp_min);
            n++;
        }
        if (!Stune::foreground_uclamp_max.empty()) {
            utils.FileWrite("/dev/stune/foreground/uclamp.max", Stune::foreground_uclamp_max);
            n++;
        }
        if (!Stune::background_uclamp_min.empty()) {
            utils.FileWrite("/dev/stune/background/uclamp.min", Stune::background_uclamp_min);
            n++;
        }
        if (!Stune::background_uclamp_max.empty()) {
            utils.FileWrite("/dev/stune/background/uclamp.max", Stune::background_uclamp_max);
            n++;
        }

        // 补充prefer_idle
        if (!Stune::background_prefer_idle.empty()) {
            utils.FileWrite("/dev/stune/background/schedtune.prefer_idle", Stune::background_prefer_idle);
            n++;
        }
        if (!Stune::system_background_prefer_idle.empty()) {
            utils.FileWrite("/dev/stune/system-background/schedtune.prefer_idle", Stune::system_background_prefer_idle);
            n++;
        }
        if (!Stune::restricted_prefer_idle.empty()) {
            utils.FileWrite("/dev/stune/restricted/schedtune.prefer_idle", Stune::restricted_prefer_idle);
            n++;
        }

        logger.Info(n ? "Stune OK(%d)" : "Stune skip", n);
    }

    bool FeasFunc(bool Enable) {
        if (checkQcomFeas()) {
            utils.FileWrite(qcomFeas, Enable ? "1" : "0");
            logger.Debug("QCOM Feas 已%s", Enable ? "开启" : "关闭");
            return true;
        }

        if (checkMtkFeas()) {
            utils.FileWrite(mtkFeas, Enable ? "1" : "0");
            logger.Debug("MTK Feas 已%s", Enable ? "开启" : "关闭");
            return true;
        }

        return false;
    }

    bool checkQcom() const {
        return !access(qcomGpuPath, F_OK);
    }

    void ioSchedulerControl() {
        if (!IOScheduler::enable) return;

        DIR* dir = opendir("/sys/block/");
        if (!dir) {
            logger.Warn("无法打开 /sys/block/ 目录, IO调度器控制未生效");
            return;
        }

        int deviceCount = 0;
        int appliedCount = 0;
        struct dirent* entry;

        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;

            if (strstr(entry->d_name, "sd") == nullptr &&
                strstr(entry->d_name, "mmcblk") == nullptr) {
                continue;
            }

            deviceCount++;

            char devicePath[256];

            if (!IOScheduler::scheduler.empty()) {
                FastSnprintf(devicePath,
                             sizeof(devicePath),
                             "/sys/block/%s/queue/scheduler",
                             entry->d_name);

                if (!access(devicePath, F_OK)) {
                    if (isSchedulerAvailable(devicePath, IOScheduler::scheduler.c_str())) {
                        utils.FileWrite(devicePath, IOScheduler::scheduler);

                        logger.Debug("块设备 %s IO调度器设置为: %s",
                                     entry->d_name,
                                     IOScheduler::scheduler.c_str());

                        appliedCount++;
                    } else {
                        logger.Debug("块设备 %s 不支持调度器: %s, 已跳过",
                                     entry->d_name,
                                     IOScheduler::scheduler.c_str());
                    }
                }
            }

            if (!IOScheduler::read_ahead_kb.empty()) {
                FastSnprintf(devicePath,
                             sizeof(devicePath),
                             "/sys/block/%s/queue/read_ahead_kb",
                             entry->d_name);

                if (!access(devicePath, F_OK)) {
                    utils.FileWrite(devicePath, IOScheduler::read_ahead_kb);

                    logger.Debug("块设备 %s read_ahead_kb设置为: %s",
                                 entry->d_name,
                                 IOScheduler::read_ahead_kb.c_str());

                    appliedCount++;
                }
            }

            if (!IOScheduler::nr_requests.empty()) {
                FastSnprintf(devicePath,
                             sizeof(devicePath),
                             "/sys/block/%s/queue/nr_requests",
                             entry->d_name);

                if (!access(devicePath, F_OK)) {
                    utils.WriteInt(devicePath, Fastatoi(IOScheduler::nr_requests.c_str()));

                    logger.Debug("块设备 %s nr_requests设置为: %s",
                                 entry->d_name,
                                 IOScheduler::nr_requests.c_str());

                    appliedCount++;
                }
            }
        }

        closedir(dir);

        if (appliedCount > 0) {
            logger.Info("IO调度器已优化 (%d项, %d个块设备)", appliedCount, deviceCount);
        } else {
            logger.Debug("IO调度器未应用任何参数");
        }
    }

private:
    bool checkQcomFeas() const {
        return !access(qcomFeas, F_OK);
    }

    bool checkMtkFeas() const {
        return !access(mtkFeas, F_OK);
    }

    bool checkCpuset() const {
        return !access(cpusetPath, F_OK);
    }

    bool checkEasSched() const {
        return !access(easSchedPath, F_OK);
    }

    bool isSchedulerAvailable(const char* schedulerPath, const char* schedulerName) const {
        char buf[256];

        int fd = open(schedulerPath, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;

        ssize_t len = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (len <= 0) return false;

        buf[len] = '\0';

        return strstr(buf, schedulerName) != nullptr;
    }

    int findBlockDevices(char devices[][32], int maxDevices) const {
        DIR* dir = opendir("/sys/block/");
        if (!dir) return 0;

        int count = 0;
        struct dirent* entry;

        while ((entry = readdir(dir)) != nullptr && count < maxDevices) {
            if (entry->d_name[0] == '.') continue;

            if (strstr(entry->d_name, "sd") || strstr(entry->d_name, "mmcblk")) {
                strncpy(devices[count], entry->d_name, 31);
                devices[count][31] = '\0';
                count++;
            }
        }

        closedir(dir);

        return count;
    }

};