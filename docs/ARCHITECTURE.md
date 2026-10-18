# 架构

面向 AI agent：模块地图、数据流、层间不变量、实测数据。
硬约束逐条在 [AGENTS.md](../AGENTS.md)，此处只在必要处引用编号。

## 分层

```
┌─ UI ──────────  src/ui/            托盘图标与菜单、滑块窗口
├─ App ─────────  src/app/           主流程、会话管理、配置持久化
├─ Device ──────  src/device/        设备发现、音频设备枚举
├─ Audio ───────  src/audio/         环回采集、重采样、系统音量与默认设备
└─ Protocol ────  third_party/       AirPlay 2 协议实现（含 patches/）
```

依赖方向**只能向下**。Audio 和 Device 不引用 UI；Protocol 不引用本项目任何代码。

## 目录

```
src/
  app/       主程序：入口、配置读写、防火墙与自启集成、i18n
  audio/     音频采集与重采样（WASAPI loopback 到 44100）
  device/    音频设备枚举、默认设备切换、系统音量与静音
  ui/        托盘、菜单构建、滑块窗口（全自绘）、深色模式
  icons/     图标资源（.ico / .rc）
tools/       图标生成脚本（仓库根下）
third_party/ 协议引擎 airplay2-sender-cpp（git submodule + 本项目补丁）
patches/     相对上游的 git diff（见 RULES.md C2）
installer/   Inno Setup 脚本
docs/        本套文档
```

**主循环和托盘菜单回调都在 `src/app/airplay_bridge.cpp` 的 `main()` 里**（近 1500 行）。
它们深度依赖 main 的局部状态，抽出去会牵动一大片调用约定 ——
这一点有专门的决策记录，见 [DECISIONS.md](DECISIONS.md) 和 [PITFALLS.md](PITFALLS.md)。

## 模块表

| 路径 | 职责 |
|---|---|
| `src/app/airplay_bridge.cpp` | 入口、参数解析、托盘消息循环、会话生命周期、日志 |
| `src/app/config_store.*` | JSON 配置读写，落在 `%LOCALAPPDATA%\AirPlayBridge\` |
| `src/audio/wasapi_capture.*` | WASAPI 环回采集 |
| `src/audio/resampler.h` | 窗函数 sinc 重采样（header-only，可脱离本程序单测） |
| `src/audio/resampler_test.cpp` | 重采样自测，四节：基本正确性 / 品质曲线 / **分块不变性** / **窗口索引范围**（后两节是回归防护，见 RULES.md C11） |
| `src/audio/sysvolume.*` | 系统音量、默认播放设备切换 |
| `src/device/discovery.*` | 设备发现（扫网段 + 单播 mDNS） |
| `src/device/audiodevices.h` | `--audio-devices` 的实现 |
| `src/ui/tray.*` | 托盘图标、菜单树构建与分发 |
| `src/ui/latency_dialog.*` | 滑块窗口，延迟/音量/音质共用 |
| `src/ui/darkmode.h` | 菜单深色模式（uxtheme 未公开序号，header-only） |
| `src/icons/` | 图标资源与 `bridge.rc` |
| `tools/gen_icons.py` | 由一张源图生成全部图标（**直接写 `src/icons/`**） |

## 数据流

```
系统声音（默认播放设备）
  │
  ├─ 接管模式：声音先进 CABLE Input，物理音箱静音
  │
  ▼
WASAPI 环回采集                    wasapi_capture.cpp
  │  设备混音格式，通常 48000 Hz
  ▼
窗函数 sinc 重采样                  resampler.h
  │  48000 → 44100，抽头数 = 4 + 28 * qualityLevel / 100
  ▼
setInputFormat(44100)              raop_sender
  │  引擎走 pass-through 分支，绕过它自带的线性插值
  ▼
RAOP 打包 + ALAC 编码              raop_sender.cpp
  │  SETUP 中声明 latency_、latencyMax、latencyMin
  ▼
TCP（控制/音频）+ UDP（时序）→ 设备
```

## 不变量

改代码时守住这些，否则会跨层泄漏。

- **I1** — 上游代码只通过 `patches/` 修改，不在 `third_party/` 里直接改。
  补丁机制见 [AGENTS.md C2](../AGENTS.md)。
- **I2** — 延迟只在会话建立时下发，无法中途改。改延迟必须重建会话。
- **I3** — 音质只作用于采集侧，不触碰协议层。见 [AGENTS.md C7](../AGENTS.md)。
- **I4** — 音量作用于接收端，与延迟、音质互不影响。
- **I5** — 默认设备切换涉及两个 vtable 顺序不同的接口，不可混用（见 [DECISIONS.md D3](DECISIONS.md)）。
- **I6** — 崩溃保护靠配置里的标记位，**只有恢复成功才清标记**（见 [DECISIONS.md D4](DECISIONS.md)）。

## 协议补丁

`patches/upstream-airplay2-sender-cpp.patch`，基线上游 `fc91350`，覆盖 4 个文件。

核心修复：瞬态配对（`HapTransient`）分支缺事件通道密钥派生，
导致连上约 30 秒后被 HomePod 主动断开。

```cpp
ap2_->eventIn  = hkdfSha512("Events-Salt", "Events-Write-Encryption-Key", ap2_->sharedSecret, 32);
ap2_->eventOut = hkdfSha512("Events-Salt", "Events-Read-Encryption-Key",  ap2_->sharedSecret, 32);
```

上面这段是**有意为之，不是笔误**：反向事件通道的收发方向与正向相反，
所以两把密钥按名字「对调」使用 —— `eventIn` 取 Write 键、`eventOut` 取 Read 键。
改回「按名字对应」会连不通。

补丁把延迟从写死改为可变（SETUP 时下发），并使相关测试断言随之动态。

## 实测数据

### 重采样质量

48k → 44.1k，单频点信噪比（dB）：

| 信号频率 | 引擎线性插值 | 窗函数 sinc |
|---|---|---|
| 1 kHz | 38.6 | 40.0 |
| 5 kHz | 28.9 | 39.5 |
| 10 kHz | 20.8 | 39.4 |
| 15 kHz | 13.9 | 39.6 |
| 19.9 kHz | 6.3 | 39.5 |

线性插值在 15 kHz 以上塌掉；sinc 全程稳定在 40 dB 左右。

### 音质档位曲线（19.9 kHz）

| `qualityLevel` | 信噪比 |
|---|---|
| 5% | 20.2 dB |
| 20% | 30.2 dB |
| 30% | 34.5 dB |
| 50% | 39.1 dB |
| 70% 及以上 | 39.5 dB（到顶） |

上限取 32 抽头，是因为 22 抽头往上测不出改善，定在 32 可让滑块整条量程都有意义。

### 开销

- 32 抽头 sinc：约占单核 8%
- 虚拟声卡额外引入约 10ms 延迟
- 采集缓冲 30ms → 10ms 未观察到爆音
