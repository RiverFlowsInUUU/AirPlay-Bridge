# AGENT 上下文

> 本文件面向 AI agent，不是给人读的。目标是**准确**和**无歧义**，
> 不考虑可读性。人类使用说明见 [README.md](README.md)。

## 项目定位

AI 辅助完成的项目。代码、文档、发布流程均由 agent 维护。

## 硬约束

违反以下任一条会导致构建失败或功能回归。

### C1 — 改 `latency_` 必须同步改测试

- 位置：`third_party/airplay2-sender-cpp/src/raop_sender.h`
- 字段：`latency_`（单位：帧 @44.1kHz）
- 关联：`test/core_tests.cpp` 中 5 处断言硬编码了 latency 的期望值
- 断言位置（行号基于上游 `fc91350`，打补丁后偏移）：
  - `RTP-Info announces seq + rtptime = latency`（约 763 行）
  - `sync: now-latency = 0, now = latency`（约 776 行）
  - `first rtptime == latency`（约 796 行）
  - `second packet: no marker, seq+1, rtptime+352`（约 805 行）
  - `our control port + latency window`（约 970 行）
- 补丁中已把这 5 处改为按 `latencyMs()` 动态计算，所以改 latency 不再需要改测试。
  **前提是不要绕过 `latencyMs()` 直接写死数值。**
- 验证：`ctest --test-dir tbuild -C Release` 必须 231/231 通过

当前值：`latency_ = 6615`（150ms）
换算：`frames = ms * 44100 / 1000`
SETUP 声明：`latencyMax = latency_ * 2`，`latencyMin = latency_ / 3`

### C2 — patch 是相对上游 fc91350 的 git diff

- 文件：`patches/upstream-airplay2-sender-cpp.patch`
- 基线：上游 commit `fc91350`
- 覆盖 4 个文件：
  - `CMakeLists.txt`（加 /utf-8）
  - `src/raop_sender.cpp`（HapTransient 补 event 密钥派生；SETUP 的 latency 改动态）
  - `src/raop_sender.h`（加 setLatencyMs；latency_ 默认值）
  - `test/core_tests.cpp`（5 处断言改动态）
- 重新生成：
  ```
  cd third_party/airplay2-sender-cpp
  git diff > ../../patches/upstream-airplay2-sender-cpp.patch
  ```
- **陷阱**：验证时必须先 `git add` + `commit`。`git clone` 只认已提交内容，
  未提交的改动不会出现在新 clone 里，会导致验证结果无效。

### C3 — 行尾必须是 LF

- `.gitattributes` 已锁 `* text=auto eol=lf` 和 `*.patch -text`
- 原因：patch 带 CRLF 时 `git apply` 会失败（报 "trailing whitespace" 或直接 reject）
- 检查：
  ```
  file patches/upstream-airplay2-sender-cpp.patch
  # 期望: ASCII text / UTF-8 text
  # 异常: ... with CRLF line terminators
  ```

### C4 — Mbed TLS 钉在 3.6.0

- 位置：`third_party/airplay2-sender-cpp/CMakeLists.txt` 的 FetchContent_Declare
- 固定 commit：`2ca6c285a0dd3f33982dd57299012dacab1ff206`
- 原因：上游 API 依赖 3.x 分支；2.x 的 API 不同，编译不过
- 首次配置会拉约 190MB

网络受限时跳过下载：
```
cmake -S . -B build -G "<generator>" -A x64 \
      -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<已有的 mbedtls 源目录>
```

### C5 — MSVC 必须加 /utf-8

- 位置：`CMakeLists.txt`（`add_compile_options(/utf-8)`，对所有目标生效）
- 原因：源码含中文注释，MSVC 默认按系统代码页（936/GBK）解码。
  UTF-8 序列的末字节恰好是 GBK 前导字节时会吞掉换行，
  把注释和下一行声明并成一行，导致"标识符未定义"这类诡异错误。
- **给上游目标也要加**，不能只加在自己的目标上

### C6 — WIN32 子系统下不能用 __argc/__argv

- 位置：`src/app/airplay_bridge.cpp` 的 `WinMain`
- 现象：`__argc` 恒为 0，所有命令行参数被静默忽略
- 正确做法：`CommandLineToArgvW(GetCommandLineW(), &argc)`
- 需要 `<shellapi.h>`，链接 `shell32`

### C6b — 日志清理策略

- 位置：`src/app/airplay_bridge.cpp` 的 `logLine()`
- 两条规则任一满足就 `remove()` 重开：
  - 超过 30 天没写入（`k30Days = 30*24*3600*10^7`，FILETIME 是 100ns 单位）
  - 超过 512KB（挂机时每 2 秒一条心跳，不限制会涨到几十 MB）
- 用 `GetFileAttributesExA` 取大小和最后写入时间

### C7 — 音质档位改的是采集侧重采样

- 位置：`src/audio/resampler.h`（窗函数 sinc 实现）
- 原理：采集端拿到的是设备混音格式（通常 48000），AirPlay 流固定 44100。
  引擎自带的转换是两点线性插值，高频失真严重（实测 15kHz 信噪比仅 14dB）。
  我们在采集回调里先转到 44100，然后 `setInputFormat(44100)` ——
  引擎会走 pass-through 分支，完全绕过它自己的重采样。
- **好处是不动第三方代码**，patch 不需要更新。
- 配置字段：`qualityLevel`（0-100）
- 品质 0-100 连续：`taps = 4 + 28 * q / 100`（4..32 抽头）
- **老配置迁移**：早期版本用 `quality`（0-3），加载时自动换算 ——
  0→0, 1→30, 2→60, 3→100。不换算的话老的 3（=最好）会被当成 3%（=很差）
