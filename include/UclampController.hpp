#pragma once

#include <string>
#include <unistd.h>
#include "SysfsBatchWriter.hpp"

/**
 * UclampController -- Android 13+ Util-Clamp 控制
 *
 * 推荐策略（按场景）：
 *   GAME    : top-app uclamp.min = 768
 *   UI      : top-app uclamp.min = 512
 *   VIDEO   : top-app uclamp.min = 256, uclamp.max = 1024
 *   IDLE    : top-app uclamp.min = 0
 *   后台/限制: uclamp.max = 128
 *
 * 写入路径优先级：
 *   1. /dev/cpuctl/<group>/cpu.uclamp.min/max     (Android 13+)
 *   2. /dev/stune/<group>/uclamp.min/max          (旧路径)
 *   3. /proc/sys/kernel/sched_util_clamp_*        (全局)
 */
class UclampController {
public:
    explicit UclampController(SysfsBatchWriter* writer = nullptr) : writer_(writer) {}

    void setBackend(SysfsBatchWriter* w) { writer_ = w; }

    /// 0~1024, -1 = 不修改
    void apply(const std::string& group, int uclampMin, int uclampMax) {
        if (group.empty()) return;
        const std::string base1 = "/dev/cpuctl/" + group + "/";
        const std::string base2 = "/dev/stune/" + group + "/";

        if (uclampMin >= 0) {
            writeOne(base1 + "cpu.uclamp.min", uclampMin);
            writeOne(base2 + "uclamp.min", uclampMin);
        }
        if (uclampMax >= 0) {
            writeOne(base1 + "cpu.uclamp.max", uclampMax);
            writeOne(base2 + "uclamp.max", uclampMax);
        }
    }

    void applyGlobal(int uclampMin, int uclampMax) {
        if (uclampMin >= 0) {
            writeOne("/proc/sys/kernel/sched_util_clamp_min", uclampMin);
        }
        if (uclampMax >= 0) {
            writeOne("/proc/sys/kernel/sched_util_clamp_max", uclampMax);
        }
    }

    /// 预设：游戏
    void presetGame() {
        apply("top-app", 768, 1024);
        apply("foreground", 512, 1024);
        apply("background", 0, 256);
        apply("system-background", 0, 384);
    }

    /// 预设：UI 滑动
    void presetUI() {
        apply("top-app", 512, 1024);
        apply("foreground", 256, 1024);
        apply("background", 0, 200);
        apply("system-background", 0, 384);
    }

    /// 预设：视频播放
    void presetVideo() {
        apply("top-app", 256, 1024);
        apply("foreground", 200, 1024);
        apply("background", 0, 200);
        apply("system-background", 0, 384);
    }

    /// 预设：闲置 / 省电
    void presetIdle() {
        apply("top-app", 0, 1024);
        apply("foreground", 0, 800);
        apply("background", 0, 128);
        apply("system-background", 0, 256);
    }

    /// 预设：重 CPU 工作负载
    void presetHeavyCpu() {
        apply("top-app", 700, 1024);
        apply("foreground", 400, 1024);
        apply("background", 0, 256);
    }

    /// 预设：相机拍摄
    void presetCamera() {
        apply("top-app", 700, 1024);
        apply("foreground", 600, 1024);
        apply("background", 0, 200);
    }

    /// 预设：安装/解压（IO 密集）
    void presetInstall() {
        apply("top-app", 600, 1024);
        apply("foreground", 256, 1024);
        apply("background", 0, 200);
    }

private:
    void writeOne(const std::string& path, int value) {
        if (access(path.c_str(), F_OK) != 0) return;
        std::string v = std::to_string(value);
        if (writer_) writer_->queue(path, v);
        else writeDirect(path, v);
    }

    static void writeDirect(const std::string& path, const std::string& v) {
        int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
        if (fd < 0) return;
        (void)write(fd, v.data(), v.size());
        close(fd);
    }

    SysfsBatchWriter* writer_ = nullptr;
};
