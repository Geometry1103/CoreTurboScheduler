#pragma once

#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include "SysfsBatchWriter.hpp"

/**
 * EASPolicy -- Energy-Aware Scheduler 适配
 *
 * 目标：
 *   - 通过 /proc/sys/kernel/sched_energy_aware 启用 EAS（如果支持）
 *   - 读取每个 CPU 的 energy_model（频率-功耗-效率三元组）
 *   - 提供建议：
 *       常驻       小核
 *       过渡负载   中核
 *       Burst      大核 + uclamp 拉满
 */
class EASPolicy {
public:
    struct OPP {
        int freqMhz;
        int powerMw;
        int capacity;       // capacity_dmips_mhz * freq, 0~1024
        int efficiency;     // capacity / power
    };

    struct ClusterModel {
        int firstCpu = -1;
        int lastCpu = -1;
        std::vector<OPP> opps;
    };

    EASPolicy() {
        easSupported_ = (access("/proc/sys/kernel/sched_energy_aware", F_OK) == 0);
    }

    bool easSupported() const { return easSupported_; }

    bool setEasEnabled(bool enable, SysfsBatchWriter* w = nullptr) {
        if (!easSupported_) return false;
        const char* path = "/proc/sys/kernel/sched_energy_aware";
        const std::string v = enable ? "1" : "0";
        if (w) { w->queue(path, v); return true; }
        int fd = open(path, O_WRONLY | O_CLOEXEC);
        if (fd < 0) return false;
        (void)write(fd, v.data(), v.size());
        close(fd);
        return true;
    }

    /// 扫描 /sys/devices/system/cpu/cpu*/cpu_capacity 以及
    /// /sys/devices/system/cpu/cpufreq/policyN/related_cpus 构建模型。
    void scan() {
        models_.clear();
        for (int p = 0; p < 8; ++p) {
            char relPath[128];
            snprintf(relPath, sizeof(relPath),
                     "/sys/devices/system/cpu/cpufreq/policy%d/related_cpus", p);
            int fd = open(relPath, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char buf[128] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';

            ClusterModel m{};
            int first = -1, last = -1;
            for (char* tok = strtok(buf, " \n\t"); tok; tok = strtok(nullptr, " \n\t")) {
                int c = atoi(tok);
                if (first < 0) first = c;
                last = c;
            }
            m.firstCpu = first;
            m.lastCpu = last;

            // 读取该 cluster 的频率列表（一次读取，两次解析，避免对同一 buffer 多次 strtok）
            char freqsPath[160];
            snprintf(freqsPath, sizeof(freqsPath),
                     "/sys/devices/system/cpu/cpufreq/policy%d/scaling_available_frequencies", p);
            int ffd = open(freqsPath, O_RDONLY | O_CLOEXEC);
            if (ffd >= 0) {
                char fbuf[1024] = {0};
                ssize_t fn = read(ffd, fbuf, sizeof(fbuf) - 1);
                close(ffd);
                if (fn > 0) {
                    fbuf[fn] = '\0';

                    // 解析所有频率到本地数组
                    int freqs[64];
                    int freqCount = 0;
                    for (char* tok = strtok(fbuf, " \n\t"); tok && freqCount < 64;
                         tok = strtok(nullptr, " \n\t")) {
                        int khz = atoi(tok);
                        if (khz > 0) freqs[freqCount++] = khz;
                    }

                    int cap = readCpuCapacity(first);
                    int maxFreq = 0;
                    for (int i = 0; i < freqCount; ++i)
                        if (freqs[i] > maxFreq) maxFreq = freqs[i];
                    if (maxFreq <= 0) maxFreq = 1;

                    for (int i = 0; i < freqCount; ++i) {
                        OPP opp{};
                        opp.freqMhz = freqs[i] / 1000;
                        opp.capacity = cap > 0 ? (cap * freqs[i]) / maxFreq : 0;
                        opp.powerMw = estimatePowerMw(opp.capacity, opp.freqMhz);
                        opp.efficiency = opp.powerMw > 0
                                            ? (opp.capacity * 100) / opp.powerMw
                                            : 0;
                        m.opps.push_back(opp);
                    }
                }
            }
            models_.push_back(std::move(m));
        }
    }

    const std::vector<ClusterModel>& models() const { return models_; }

    /// 给定目标负载（0~1024），推荐最高效（efficiency）的 cluster index
    int recommendCluster(int targetCapacity) const {
        int best = 0;
        int bestEff = -1;
        for (size_t i = 0; i < models_.size(); ++i) {
            const auto& m = models_[i];
            if (m.opps.empty()) continue;
            const auto& topOpp = m.opps.back();
            if (topOpp.capacity < targetCapacity) continue;
            // 找该 cluster 中刚好覆盖 targetCapacity 的 OPP
            int eff = -1;
            for (const auto& opp : m.opps) {
                if (opp.capacity >= targetCapacity && opp.efficiency > eff) {
                    eff = opp.efficiency;
                }
            }
            if (eff > bestEff) {
                bestEff = eff;
                best = static_cast<int>(i);
            }
        }
        return best;
    }

private:
    static int readCpuCapacity(int cpu) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        char buf[16] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return 0;
        return atoi(buf);
    }

    /// 简易功耗近似：power ∝ capacity * freq^1.6
    static int estimatePowerMw(int capacity, int freqMhz) {
        if (capacity <= 0 || freqMhz <= 0) return 0;
        double p = (capacity / 1024.0) * (freqMhz / 1000.0) * (freqMhz / 1000.0);
        return static_cast<int>(p * 1500.0);
    }

    bool easSupported_ = false;
    std::vector<ClusterModel> models_;
};
