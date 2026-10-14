# AirPlay Bridge

把 Windows 的系统声音实时串流到 AirPlay 2 设备，跑在托盘里。

TuneBlade 在 HomePod OS 27 上失效了 —— Apple 改动了 AirPlay 1 的握手路径，
所有走 RAOP 的第三方发送端（TuneBlade、Airfoil、Roon）一起哑火。
TuneBlade 闭源、2018 年停更，没有修复的可能。本项目是走 AirPlay 2 的开源实现。

## 快速开始

1. 从 [Releases](https://github.com/RiverFlowsInUUU/AirPlay-Bridge/releases/latest) 下载 `.exe`
2. 双击运行，托盘出现图标
3. 托盘图标 → 选择设备 → 点你的 HomePod

Windows 10 / 11，64 位。单文件，不需要安装，不需要运行库。

> 首次运行会弹「Windows 已保护你的电脑」。这是 SmartScreen 对未签名程序的常规提示，
> 不是报毒。点「更多信息」→「仍要运行」。
> 不买代码签名证书是因为它按年收费，个人项目不划算。介意的话可以自行编译。

## 两种工作模式

### 监听复制（开箱可用）

采集系统默认声卡的输出，复制一份推给 HomePod。电脑自己的音箱和 HomePod 同时出声。

适合电脑没接音箱，或者不在乎两边同时响。

### 接管（推荐，需要虚拟声卡）

```
系统声音 → CABLE Input（虚拟声卡）→ AirPlay Bridge → HomePod
电脑物理音箱：静音
```

先装 [VB-Audio Virtual Cable](https://vb-audio.com/Cable/)（免费）。

程序会自动：连上时把系统默认播放设备切到虚拟声卡；断开或退出时切回原设备；
被强杀后，下次启动自动恢复。

## 托盘菜单

```
✓ 已连接
  <设备名>
  <设备 IP>
────────────────
  断开
  选择设备  ›
  延迟  150ms  ›
  音量  45%  ›
  音质  至臻  ›
────────────────
  开机自启
  打开日志文件夹
────────────────
  退出
```

左键和右键都能打开菜单。「选择设备」里可以重新扫描。

## 三个可调项

| 项 | 范围 | 生效方式 |
|---|---|---|
| 延迟 | 60 – 1000 ms，5 档 + 滑块 | 重连后生效（写在握手参数里） |
| 音量 | 5% – 100%，20% 一档 + 滑块 | 立即生效（接收端调节） |
| 音质 | 0 – 100%，5 档 + 滑块 | 重连后生效（采集侧重采样） |

三者互不影响：延迟在协议层，音量在接收端，音质在采集侧。

**音质档位**按音频软件惯例命名（SoX 用 quick/low/medium/high）：

| 档位 | 区间 | 说明 |
|---|---|---|
| 快速 | < 10 | 最省 CPU，高频有可闻失真 |
| 标准 | 10 – 29 | 高频仍有可闻失真 |
| 高 | 30 – 54 | 明显改善 |
| 极高 | 55 – 79 | 接近透明 |
| 至臻 | ≥ 80 | 转换环节已透明（默认） |

声卡通常跑 48000 Hz，AirPlay 流固定 44100 Hz，中间必须重采样。
协议引擎自带的是两点线性插值，15 kHz 以上基本塌掉；本程序在采集侧改用窗函数 sinc，
把这一段拉到透明。选最低档不是「不转换」，只是算法更省 CPU。

> 不叫「无损」是有意的 —— 这条链路本身是 ALAC 有损压缩，且上游已重采样过一次。
> 「至臻」只表示转换环节做到了透明。

## 命令行参数

```
airplay_bridge                     托盘模式
airplay_bridge --console           命令行模式
airplay_bridge --list              列出发现的设备
airplay_bridge <ip>                直连指定 IP
airplay_bridge --last              重连上次的设备
airplay_bridge --install           注册开机自启
airplay_bridge --uninstall         取消开机自启
airplay_bridge --audio-devices     列出音频设备（诊断）
airplay_bridge --set-default <名>  切换默认播放设备（诊断）
```

## 排查没声音

1. 托盘菜单里确认「已连接」
2. 跑 `airplay_bridge.exe --audio-devices`，确认默认播放设备是不是你以为的那个
3. 看日志

日志在 `%LOCALAPPDATA%\AirPlayBridge\bridge.log`（托盘菜单有「打开日志文件夹」）。
自动清理：超过 30 天未写入，或超过 512KB。

程序是 WIN32 子系统（没有控制台），从某些终端调用时 stdout 可能看不到 ——
`--audio-devices` 的输出会同时写一份到同目录的 `audio-devices.log`。

## 已知限制

- 接管模式依赖虚拟声卡；没装就只能用监听复制模式
- 延迟和音质改档需要重连一次
- 事件通道断线不自动重连（有上层 6 秒重连兜底）
- 只在 Windows 上测过（采集用 WASAPI，是 Windows 独有的）

## 深入

- 参与开发、构建、验收、发布 → [AGENTS.md](AGENTS.md)
- 模块地图与数据流 → [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- 关键决策与理由 → [docs/DECISIONS.md](docs/DECISIONS.md)
- 术语表 → [docs/GLOSSARY.md](docs/GLOSSARY.md)
- 变更记录 → [CHANGELOG.md](CHANGELOG.md)

## 依赖

- [airplay2-sender-cpp](https://github.com/akustikrausch/airplay2-sender-cpp) — AirPlay 2 协议实现（Apache-2.0）
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) — 加密

协议层带一个本地补丁（`patches/`），补上瞬态配对路径上缺失的事件通道密钥派生 ——
缺了它会连上约 30 秒后被 HomePod 主动断开。补丁固定在上游某个 commit 上，
所以上游更新不会自动破坏它；反过来说，协议跟进依赖上游继续维护。

## 许可证

Apache-2.0