- 品质为 0 时 `needResample` 改成 false，让引擎自己处理
- 实测曲线（19.9kHz 信噪比）：5%→20.2dB, 20%→30.2dB,
  30%→34.5dB, 50%→39.1dB, 70%+→39.5dB（到顶）
- 上限定 32 抽头是因为 22 抽头往上测不出改善，
  定在 32 能让滑块整条量程都有意义
- 改档位要重连（`Resampler::configure()` 在会话建立时调）
- 自测：`resampler_test` 目标，输出信噪比对比表

### C8 — 无控制台程序的诊断输出要落盘

- 本项目是 `WIN32` 子系统（`add_executable(... WIN32 ...)`），没有控制台
- `AttachConsole(ATTACH_PARENT_PROCESS)` 在重定向场景下会失败
- 诊断类输出（如 `--audio-devices`）必须同时写文件：
  `%LOCALAPPDATA%\AirPlayBridge\audio-devices.log`
- 参考实现：`src/device/audiodevices.h` 的 `audioOut()`

## 架构

```
src/app/airplay_bridge.cpp     主流程：参数解析 → 托盘循环 → 会话管理
src/app/config_store.*         JSON 配置（%LOCALAPPDATA%\AirPlayBridge\）
src/audio/wasapi_capture.*     环回采集
src/audio/sysvolume.*          系统音量 + 默认播放设备切换
src/audio/resampler.h          采集侧重采样（窗函数 sinc）
src/device/discovery.*         设备发现（扫网段 + 单播 mDNS）
src/device/audiodevices.h      --audio-devices 的实现
src/ui/tray.*                  托盘图标与菜单
src/ui/latency_dialog.*        滑块窗口（延迟/音量共用）
src/icons/                     图标资源 + bridge.rc
```

### 关键设计决策

**D1 — 设备发现用单播 mDNS，不用组播。**
Windows 上组播（224.0.0.251）经常被网卡或路由器拦掉，实测零应答。
做法：先扫网段找 7000 端口开放的 IP，再对每个 IP 发单播 mDNS 查询。

**D2 — 采集默认单设备（`CaptureMode::DefaultOnly`）。**
多设备混音（`AllDevices`）的实现保留但未启用：
实测多台设备同时打开时采集全是静音，单设备正常，原因未查明。
接管模式下也不需要（声音都进虚拟声卡了）。

**D3 — 接管模式靠切默认播放设备，不靠驱动。**
连上 HomePod 时把系统默认输出切到虚拟声卡（CABLE Input），
断开时切回。用未公开的 `IPolicyConfig` 接口。

- `sysvolume.cpp` 里定义了两个变体：
  - `IPolicyConfig`（有 `ResetDeviceFormat`），配 CLSID `{870af99c-...}`
  - `IPolicyConfigVista`（**无** `ResetDeviceFormat`），配 CLSID `{294935CE-...}`
- Win11 上老 CLSID 未注册，必须用 Vista 版 + IID `{568b9108-...}`
- **两个接口的 vtable 顺序不同**，混用会导致 `SetDefaultEndpoint` 落到错误的槽位

**D4 — 崩溃保护靠配置文件留标记。**
切到虚拟声卡前把原设备名写进 `bridgeConfig.preCaptureDevice`。
启动时若该字段非空且当前默认是 CABLE，说明上次异常退出，执行恢复。
**只有恢复成功才清标记** —— 失败必须保留，否则用户永久没声音。

## 构建

```
git clone --recursive <repo>
cd <repo>
cd third_party/airplay2-sender-cpp
git apply ../../patches/upstream-airplay2-sender-cpp.patch
cd ../..
cmake -S . -B build -G "<本机生成器>" -A x64
cmake --build build --config Release
```

生成器名字以本机为准：
```
cmake --help | grep -i "visual studio"
```

产物：`build/Release/airplay_bridge.exe`（单文件，静态链接 CRT）

## 验收

改完照这个走，全绿才算完成：

```
# 1. 干净克隆
git clone --recursive <repo> /tmp/verify && cd /tmp/verify

# 2. 打补丁（必须干净应用）
cd third_party/airplay2-sender-cpp
git apply ../../patches/upstream-airplay2-sender-cpp.patch
cd ../..

# 3. 编译
cmake -S . -B build -G "<生成器>" -A x64
cmake --build build --config Release
# 期望：无 error，产出 build/Release/airplay_bridge.exe

# 4. 上游测试（231 项）
cd third_party/airplay2-sender-cpp
cmake -S . -B tbuild -G "<生成器>" -A x64 -DAIRPLAY_BUILD_TESTS=ON
cmake --build tbuild --config Release
ctest --test-dir tbuild -C Release
# 期望：100% 通过（231/231）
```

运行时验证（需要 HomePod 在线）：
```
# 串流稳定性：跑 120 秒，日志应无 "连接断开"
%LOCALAPPDATA%\AirPlayBridge\bridge.log

# 接管模式：默认设备应自动切到 CABLE Input
airplay_bridge.exe --audio-devices
```

## 已知未解问题

| ID | 问题 | 状态 |
|---|---|---|
| K1 | 事件通道断开后不重连 | 尝试修复时加成员变量导致堆损坏，已回滚。有上层 6 秒重连兜底 |
| K2 | 多设备混音采集全是静音 | 单设备正常。接管模式下不需要，暂搁置 |
| K3 | `IPolicyConfig` 是未公开接口 | Windows 更新可能失效。已做重试 + 复核 |

## 发布

```
# 编译
cmake --build build --config Release

# 打包（单文件，直接挂 exe 不压缩）
gh release create vX.Y.Z build/Release/airplay_bridge.exe \
  --title "AirPlay Bridge vX.Y.Z" --notes "..."
```
