#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>

/**
 * ThermalEngine -- 热预测引擎
 *
 * 输入：
 *   - 周期采样 SoC/CPU 温度（℃）
 *   - 周期采样 thermal_pressure（每核）
 *
 * 输出：
 *   - currentTemp、futureTemp（线性外推 + 趋势权重）
 *   - 4 档行为建议：BURST / NORMAL / LIMIT / COOL
 *
 * 公式：
 *   futureTemp = current + slope * lookAheadSec * thermalFactor
 *   slope = (lastN 温度的线性回归斜率)
 */
class ThermalEngine {
public:
    enum class Action {
        BURST,   // 低温且趋势平稳 → 允许突发
        NORMAL,  // 稳定 → 维持
        LIMIT,   // 升温较快 → 提前降频
        COOL     // 已经过热 → 强降频
    };

    struct Snapshot {
        int currentTempC = 0;
        int predictedTempC = 0;
        int slopeMilliC = 0;          // 斜率 ×1000（每秒变化的毫℃）
        int avgPressure = 0;          // 全核平均 thermal_pressure
        Action action = Action::NORMAL;
    };

    ThermalEngine() {
        head_ = 0;
        count_ = 0;
        for (int i = 0; i < HISTORY; ++i) history_[i] = 0;
    }

    /// 推入一次温度采样（℃整数），返回当前快照建议。
    Snapshot push(int currentTempC) {
        std::lock_guard<std::mutex> lock(mu_);
        history_[head_] = currentTempC;
        head_ = (head_ + 1) % HISTORY;
        if (count_ < HISTORY) ++count_;

        Snapshot snap;
        snap.currentTempC = currentTempC;
        snap.slopeMilliC = computeSlopeMilliC();
        const double slopePerSec = snap.slopeMilliC / 1000.0;
        snap.predictedTempC = currentTempC +
            static_cast<int>(slopePerSec * lookAheadSec_ * thermalFactor_);
        snap.avgPressure = readAvgThermalPressure();
        snap.action = decide(currentTempC, snap.predictedTempC,
                             snap.slopeMilliC, snap.avgPressure);
        latest_ = snap;
        return snap;
    }

    Snapshot latest() const {
        std::lock_guard<std::mutex> lock(mu_);
        return latest_;
    }

    /// 读取最高 SoC 温度（℃），失败返回 -1
    static int readMaxSocTempC() {
        DIR* dir = opendir("/sys/class/thermal");
        if (!dir) return -1;
        int maxT = -1;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (strncmp(ent->d_name, "thermal_zone", 12) != 0) continue;
            char typePath[256];
            snprintf(typePath, sizeof(typePath),
                     "/sys/class/thermal/%s/type", ent->d_name);
            int tfd = open(typePath, O_RDONLY | O_CLOEXEC);
            if (tfd < 0) continue;
            char tbuf[64] = {0};
            ssize_t n = read(tfd, tbuf, sizeof(tbuf) - 1);
            close(tfd);
            if (n <= 0) continue;
            tbuf[n] = '\0';
            // 仅采样 CPU/SoC 类型
            if (!strstr(tbuf, "cpu") && !strstr(tbuf, "soc") &&
                !strstr(tbuf, "tsens") && !strstr(tbuf, "mtktscpu")) {
                continue;
            }
            char tempPath[256];
            snprintf(tempPath, sizeof(tempPath),
                     "/sys/class/thermal/%s/temp", ent->d_name);
            int fd = open(tempPath, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char buf[32] = {0};
            ssize_t m = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (m <= 0) continue;
            buf[m] = '\0';
            int t = atoi(buf);
            // 大多数内核以 mC 上报
            if (t > 1000) t /= 1000;
            if (t > maxT) maxT = t;
        }
        closedir(dir);
        return maxT;
    }

    /// 读取平均 thermal_pressure（来自每核 sched）
    static int readAvgThermalPressure() {
        int sum = 0, cnt = 0;
        for (int i = 0; i < 8; ++i) {
            char path[96];
            snprintf(path, sizeof(path),
                     "/sys/devices/system/cpu/cpu%d/thermal_pressure", i);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char buf[16] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';
            sum += atoi(buf);
            ++cnt;
        }
        return cnt ? (sum / cnt) : 0;
    }

    void setThresholds(int warnC, int hotC, int criticalC) {
        std::lock_guard<std::mutex> lock(mu_);
        warnC_ = warnC;
        hotC_ = hotC;
        criticalC_ = criticalC;
    }

    void setLookAhead(double seconds, double thermalFactor = 1.0) {
        std::lock_guard<std::mutex> lock(mu_);
        lookAheadSec_ = seconds;
        thermalFactor_ = thermalFactor;
    }

    static const char* actionName(Action a) {
        switch (a) {
            case Action::BURST:  return "BURST";
            case Action::NORMAL: return "NORMAL";
            case Action::LIMIT:  return "LIMIT";
            case Action::COOL:   return "COOL";
        }
        return "?";
    }

private:
    int computeSlopeMilliC() const {
        // 简单线性回归（x 单位为采样间隔 1s 的虚拟刻度）
        if (count_ < 2) return 0;
        double sumX = 0, sumY = 0, sumXY = 0, sumXX = 0;
        for (int i = 0; i < count_; ++i) {
            int idx = (head_ - count_ + i + HISTORY) % HISTORY;
            double x = i;
            double y = history_[idx];
            sumX += x; sumY += y;
            sumXY += x * y;
            sumXX += x * x;
        }
        double n = count_;
        double denom = (n * sumXX - sumX * sumX);
        if (denom < 1e-9) return 0;
        double slope = (n * sumXY - sumX * sumY) / denom;
        return static_cast<int>(slope * 1000.0);
    }

    Action decide(int cur, int predicted, int slopeMilliC, int pressure) const {
        if (cur >= criticalC_ || predicted >= criticalC_ + 2) return Action::COOL;
        if (cur >= hotC_ || predicted >= hotC_ + 1 || pressure > 200)
            return Action::LIMIT;
        if (cur < warnC_ - 4 && slopeMilliC < 200) return Action::BURST;
        return Action::NORMAL;
    }

    static constexpr int HISTORY = 12;
    int history_[HISTORY];
    int head_;
    int count_;

    int warnC_ = 50;
    int hotC_ = 65;
    int criticalC_ = 80;
    double lookAheadSec_ = 3.0;
    double thermalFactor_ = 1.0;

    Snapshot latest_{};
    mutable std::mutex mu_;
};
