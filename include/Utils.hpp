#pragma once

#include <iostream>
#include <fstream>
#include <cstring>
#include <thread>
#include <string>
#include <vector>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <stdarg.h>
#include <stddef.h>
#include <chrono>
#include <cstdio>
#include <sys/inotify.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <string_view>
#include <unordered_map>
#include <functional>
#include <sys/system_properties.h>
#include <mutex>
#include <atomic>
#include <memory>
#include "LibUtils.hpp"
#include "Json/string.hpp"

// Build configuration
#define DEBUG_DURATION 0
#define MAX_PKG_LEN 128
#define MAX_THREAD_LEN 128
#define CPU_POLICY 8 

using namespace LibUtils;

using string_t = qlib::string_t;

using std::atomic;
using std::stringstream;
using std::unordered_map;
using std::lock_guard;
using std::unique_ptr;
using std::ifstream;
using std::vector;
using std::string;
using std::string_view;
using std::thread;
using std::mutex;
using std::exception;
using std::make_unique;
using std::to_string;
using std::move;

enum class LOG_LEVEL : uint32_t {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3,
};

class Utils {
private:
    static constexpr const char* thermalPath = "/sys/class/thermal";
    static constexpr int maxBucketSize = 32;
    std::mutex cacheMutex;
    std::unordered_map<std::string,std::string> writeCache;
public:
    bool CachedWrite(const std::string& path,const std::string& value) noexcept {
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = writeCache.find(path);
            if(it != writeCache.end() && it->second == value) {
                return true;
            }
            writeCache[path] = value;
        }
        FileWrite(path, value);
        return true;
    }

    void FileWrite(const char* filePath, const char* content) noexcept {
        int fd = open(filePath, O_WRONLY | O_NONBLOCK, 0666);

        if (fd < 0) {
            chmod(filePath, 0666);
            fd = open(filePath, O_WRONLY | O_CREAT | O_NONBLOCK); 
        }

        if (fd >= 0) {
            write(fd, content, Faststrlen(content));
            close(fd);
        }
    }

    
    void FileWrite(const string& filePath, const string& content) noexcept {
        int fd = open(filePath.c_str(), O_WRONLY | O_NONBLOCK);

        if (fd < 0) {
            chmod(filePath.c_str(), 0666);
            fd = open(filePath.c_str(), O_WRONLY | O_CREAT | O_NONBLOCK); 
        }

        if (fd >= 0) {
            write(fd, content.data(), content.size());
            close(fd);
            chmod(filePath.c_str(), 0444);
        }
    }

        
    void FileWrite(const char* filePath, const string_t& content) noexcept {
        int fd = open(filePath, O_WRONLY | O_NONBLOCK);

        if (fd < 0) {
            chmod(filePath, 0666);
            fd = open(filePath, O_WRONLY | O_CREAT | O_NONBLOCK); 
        }

        if (fd >= 0) {
            write(fd, content.data(), content.size());
            close(fd);
        }
    }


    void WriteFile(const char* filePath, const char* content) noexcept {
        int fd = open(filePath, O_WRONLY | O_TRUNC | O_CREAT, 0666);

        if (fd < 0) {
            // Try to create parent directories
            char pathCopy[256];
            strncpy(pathCopy, filePath, sizeof(pathCopy) - 1);
            pathCopy[sizeof(pathCopy) - 1] = '\0';
            char* lastSlash = strrchr(pathCopy, '/');
            if (lastSlash && lastSlash != pathCopy) {
                *lastSlash = '\0';
                mkdirRecursive(pathCopy);
            }
            fd = open(filePath, O_WRONLY | O_TRUNC | O_CREAT, 0666);
        }

        if (fd >= 0) {
            write(fd, content, Faststrlen(content));
            close(fd);
        }
    }

    void InotifyMain(const char* dir_name, uint32_t mask) {
        int fd = inotify_init();
        if (fd < 0) {
            printf("Failed to initialize inotify.\n");
            exit(-1);
        }

        int wd = inotify_add_watch(fd, dir_name, mask);
        if (wd < 0) {
            printf("Failed to add watch for directory: %s",dir_name);
            close(fd);
            exit(-1);
        }

        const int buflen = sizeof(struct inotify_event) + NAME_MAX + 1;
        char buf[buflen];
        fd_set readfds;

        while (true) {
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);

            int iRet = select(fd + 1, &readfds, nullptr, nullptr, nullptr);
            if (iRet < 0) {
                break;
            }

            int len = read(fd, buf, buflen);
            if (len < 0) {
                printf("Failed to read inotify events.\n");
                break;
            }

            const struct inotify_event *event = reinterpret_cast<const struct inotify_event *>(buf);
            if (event->mask & mask) {
                break;
            }
        }

        inotify_rm_watch(fd, wd);
        close(fd);
    }

    void sleep_ms(const int ms) {
        usleep(1000 * ms);
    }

    bool exec(const char* cmd) {
        auto fp = popen(cmd, "r");
        if (fp == nullptr) return false;
        pclose(fp);
        return true;
    }

    int readInt(const char* path) noexcept {
        auto fd = open(path, O_RDONLY);
        if (fd < 0) return 0;

        char buff[16] = { 0 };
        auto len = read(fd, buff, sizeof(buff));
        close(fd);

        if (len <= 0) return 0;
    
        buff[15] = 0;
        return atoi(buff);
    }

    void WriteInt(const char* path, int value) noexcept {
        auto fd = open(path, O_WRONLY);
        if (fd < 0) {
            chmod(path, 0666);
            fd = open(path, O_WRONLY);
        }
        if (fd < 0) return;  // Fix: 两次 open 均失败时直接返回，避免 write(-1,...) UB

        char tmp[16];
        auto len = FastSnprintf(tmp, sizeof(tmp), "%d", value);
        write(fd, tmp, len);
        close(fd);
        chmod(path, 0444);
    }

    int getPid(const char* processName) {
        DIR *dir = opendir("/proc");
        if (dir == nullptr) {
            printf("ERROR:无法打开 /proc 目录\n");
            return -1; 
        }

        struct dirent* file;
        int pid = -1;

        while ((file = readdir(dir)) != nullptr) {
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;
            char cmdlinePath[32] = "/proc/";
            char readBuff[256];
            size_t len = Faststrlen(file->d_name);

            memcpy(cmdlinePath + 6, file->d_name, len);
            memcpy(cmdlinePath + len + 6, "/cmdline", 9);


            if (readString(cmdlinePath, readBuff, sizeof(readBuff) - 1) <= 0) continue;
            const char *slash = strrchr(readBuff, '/');
            const char *proc_name = slash ? slash + 1 : readBuff;
            long base_len = Faststrlen(proc_name);

            if (base_len == Faststrlen(processName) && !memcmp(proc_name, processName, Faststrlen(processName))) {
                pid = Fastatoi(file->d_name);
                break;
            }
        }
        closedir(dir);
        return pid;
    }

    int getTid(const char* processName, const char* comm) {
        DIR *dir = opendir("/proc");
        if (dir == nullptr) {
            printf("ERROR: 无法打开 /proc 目录\n");
            return -1;
        }
    
        struct dirent* file;
        int tid = -1;
    
        while ((file = readdir(dir)) != nullptr) {
            if (file->d_name[0] < '0' || file->d_name[0] > '9') continue;
            char cmdlinePath[256] = "/proc/";
            size_t pid_len = strlen(file->d_name);
            memcpy(cmdlinePath + 6, file->d_name, pid_len);
            memcpy(cmdlinePath + 6 + pid_len, "/cmdline", 8); 
    
            cmdlinePath[14 + pid_len] = '\0';

            char readBuff[256];
            if (readString(cmdlinePath, readBuff, sizeof(readBuff)) <= 0) continue;
    
            const char *slash = strrchr(readBuff, '/');
            const char *proc_name = slash ? slash + 1 : readBuff;
    
            if (strcmp(proc_name, processName) != 0) continue;
    
            //int pid = atoi(file->d_name);
    
            char taskPath[256] = "/proc/";
            memcpy(taskPath + 6, file->d_name, pid_len);
            memcpy(taskPath + 6 + pid_len, "/task", 5);
            taskPath[12 + pid_len] = '\0';
    
            DIR *taskDir = opendir(taskPath);
            if (taskDir == nullptr) continue;
    
            struct dirent *taskFile;
            while ((taskFile = readdir(taskDir)) != nullptr) {
                if (taskFile->d_name[0] < '0' || taskFile->d_name[0] > '9') continue;

                char commPath[256] = "/proc/";
                memcpy(commPath + 6, file->d_name, pid_len);
                memcpy(commPath + 6 + pid_len, "/task/", 7);

                size_t tid_len = strlen(taskFile->d_name);
                memcpy(commPath + 12 + pid_len, taskFile->d_name, tid_len);
                memcpy(commPath + 12 + pid_len + tid_len, "/comm", 5); 

                commPath[18 + pid_len + tid_len] = '\0';

                char Comm[256];
                if (readString(commPath, Comm, sizeof(Comm)) <= 0) continue;
    
                Comm[strcspn(Comm, "\n")] = '\0';
    
                if (!strcmp(Comm, comm)) {
                    tid = atoi(taskFile->d_name);
                    printf("进程: %s 线程: %s PID: %s TID: %d)\n", processName, comm, file->d_name, tid);
                    closedir(taskDir);
                    closedir(dir);
                    return tid;
                }
            }
            closedir(taskDir);
        }
        closedir(dir);
        return tid;
    }

    int getScreenProperty() {
        static const prop_info* pi = nullptr;

        if (pi == nullptr) {
            pi = __system_property_find("debug.tracing.screen_state");
            if (pi == nullptr) {
                return -1;
            }
        }

        char name[PROP_NAME_MAX] = { 0 };
        char res[PROP_VALUE_MAX] = { 0 };
        __system_property_read(pi, name, res);

        return res[0] ? res[0] - '0' : -1;
    }


    int readFrequencies(const char* path, int frequencies[], int maxCount) noexcept {
        auto fd = open(path, O_RDONLY);
        if (fd < 0) return 0;

        char buff[1024];
        auto len = read(fd, buff, sizeof(buff) - 1);
        close(fd);
        if (len <= 0) return 0;

        buff[len] = '\0';
        
        int count = 0;
        for (char* token = strtok(buff, " \n\t"); token && count < maxCount; token = strtok(nullptr, " \n\t")) {
            int freq = atoi(token);
            if (freq > 0) frequencies[count++] = freq;
        }
        
        return count;
    }
    
    int openZonePath(const char* zoneName) {
        char path[256] = "/sys/class/thermal/";

        size_t zoneLen = strlen(zoneName);
        memcpy(path + 19, zoneName, zoneLen);
        memcpy(path + 19 + zoneLen, "/type", 6);

        auto fd = open(path, O_RDONLY);
        if (fd < 0) return -1;

        char buf[64];
        auto n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        
        if (n <= 0) return -1;
        
        buf[n - 1] = '\0';
        
        if (!checkSensorPath(buf)) return -1;

        memcpy(path + 19 + zoneLen, "/temp", 6);
        return open(path, O_RDONLY);
    }
    
    int readTemp(int fd) {
        char buf[32] = {0};

        lseek(fd, 0, SEEK_SET);
        auto n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n <= 0) return 0;
        buf[31] = '\0';
        return atoi(buf);
    }


    int getMaxCpuTemp() {
        int maxTemp = -1;
        auto dir = opendir(thermalPath);

        if (dir == nullptr) {
            printf("无法打开文件夹:%s\n", thermalPath);
            return -1;
        }

        struct dirent* entry;
        while((entry = readdir(dir)) != nullptr) {
            // [Fix v4.3] 原版逻辑反了：!strncmp == 0 是"匹配"，应该 continue 不匹配的项
            if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
            auto fd = openZonePath(entry->d_name);
            if (fd < 0) continue;
            int currentTemp = readTemp(fd);
            if (currentTemp > 0 && (maxTemp < 0 || currentTemp > maxTemp)) {
                maxTemp = currentTemp;
            }
        }

        closedir(dir);
        return (maxTemp != -1) ? maxTemp / 1000 : -1; // 舍去部分小数
    }

    // [Fix v4.3] 原版漏了大括号导致 pclose 在 while 循环体内，第二次 fgets 用已关闭 FILE* → UB
    // 只读第一行（dumpsys window | grep mCurrentFocus 输出本就只有一行）
    void popenShell(const char* cmd, char* buf, size_t buf_size) {
        if (!buf || buf_size == 0) return;
        buf[0] = '\0';
        auto fp = popen(cmd, "r");
        if (!fp) return;
        if (fgets(buf, buf_size, fp) == nullptr) buf[0] = '\0';
        pclose(fp);
    }
    
    string getActivity() {
        char str[256] = { 0 };  // Fix: 初始化，防止 popen 失败时读垃圾内存
        popenShell("dumpsys window | grep mCurrentFocus", str, sizeof(str));
        if (strstr(str, "mCurrentFocus=null")) return "null";

        // Fix: strstr/strchr 返回 nullptr 时直接 +offset 会 segfault，必须先判空
        const char* slash = strstr(str, "/");
        if (!slash) return "null";
        const char* ptr = slash + 1;

        const char* end_pos = strchr(ptr, '}');
        if (!end_pos) return "null";

        char activity[256];
        ptrdiff_t len = end_pos - ptr;
        if (len <= 0 || len >= (ptrdiff_t)sizeof(activity)) return "null";
        memcpy(activity, ptr, len);
        activity[len] = '\0';
        return string(activity);
    }

    string getTopApp() {
        char data[256] = { 0 };  // Fix: 初始化，防止 popen 失败时读垃圾内存
        popenShell("dumpsys window | grep mCurrentFocus", data, sizeof(data));
        if (strstr(data, "mCurrentFocus=null")) return "null";

        // Fix: strstr/strchr 返回 nullptr 时直接 +offset 会 segfault，必须先判空
        const char* u0pos = strstr(data, "u0");
        if (!u0pos) return "null";
        const char* ptr = u0pos + 3;

        const char* end_pos = strchr(ptr, '/');
        if (!end_pos) return "null";

        char temp[256];
        ptrdiff_t len = end_pos - ptr;
        if (len <= 0 || len >= (ptrdiff_t)sizeof(temp)) return "null";
        memcpy(temp, ptr, len);
        temp[len] = '\0';
        return string(temp);
    }

    // [新增 v4.3] 带 TTL 缓存的 getTopApp，省电关键
    //   dumpsys 是个昂贵操作（每次约 100~300ms），频繁调用很费电。
    //   minIntervalMs 内的请求直接返回上次结果，调用方应配合 inotify 触发
    string getTopAppCached(int minIntervalMs = 1500) {
        using clock = std::chrono::steady_clock;
        static std::mutex   m;
        static std::string  lastResult = "null";
        static clock::time_point lastTime{};
        auto now = clock::now();
        {
            std::lock_guard<std::mutex> lk(m);
            auto since = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTime).count();
            if (since < minIntervalMs && !lastResult.empty()) return lastResult;
        }
        std::string r = getTopApp();
        {
            std::lock_guard<std::mutex> lk(m);
            lastResult = r;
            lastTime   = now;
        }
        return r;
    }

    size_t popenRead(const char* cmd, char* buf, size_t len) {
        auto fp = popen(cmd, "r");
        if (!fp) return 0;
        // Leave 1 byte for null terminator to prevent strtok/printf overflow
        auto readLen = fread(buf, 1, len - 1, fp);
        pclose(fp);
        buf[readLen] = '\0';  // CRITICAL: null-terminate to prevent buffer overflow
        return readLen;
    }
    
    size_t readString(const char* path, char* buff, const size_t maxLen) {
        if (maxLen == 0) return 0;
        auto fd = open(path, O_RDONLY);
        if (fd < 0) {  // Fix: fd <= 0 误判，fd=0(stdin) 是合法 fd，应判 fd < 0
            buff[0] = 0;
            return 0;
        }
        ssize_t len = read(fd, buff, maxLen - 1);  // Fix: 最多读 maxLen-1，为 '\0' 留一位
        close(fd);
        if (len <= 0) {
            buff[0] = 0;
            return 0;
        }
        buff[len] = 0;  // 现在 len <= maxLen-1，不会越界
        return (size_t)(len);
    }

    void mkdirRecursive(const char* dirPath) {
        char tmp[256];
        strncpy(tmp, dirPath, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        size_t len = strlen(tmp);
        if (len == 0) return;
        if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
        
        for (char* p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                mkdir(tmp, 0755);
                *p = '/';
            }
        }
        mkdir(tmp, 0755);
    }
private: 
    bool checkSensorPath(const char* str) {
        return strstr(str, "soc_max") != nullptr || strstr(str, "mtktscpu") != nullptr || strstr(str, "cpu-1-") != nullptr;
    }
};