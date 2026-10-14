# AGENTS.md

面向 AI agent 的协作契约：任务流程、硬约束、禁止事项、发布规范。
目标是**准确**和**无歧义**，不考虑人类可读性。人类使用说明见 [README.md](README.md)。

本项目由 agent 维护 —— 代码、文档、发布流程都是。

## 任务流程

1. **改前先读** [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，确认改的是哪一层
2. **改完必跑** [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) 的「验收」全流程
3. **提交** —— 补丁有改动时必须先 commit（见 C2）
4. **发版** —— 按 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) 的「发布」，同步更新 CHANGELOG

## 硬约束

违反任一条会导致构建失败或功能回归。

### C1 — 延迟只能通过 `latencyMs()` 设置

`latency_`（单位：帧 @44.1kHz，当前 `6615` = 150ms）位于上游 `src/raop_sender.h`。

补丁已把上游 `test/core_tests.cpp` 中 5 处硬编码断言改为按 `latencyMs()` 动态计算 ——
**直接写死数值会让测试挂**。

换算 `frames = ms * 44100 / 1000`。SETUP 声明 `latencyMax = latency_ * 2`、`latencyMin = latency_ / 3`。

### C2 — 补丁是相对上游 `fc91350` 的 git diff

`patches/upstream-airplay2-sender-cpp.patch` 覆盖 4 个文件：
`CMakeLists.txt`（加 `/utf-8`）、`src/raop_sender.cpp`（瞬态配对补 event 密钥派生；SETUP 的 latency 改动态）、
`src/raop_sender.h`（加 `setLatencyMs`）、`test/core_tests.cpp`（5 处断言改动态）。

```bash
cd third_party/airplay2-sender-cpp
git diff > ../../patches/upstream-airplay2-sender-cpp.patch
```

**重新生成后必须先 commit 再验证** —— `git clone` 只认已提交内容，未提交的改动不会进 clone，
验证结果无效。

### C3 — 行尾必须是 LF

`.gitattributes` 已锁 `* text=auto eol=lf` 与 `*.patch -text`。
补丁带 CRLF 时 `git apply` 会 reject。

```bash
file patches/upstream-airplay2-sender-cpp.patch
# 期望 ASCII text / UTF-8 text；出现 "with CRLF line terminators" 即为异常
```

### C4 — Mbed TLS 钉在 3.6.0

固定 commit `2ca6c285a0dd3f33982dd57299012dacab1ff206`（上游 API 依赖 3.x 分支）。
首次配置拉约 190MB。网络受限时跳过下载：

```bash
cmake -S . -B build -G "<生成器>" -A x64 \
      -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<已有 mbedtls 源目录>
```

### C5 — MSVC 必须加 `/utf-8`

`CMakeLists.txt` 里 `add_compile_options(/utf-8)`，**必须对所有目标生效，包括上游目标**。

源码含中文注释，MSVC 默认按系统代码页（936/GBK）解码。UTF-8 序列的末字节恰好是
GBK 前导字节时会吞掉换行，把注释和下一行声明并成一行，报「标识符未定义」这类诡异错误。

### C6 — WIN32 子系统下不能用 `__argc` / `__argv`

`WinMain` 中 `__argc` 恒为 0，命令行参数被静默忽略。

改用 `CommandLineToArgvW(GetCommandLineW(), &argc)`，需要 `<shellapi.h>` 并链接 `shell32`。

### C6b — 日志清理策略

`logLine()` 中两条规则任一满足就 `remove()` 重开：
超过 30 天未写入（FILETIME 是 100ns 单位），或超过 512KB（挂机时每 2 秒一条心跳，
不限制会涨到几十 MB）。用 `GetFileAttributesExA` 取大小和最后写入时间。

### C7 — 音质档位改的是采集侧重采样

`src/audio/resampler.h`（窗函数 sinc）。采集端拿到的是设备混音格式（通常 48000），
AirPlay 流固定 44100。引擎自带两点线性插值，15kHz 处信噪比仅 14dB。

做法：在采集回调里先转到 44100，再 `setInputFormat(44100)` —— 引擎走 pass-through
分支，完全绕过它自己的重采样。**好处是不动第三方代码，补丁不需要跟着更新。**

- 配置字段 `qualityLevel`（0–100），连续：`taps = 4 + 28 * q / 100`（4..32 抽头）
- 品质为 0 时 `needResample` 置 false，交给引擎
- 老配置迁移：早期用 `quality`（0–3），加载时换算 `0→0, 1→30, 2→60, 3→100`。
  **不换算的话老的 3（=最好）会被当成 3%（=很差）**
- 档位名：快速(<10) / 标准(10–29) / 高(30–54) / 极高(55–79) / 至臻(≥80)
- **最低档不叫「关闭」** —— 它并非不转换，只是更省 CPU。命名沿用音频软件惯例
  （SoX: quick/low/medium/high）。原则：按快慢和质量命名，避免会被误解成没声音的词
- 上限 32 抽头：22 抽头往上测不出改善，定在 32 让滑块整条量程都有意义
- 改档要重连（`Resampler::configure()` 在会话建立时调）
- 自测：`resampler_test` 目标，输出信噪比对比表

### C9 — 进程必须声明 DPI 感知

