#include "scheduler.hpp"

int main(void) {
    // Schedule 构造函数中完成所有初始化：
    //   1. 进程互斥检测（SIGTERM -> SIGKILL -> 退出检查）
    //   2. 配置加载与初始频率设置
    //   3. WatchDog 看门狗启动
    //   4. 三线程启动（Monitor / Policy / Dispatch）
    //
    // 析构时自动优雅关闭：
    //   1. 发送 SHUTDOWN 事件
    //   2. 等待所有线程退出
    //   3. 停止 WatchDog
    //   4. 恢复温控 / GPU 守护线程
    Schedule sched;

    // 主线程进入休眠，所有工作由三线程 + 事件驱动模型处理
    // Schedule 析构时会优雅关闭所有资源
    while (true) {
        sleep(3600);
    }
}
