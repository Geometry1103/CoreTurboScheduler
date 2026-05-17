# CoreTurboScheduler (CTS) v4.0 — 小白完全指南

> **一句话：让手机自动省电，玩游戏自动变流畅，不用你操心。**

CTS 是一个 Android CPU/GPU 智能调度工具。它会根据你在用什么 App、屏幕亮没亮、是否在触摸屏幕等情况，自动调整 CPU 频率和核心开关，达到省电+流畅的平衡。

---

## 目录
1. [快速上手（3分钟搞定）](#快速上手)
2. [四种模式怎么选](#四种模式怎么选)
3. [配置文件结构](#配置文件结构)
4. [models 详解（核心）](#models-详解)
5. [常见问题](#常见问题)

---

## 快速上手

### 1. 安装后文件位置
```
/sdcard/Android/CTS/
├── config.json      # 主配置文件
├── mode.txt         # 当前模式（powersave/balance/performance/fast）
└── log.txt          # 运行日志
```

### 2. 切换模式（立即生效，不用重启）
```bash
# 用终端或MT管理器执行：
echo "powersave"   > /sdcard/Android/CTS/mode.txt   # 最省电，适合待机
echo "balance"     > /sdcard/Android/CTS/mode.txt   # 推荐日常使用
echo "performance" > /sdcard/Android/CTS/mode.txt   # 偏性能，游戏用
echo "fast"        > /sdcard/Android/CTS/mode.txt   # 风驰模式，极速但费电
```

### 3. 查看日志
```bash
cat /sdcard/Android/CTS/log.txt
```

---

## 四种模式怎么选

| 模式 | 中文名 | 适合场景 | CPU核心 | 特点 |
|------|--------|----------|---------|------|
| **powersave** | 省电模式 | 待机、睡觉、轻度使用 | 0-5核 | 最省电，性能最低 |
| **balance** | 平衡模式 | 日常刷微博、微信、看视频 | 0-6核 | 省电+流畅平衡 |
| **performance** | 性能模式 | 轻度游戏、多任务 | 0-7核 | 性能优先 |
| **fast** | 风驰模式 | 重度游戏、跑分 | 0-7核 | hmbird调速器，极速响应 |

> **小白建议**：平时用 `balance`，玩游戏前切 `fast`，睡觉切 `powersave`。

---

## 配置文件结构

`config.json` 主要包含这几块：

```json
{
    "meta": { ... },           // 基本信息（名字、作者）
    "Policy": { ... },         // CPU分组设置
    "Function": { ... },       // 功能开关
    "package_blacklist": [...],// 黑名单App
    "models": [ ... ]          // 各种场景的频率配置（核心！）
}
```

### 1. meta — 基本信息
```json
"meta": {
    "name": "CTS-GPU",
    "version": "1",
    "author": "你的名字",
    "loglevel": "INFO"
}
```
- `loglevel`: `INFO` 正常用，`DEBUG` 查问题时开

### 2. Policy — CPU分组
```json
"Policy": { "c0": 0, "c1": 6, "c2": -1, "c3": -1 }
```
**怎么查自己手机？**
```bash
ls /sys/devices/system/cpu/cpufreq/
# 看到 policy0 policy6 → 填 "c0":0, "c1":6
# 看到 policy0 policy4 policy7 → 填 "c0":0, "c1":4, "c2":7
```
没有的填 `-1`。

### 3. Function — 功能开关
```json
"Function": {
    "Cpuset": { "enable": true, ... },      // 核心分配
    "SceneDetect": { "enable": true, ... }, // 场景识别（触屏、重负载等）
    "AppProfile": { "enable": true },        // 应用画像（按App调频）
    "GpuFreq": { "enable": true },           // GPU控制
    "NetlinkScreen": { "enable": true }      // 熄屏监听
}
```
**小白建议**：全部保持 `true` 就行，别关。

### 4. package_blacklist — 黑名单
这些 App 不参与智能匹配（避免系统UI、输入法干扰）：
```json
"package_blacklist": [
    "com.android.systemui",
    "com.android.launcher*",
    "com.baidu.input*"
]
```
支持 `*` 通配符。

---

## models 详解

**这是最重要的部分！** `models` 是一个数组，每个元素定义了一套频率配置。

### 基础模型（4个必有的）

```json
{
    "model_name": "powersave",
    "mode": "powersave",           // 对应 mode.txt 的值
    "is_game": false,
    "packages": [],                // 空数组 = 基础模型
    
    // CPU频率（单位：Hz）
    "freq_min_c0": "300000",       // 小核最低频率
    "freq_max_c0": "2000000",      // 小核最高频率
    "freq_min_c1": "0",
    "freq_max_c1": "0",
    
    // GPU频率（单位：MHz）
    "gpu_min_mhz": "0",
    "gpu_max_mhz": "500",
    
    // CPU核心开关（1=开，0=关）
    "CoreOnline": {
        "Core0": 1, "Core1": 1, "Core2": 1, "Core3": 1,
        "Core4": 1, "Core5": 1, "Core6": 0, "Core7": 0   // 省电模式只开6核
    },
    
    // 场景覆盖（可选）
    "Scenes": {
        "Standby": {               // 熄屏时：频率压到最低
            "freq_max_c0": "600000",
            "freq_max_c1": "800000"
        },
        "HeavyLoad": {             // 重负载时：允许更高频率
            "freq_max_c0": "1600000",
            "freq_max_c1": "1800000"
        },
        "AmSwitch": {},            // 切换App时：空=保持基础频率
        "Touch": {},               // 触摸时
        "None": {}                 // 默认状态
    },
    
    // 调速器配置
    "gov_c0": {
        "governor": "walt",
        "params": {
            "hispeed_freq": "787200",
            "hispeed_load": "95",
            "pl": "0"
        }
    },
    "gov_c1": { ... }
}
```

### 字段说明

| 字段 | 说明 | 示例 |
|------|------|------|
| `model_name` | 模型名字 | `"powersave"` |
| `mode` | 对应情景模式 | `"powersave"` 对应 `mode.txt` |
| `is_game` | 是否游戏模型 | `true`/`false` |
| `packages` | 匹配的App包名 | `["com.tencent.*"]`，空数组=基础模型 |
| `freq_min_cN` | 第N簇最低频率 | `"300000"` (300MHz) |
| `freq_max_cN` | 第N簇最高频率 | `"2000000"` (2GHz) |
| `gpu_min_mhz` | GPU最低频率 | `"0"` |
| `gpu_max_mhz` | GPU最高频率 | `"500"` |
| `CoreOnline` | 核心开关 | `{"Core0":1, ...}` |
| `Scenes` | 场景覆盖 | 见下文 |
| `gov_cN` | 调速器配置 | `governor` + `params` |

### 五个场景说明

| 场景 | 触发条件 | 用途 |
|------|----------|------|
| `None` | 默认状态 | 基础频率 |
| `Touch` | 手指触摸屏幕 | 略微提频，响应更快 |
| `AmSwitch` | 切换App瞬间 | 短时提频，让App启动更快 |
| `HeavyLoad` | CPU持续高负载 | 加速完成任务 |
| `Standby` | **屏幕熄灭** | **压低频率，关键省电** |

### 游戏模型（自动匹配游戏App）

```json
{
    "model_name": "game_heavy",
    "is_game": true,
    "packages": [
        "com.tencent.tmgp.sgame",      // 王者荣耀
        "com.miHoYo.Yuanshen",          // 原神
        "com.miHoYo.hkrpg",             // 星铁
        "..."                           // 更多游戏
    ],
    "freq_min_c0": "700000",
    "freq_max_c0": "3600000",
    "freq_min_c1": "700000",
    "freq_max_c1": "4000000",
    "gpu_min_mhz": "0",
    "gpu_max_mhz": "1200",
    "CoreOnline": {
        "Core0": 1, "Core1": 1, "Core2": 1, "Core3": 1,
        "Core4": 1, "Core5": 1, "Core6": 1, "Core7": 1   // 全开
    },
    "gov_c0": {
        "governor": "hmbird",          // 风驰调速器！
        "params": {}
    },
    "gov_c1": {
        "governor": "hmbird",
        "params": {}
    }
}
```

> **风驰模式 (fast) 和游戏都用 `hmbird` 调速器**，响应速度比 `walt` 更快，适合游戏场景。

### 优先级规则

最终的频率按这个顺序决定（从高到低）：

```
1. 应用画像（当前App匹配的model）
   ↓ 没设置就继承
2. 场景覆盖（Scenes里的Standby/Touch等）
   ↓ 没设置就继承
3. 基础模型（powersave/balance/performance/fast）
   ↓ 没设置就继承
4. 系统默认值
```

**举例**：你正在玩王者荣耀
1. 匹配到 `game_heavy` 模型
2. 屏幕亮着 → 用 `game_heavy` 的频率
3. 屏幕熄灭 → 继承 `Scenes.Standby` 的低频率（省电）
4. 手指触摸 → 临时提升到 `Scenes.Touch` 的频率

---

## 常见问题

### Q: 怎么添加新游戏？
找到 `game_heavy` 或 `game_lite`，在 `packages` 里加包名：
```json
"packages": [
    "com.tencent.tmgp.pubgmhd",
    "com.your.new.game"    // 加在这里
]
```

### Q: 配置改了没效果？
1. 检查 JSON 格式（少逗号、引号不匹配会导致解析失败）
2. 看日志：`cat /sdcard/Android/CTS/log.txt`
3. 开 DEBUG 模式：`"loglevel": "DEBUG"`
4. 确认 `mode.txt` 内容正确

### Q: 怎么自定义某个App的频率？
新建一个 model，放在 `models` 数组里（顺序靠前的优先匹配）：
```json
{
    "model_name": "my_app",
    "packages": ["com.example.app"],
    "freq_max_c0": "1500000",
    ...
}
```

### Q: 为什么游戏时CPU频率上不去？
检查：
1. 游戏包名是否在 `game_heavy.packages` 里
2. `AppProfile.enable` 是否为 `true`
3. 游戏是否在 `package_blacklist` 里（会被跳过）

### Q: 核心开关怎么用？
在任意 model 里加 `CoreOnline`：
```json
"CoreOnline": {
    "Core0": 1,  // 开
    "Core1": 1,
    "Core2": 0,  // 关
    "Core3": 0
}
```

---

## 更新日志 v4.4

### 配置变更
- **Switch 合并到 models**：不再单独写 `Switch` 节点，直接在 model 里加 `mode` 和 `Scenes`
- **新增 CoreOnline**：所有基础模型和游戏模型都支持开关 CPU 核心
- **风驰模式统一**：`fast` 模式和游戏模型都使用 `hmbird` 调速器
- **SM8850 游戏包名**：`game_heavy` 内置 33 个主流游戏包名

### Bug 修复
- 修复 `GpuFreq::enable` 编译报错
- 修复核心被误关闭的问题
- 修复 GPU 守护线程重复启动

---

**协议**: GPL-3.0
