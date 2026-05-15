#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

/**
 * FrameAnalyzer -- 帧渲染压力评估
 *
 * 我们没有 GFX HAL 直接接口，因此使用以下信号代替：
 *   1. /proc/<surfaceflinger pid>/sched 中的 wait_sum 增长
 *   2. /proc/<top-app pid>/task/<tid>/comm 找 "RenderThread"，读 sched
 *   3. 兜底：使用窗口期内 cpuLoad 大跳变近似 jank
 *
 * 输出：
 *   - frameJankPct : 0~100  最近 N 次采样卡顿比例
 *   - renderBusyMs : 渲染线程估算耗时
 *
 * 这是轻量近似，不依赖 ATRACE，跑在 root 守护进程下足够。
 */
class FrameAnalyzer {
public:
    struct Sample {
        int frameJankPct = 0;
        int renderBusyMs = 0;
        int sfBusyMs = 0;
        bool valid = false;
    };

    FrameAnalyzer() = default;

    /// 主动告知前台 PID（由 cpuset top-app 切换事件提供）
    void setTopAppPid(int pid) {
        std::lock_guard<std::mutex> lock(mu_);
        topAppPid_ = pid;
        renderTid_ = -1;  // 失效，下次重新查找
    }

    /// 根据上一次 cpu 负载差分喂入一次卡顿判定（最简单兜底）
    void feedCpuJankSignal(int cpuLoad) {
        std::lock_guard<std::mutex> lock(mu_);
        bool jank = (cpuLoad - lastCpuLoad_) > 35;  // 短时大跳变
        lastCpuLoad_ = cpuLoad;
        history_[head_] = jank ? 1 : 0;
        head_ = (head_ + 1) % HISTORY;
        if (count_ < HISTORY) ++count_;
    }

    Sample sample() {
        Sample s{};
        std::lock_guard<std::mutex> lock(mu_);
        if (count_ > 0) {
            int sum = 0;
            for (int i = 0; i < count_; ++i) sum += history_[i];
            s.frameJankPct = (sum * 100) / count_;
        }
        s.renderBusyMs = readRenderBusyMs();
        s.sfBusyMs = readSurfaceFlingerBusyMs();
        s.valid = true;
        return s;
    }

private:
    int readRenderBusyMs() {
        if (topAppPid_ <= 0) return 0;
        if (renderTid_ <= 0) renderTid_ = findRenderThread(topAppPid_);
        if (renderTid_ <= 0) return 0;
        long long sumExec = readSchedSumExec(topAppPid_, renderTid_);
        if (sumExec <= 0) return 0;
        long long delta = sumExec - lastRenderExec_;
        lastRenderExec_ = sumExec;
        if (delta < 0) delta = 0;
        return static_cast<int>(delta / 1000000LL);  // ns → ms
    }

    int readSurfaceFlingerBusyMs() {
        if (sfPid_ <= 0) sfPid_ = findPidByName("surfaceflinger");
        if (sfPid_ <= 0) return 0;
        long long sumExec = readSchedSumExec(sfPid_, sfPid_);
        if (sumExec <= 0) return 0;
        long long delta = sumExec - lastSfExec_;
        lastSfExec_ = sumExec;
        if (delta < 0) delta = 0;
        return static_cast<int>(delta / 1000000LL);
    }

    static long long readSchedSumExec(int pid, int tid) {
        char path[128];
        snprintf(path, sizeof(path), "/proc/%d/task/%d/sched", pid, tid);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) return 0;
        char buf[2048] = {0};
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) return 0;
        buf[n] = '\0';
        const char* p = strstr(buf, "se.sum_exec_runtime");
        if (!p) return 0;
        const char* colon = strchr(p, ':');
        if (!colon) return 0;
        return strtoll(colon + 1, nullptr, 10);
    }

    static int findRenderThread(int pid) {
        char dirPath[64];
        snprintf(dirPath, sizeof(dirPath), "/proc/%d/task", pid);
        DIR* dir = opendir(dirPath);
        if (!dir) return -1;
        int found = -1;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
            char commPath[128];
            snprintf(commPath, sizeof(commPath), "%s/%s/comm", dirPath, ent->d_name);
            int fd = open(commPath, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char buf[64] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';
            if (strstr(buf, "RenderThread") || strstr(buf, "RenderEngine")) {
                found = atoi(ent->d_name);
                break;
            }
        }
        closedir(dir);
        return found;
    }

    static int findPidByName(const char* name) {
        DIR* dir = opendir("/proc");
        if (!dir) return -1;
        int pid = -1;
        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
            char path[128];
            snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            char buf[64] = {0};
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n <= 0) continue;
            buf[n] = '\0';
            // strip newline
            for (int i = 0; i < n; ++i)
                if (buf[i] == '\n' || buf[i] == '\r') { buf[i] = 0; break; }
            if (strcmp(buf, name) == 0) { pid = atoi(ent->d_name); break; }
        }
        closedir(dir);
        return pid;
    }

    static constexpr int HISTORY = 60;  // 最近 60 次采样
    int history_[HISTORY] = {0};
    int head_ = 0;
    int count_ = 0;
    int lastCpuLoad_ = 0;

    int topAppPid_ = -1;
    int renderTid_ = -1;
    int sfPid_ = -1;
    long long lastRenderExec_ = 0;
    long long lastSfExec_ = 0;
    mutable std::mutex mu_;
};