`WinMain` 第一行调 `enableDpiAwareness()`，且必须在创建任何窗口之前。

不声明的话，系统缩放不是 100% 时 Windows 会把整个进程的界面按比例位图放大，
托盘菜单、托盘图标、滑块窗口全部发虚。本机 1920x1080 @125% 实测菜单被放大
1.25 倍，且在屏幕底部被截掉一整行。

托盘图标资源已含 16/20/24/32/48/64/128/256 全套尺寸，声明后
`LoadImageW` 会自动取到匹配当前 DPI 的那一帧。

### C8 — 无控制台程序的诊断输出要落盘

本项目是 WIN32 子系统，没有控制台；`AttachConsole(ATTACH_PARENT_PROCESS)`
在重定向场景下会失败。诊断类输出必须同时写文件。

参考实现：`src/device/audiodevices.h` 的 `audioOut()`。

## 禁止事项

都是实际踩过的，别重试。

**不要给引擎的类加成员变量。** 试过两次，导致堆损坏崩溃。加 `latencyMs_` 没事 ——
具体触发条件未查明，但「事件通道重连」那个改动确实是加成员后崩的（见 K1）。

**不要把延迟/音量/音质改成「点击才展开的独立菜单」。** 展开时主菜单会消失、
新菜单另起一处，观感不连续。在 Windows 原生菜单里，「贴合展开」与「悬停即展开」
是同一套机制，拆不开。

**不要用 emoji 或位图给原生菜单上色。** 两者都不生效。要颜色就用系统自带的勾选态
（`checked=true`，显示为蓝色勾）。

**不要用被 `enabled=false` 的行来显示状态。** Windows 会把禁用项整行变灰，
连右边的数值一起灰掉。要显示不可选状态，做成可点但空操作。

**不要在单元测试里依赖 `latency_` 的字面值。** 见 C1。

**对外文案不要写 Apple TV。** 设备列表默认屏蔽 Apple TV（`hideAppleTV=true`，只显示 HomePod），README、仓库 About、发布说明等所有对外描述只写「AirPlay 2 设备」。

## 已知未解问题

| ID | 问题 | 状态 |
|---|---|---|
| K1 | 事件通道断开后不重连 | 修复尝试加成员变量导致堆损坏，已回滚。有上层 6 秒重连兜底 |
| K2 | 多设备混音采集全是静音 | 单设备正常。接管模式下不需要，暂搁置 |
| K3 | `IPolicyConfig` 是未公开接口 | Windows 更新可能失效。已做重试 + 复核 |

## 挂账

| 项 | 说明 | 位置 |
|---|---|---|
| macOS 版 | 不是本项目移植，需求是「开机自动把输出切到 HomePod」。经调研 macOS 不提供该能力（AirPlay 设备自 Big Sur 起不再作为普通 Core Audio 设备暴露，脚本访问权限已被 Apple 移除），界面自动化方案在 macOS 26 上已失效。可自动化的路线反而是自行采集系统音频（Core Audio process taps，需 macOS 14.2+）。暂不启动 | [issue #1](https://github.com/RiverFlowsInUUU/AirPlay-Bridge/issues/1) |

## 发布说明文风（硬性）

发布说明是给用户的，不是开发周记。

**必须**：按 `Added` / `Changed` / `Fixed` / `Removed` / `Deprecated` / `Security` 分组；
一条 bullet 一个改动；用户视角（写「修复了 X 时会 Y」，不写「重构了 Z 模块」）；
破坏性变更单独一节且位置显眼；可扫读 —— 用户 10 秒内能判断要不要升级。

**禁止**：第一人称与叙事（「我」「我们觉得」「被指出」「之前偷懒了」）；
解释「为什么这么改」的段落；实现细节（文件路径、函数名、commit hash、PR 编号）；
开篇句式「这一版改了什么」。

正确开头就是直接 `## Added` / `## Fixed` 开列。日期用 ISO 格式，同步更新 `CHANGELOG.md`。

## README 文风（硬性）

README 是唯一人类入口。**只陈述，不解释。**

**必须**：开头一句话说清是什么；紧跟安装步骤；特性一行一条；
配置 / 命令 / 参数一律用表格；细节链接到 docs/。

**禁止**：

- 解释「为什么这么做」（为什么不买签名证书、为什么不叫无损、为什么 48k 转 44.1k）
- 背景故事与心路历程
- 段落式散文 —— 连续超过两行的文字就该改成表格或链接出去
- 内部实现细节（子系统类型、缓冲区、补丁机制）
- 命令行用法 —— 用户走 GUI，参数说明只属于 docs/DEVELOPMENT.md

**判断标准**：读者扫一遍就知道「这是什么、怎么跑起来、去哪深入」。想解释的冲动全部让给 docs/。

## 文档地图

| 文件 | 内容 |
|---|---|
| [README.md](README.md) | 人类入口 |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 分层、模块表、数据流、不变量、实测数据 |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | 环境、构建、测试、调试、发布 |
| [docs/DECISIONS.md](docs/DECISIONS.md) | 关键决策与理由、被否决的方案 |
| [docs/GLOSSARY.md](docs/GLOSSARY.md) | 术语表 |
| [CHANGELOG.md](CHANGELOG.md) | 变更记录（人类可读） |
