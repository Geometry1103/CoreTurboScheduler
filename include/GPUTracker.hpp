#pragma once

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <chrono>
#include <mutex>

/**
 * GPUTracker -- 跨厂商 GPU 占用率与频率读取
 *
 * 支持：
 *   - Qualcomm Adreno: /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage
 *                       /sys/class/kgsl/kgsl-3d0/gpubusy
 *   - Mali (devfreq):  /sys/class/devfreq/.../load
 *   - 通用 devfreq:    /sys/class/devfreq/{gpu,*.gpu*}/cur_freq
 *
 * 输出：
 *   - busyPercent: 0~100
 *   - curFreqMhz:  当前 GPU 频率
 */
class GPUTracker {
public:
    struct Sample {
        int busyPercent = 0;     // 0~100
        int curFreqMhz = 0;
        bool valid = false;
    };

    GPUTracker() {
        detectPaths();
    }

    /// 读取一次采样。线程安全。
    Sample sample() {
        Sample s{};
        s.busyPercent = readBusy();
        s.curFreqMhz = readCurFreqMhz();
        s.valid = (s.busyPercent >= 0);
        if (!s.valid) s.busyPercent = 0;
        return s;
    }

    /// 仅返回繁忙百分比，找不到时返回 -1
    int busyPercent() {
        return readBusy();
    }

    /// 当前频率（MHz）。找不到时返回 0。
    int curFreqMhz() {
        return readCurFreqMhz();
    }

    /// 检测到的 busy 路径
    const char* busyPath() const { return busyPath_; }
    const char* freqPath() const { return freqPath_; }

private:
    void detectPaths() {
        // Adreno first
        const char* adrenoBusy[] = {
            "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
            "/sys/class/kgsl/kgsl-3d0/gpubusy",
            nullptr
        };
        for (int i = 0; adrenoBusy[i]; ++i) {
            if (access(adrenoBusy[i], R_OK) == 0) {
                busyPath_ = adrenoBusy[i];
                break;
            }
        }
        const char* adrenoFreq[] = {
            "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
            "/sys/class/kgsl/kgsl-3d0/gpuclk",
            nullptr
        };
        for (int i = 0; adrenoFreq[i]; ++i) {
            if (access(adrenoFreq[i], R_OK) == 0) {
                freqPath_ = adrenoFreq[i];
                break;
            }
        }

        // Mali / generic devfreq
        if (!busyPath_) {
            const char* maliBusy[] = {
                "/sys/devices/platform/gpu/utilization",
                "/sys/kernel/gpu/gpu_busy",
                "/sys/class/devfreq/gpu/load",
                nullptr
            };
            for (int i = 0; maliBusy[i]; ++i) {
                if (access(maliBusy[i], R_OK) == 0) {
                    busyPath_ = maliBusy[i];
                    break;
                }
            }
        }
        if (!freqPath_) {
            const char* maliFreq[] = {
                "/sys/kernel/gpu/gpu_clock",
                "/sys/class/devfreq/gpu/cur_freq",
                nullptr
            };
            for (int i = 0; maliFreq[i]; ++i) {
                if (access(maliFreq[i], R_OK) == 0) {
                    freqPath_ = maliFreq[i];
                    break;
                }
            }
        }
    }

    int readBusy() {
        if (!busyPath_) return -1;
        char buf[64] = {0};
        int fd = open(busyPath_, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return -1;
        buf[n] = '\0';
        // gpubusy format: "<busy_us> <total_us>"
        // gpu_busy_percentage format: "<percent>"
        int a = 0, b = 0;
        int parsed = sscanf(buf, "%d %d", &a, &b);
        if (parsed == 2 && b > 0) {
            int pct = static_cast<int>((static_cast<long long>(a) * 100LL) / b);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            return pct;
        }
        if (parsed >= 1) {
            if (a < 0) a = 0;
            if (a > 100) a = 100;
            return a;
        }
        return -1;
    }

    int readCurFreqMhz() {
        if (!freqPath_) return 0;
        char buf[32] = {0};
        int fd = open(freqPath_, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return 0;
        buf[n] = '\0';
        long long hz = atoll(buf);
        if (hz <= 0) return 0;
        // Heuristic: > 1e6 means Hz, else already MHz
        if (hz > 1000000LL) return static_cast<int>(hz / 1000000LL);
        if (hz > 1000LL) return static_cast<int>(hz / 1000LL);
        return static_cast<int>(hz);
    }

private:
    const char* busyPath_ = nullptr;
    const char* freqPath_ = nullptr;
};
