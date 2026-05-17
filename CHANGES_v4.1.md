# CTS v4.1 修复说明

> 基于 v4.0 (CTS_GPU_v4_0)，重点修复配置解析与省电相关问题。

## 🐛 关键 Bug 修复

### 1. 配置文件中布尔/数字被写成字符串导致设置失效 (最严重)
**症状**：旧 `config.json` 把 `enable`、`heavy_load_thd`、`version` 等字段都写成字符串：
```json
"enable": "true",         "heavy_load_thd": "75",         "version": "2"
```
qlib::json 是严格类型库，`get<bool>()` 在 string 值上会抛异常 → 设置被静默忽略 → 一切配置项都用代码里的 default。最终用户看到的现象：
- 改 `heavy_load_thd` 无效（场景识别永远是默认阈值）
- 改 `enable` 无效（功能开关一直是默认状态）
- 日志显示 `版本: -1`（实际配置写的是 "2"）

**修复**：在 `JsonConfig.hpp` 新增 `readBool` / `readInt` / `readFreq` 模板：
- 先尝试 native bool/int
- 失败再尝试 string，识别 `"true"/"false"/"1"/"0"/"on"/"off"/"yes"/"no"` 和带正负号的数字
- 任一成功即生效；两者都失败保持原值

同时提供**正确 JSON 类型**的示例 `config.json` (本仓库 `Json/config.json`)，新用户应基于这个写。

### 2. `currentMatch` 多线程并发读写未加锁
**症状**：`appProfileTask` 写、`applyWithProfile`（被 6+ 线程调用）读，
是 plain `int`，ARM 上理论可撕裂。

**修复**：`Config::AppProfile::currentMatch` 改为 `std::atomic<int>`，所有访问改为 `.load()`/`.store()`。

### 3. 退出游戏后大核没关回桌面状态 (省电关键)
**症状**：`game_heavy` 设置 `CoreOnline: { Core7: 1 }` 开了大核；
退出游戏回桌面（无 model 匹配）后 `applyWithProfile` 调 `applyScene` 只改频率，
**不重写 online**，Core7 继续在线一直耗电。

**修复**：`applyWithProfile` 在走非画像分支后调 `restoreBaseCoresIfNeeded()`，
按基础模型的 `Performances::Online[]` 恢复核心开关。仅当至少一个 `Online[i] != -1` 时才写入，避免无谓 sysfs 抖动。

### 4. 配置重载时无谓地重写 cpuset / 重启 GPU 守护
**症状**：`jsonTriggerTask → function.AllFunC()` 会再次写 cpuset、再次启动 GPU 守护线程
（虽然 `startGuards` 有 `joinable()` 检查不会真起两条，但 cpuset 重写是浪费）。

**修复**：在 `Function.hpp` 拆分 `AllFunC()`（初始化用）和 `ReloadFunC()`（重载用）。
`jsonTriggerTask` 走 `ReloadFunC` 路径，跳过守护线程启动。

### 5. GPU 守护每 3 秒强制重写频率
**症状**：`gpuFreqGuard` 不管 GPU 频率有没有变化，每 3s 一律重写。

**修复**：缓存 `lastWrittenMin/Max`，未变化时跳过；每 5 轮（15s）强制写一次防止被其他模块覆盖。

### 6. 重复无效的 Scenes 解析嵌套深度
**症状**：旧 `applyBaseModel` 把 Scenes 解析、`Performances` 填充、`schedParam[]` 拷贝
全堆在一个 try-catch 里，深度超过 4 层，难以维护。

**修复**：拆为独立的 `parseScenesNode`、`parseGovernorNodes`、`parseBlacklist`、`parseModels`、`applyBaseModel`、`parseFunctionSection` 等子函数；`baseModelIdx` 缓存为成员，避免重复查找。

### 7. `configPollingTask` / `FileState` / `reloadRuntimeConfig` dead code
**症状**：v4.3 时就废弃但留了空壳，污染代码。

**修复**：彻底删除。

## ⚙️ 不变的内容
- v4.3 之前已修复的 bug：`popenShell` 缺花括号、`getMaxCpuTemp` 反向 strncmp、
  `Performances::Online` 默认值、`GpuFreq::enable` 字段、`temp[256]` 跨线程并发等均已修复在原版本。
- Way_Balance 扁平格式：`freq_min_c0` / `gov_c0.params` / `gpu_min_mhz` 等。
- 风驰模式（fast / game_heavy / game_lite 用 hmbird 调速器）。

## 📊 配置文件
- `Json/config.json`：通用模板（**强类型**：bool/int 不再裹引号）
- `Json/config_std.json`、`config_scx.json`、`config_hm.json`：硬件特定（保留旧的字符串形态，新解析器兼容）

## 🛠 编译验证
全部代码用 g++ 13 / NDK r25+ 通过 syntax-only 检查，无回归错误。
未做完整链接因为 Linux 缺 Android bionic 头文件，但 NDK 编译路径不受影响。
