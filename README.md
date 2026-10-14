# AirPlay Bridge

把 Windows 的系统声音实时串流到 AirPlay 2 设备（HomePod / Apple TV），跑在托盘里。

写这个是因为 TuneBlade（2018 年停更的 AirPlay 1 实现）在 HomePod OS 27 上彻底失效 ——
Apple 改动了 AirPlay 1 的握手路径，导致所有走 RAOP 的第三方发送端（TuneBlade、Airfoil、
Roon 等）集体哑火。这个是走 AirPlay 2 的新实现，不受影响。

## 特性

- **自动发现设备** —— 扫网段 + 单播 mDNS（绕开 Windows 上经常失效的组播）
- **接管模式** —— 配合虚拟声卡，电脑自己不出声，声音只从 HomePod 出
- **自动切换** —— 连上时切到虚拟声卡，退出时切回原来的设备
- **崩溃保护** —— 程序被强杀后，下次启动自动恢复音频设备
- **断线重连** —— 断线后自动重连，设备 IP 变了也能找到（按 deviceid 记忆）
- **可调延迟** —— 5 档固定 + 无极滑块（60-1000ms），不同网络环境自己试出最优值
- **可调音量** —— 5 档固定 + 无极滑块
- **可调音质** —— 采集端重采样质量（关 / 线性 / 好 / 最好）
- **开机自启**
- **单文件零外挂** —— 一个 exe，放哪都能跑，配置写在 `%LOCALAPPDATA%`

## 两种模式

### 模式一：监听复制（不需要额外软件）

直接采集系统默认声卡的输出，复制一份推给 HomePod。
电脑自己的音箱和 HomePod 同时出声。

适合：电脑没接音箱，或者不在乎两边同时响。

### 模式二：接管（推荐，需要虚拟声卡）

```
系统声音 → CABLE Input（虚拟声卡）→ 本程序采集 → HomePod
电脑物理音箱：完全静音
```

需要预先安装 [VB-Audio Virtual Cable](https://vb-audio.com/Cable/)（免费）。

程序会自动：

- 连上 HomePod 时把系统默认播放设备切到虚拟声卡
- 断开/退出时切回原来的设备
- 如果程序被强杀，下次启动自动恢复设备

## 系统要求

- Windows 10 / 11，64 位
- 接管模式需要装 VB-Audio Virtual Cable（见上）

> **首次运行会弹「Windows 已保护你的电脑」** —— 这是 SmartScreen 对
> 未签名程序的常规提示，不是报毒。点「更多信息」→「仍要运行」即可。
> 没有代码签名证书是因为那玩意儿要年费，个人项目不划算。
> 介意的话可以自己从源码编译。

## 自己编译

构建步骤和注意事项见 [AGENT.md](AGENT.md)。

## 用法

双击 `airplay_bridge.exe`，托盘图标出现在右下角。

右键托盘图标：

```
✓ 已连接
  Living Room HomePod
  192.168.1.100
────────────────
  断开
  重新扫描设备
  延迟  150ms
  音量  45%
────────────────
  开机自启
────────────────
  退出
```

### 命令行参数

```
airplay_bridge                    托盘模式
airplay_bridge --console          命令行模式
airplay_bridge --list             列出发现的设备
airplay_bridge <ip>               直连指定 IP
airplay_bridge --last             重连上次的设备
airplay_bridge --install          注册开机自启
airplay_bridge --uninstall        取消开机自启
airplay_bridge --audio-devices    列出音频设备（诊断用）
airplay_bridge --set-default <名> 切换默认播放设备
```

### 诊断与音频设备

这两个功能已经并进主程序，不需要额外的小工具：

```
airplay_bridge --audio-devices            列出音频设备及缓冲参数
airplay_bridge --set-default "CABLE Input"  切换默认播放设备
airplay_bridge --set-default "Realtek"      切回物理声卡
```

> `--audio-devices` 的输出同时写一份到
> `%LOCALAPPDATA%\AirPlayBridge\audio-devices.log`，
> 因为这个程序是 WIN32 子系统（没有控制台），从某些终端调用时
> stdout 可能看不到 —— 那个文件里一定有。

排查"串流没声音"时先跑 `airplay_bridge.exe --audio-devices` —— 确认默认设备是不是你以为的那个。

## 关于音质

声卡通常跑 48000 Hz，而 AirPlay 流固定 44100 Hz —— 中间必须转换。
协议引擎自带的是两点线性插值，高频段失真明显。

实测（48k → 44.1k，1 kHz 到 19.9 kHz 扫频，单频点信噪比）：

```
信号频率    线性插值    窗函数 sinc
 1 kHz      38.6 dB     40.0 dB
 5 kHz      28.9 dB     39.5 dB
10 kHz      20.8 dB     39.4 dB
15 kHz      13.9 dB     39.6 dB
19.9 kHz     6.3 dB     39.5 dB
```

线性插值在 15 kHz 以上基本塌掉，sinc 全程稳在 40 dB 左右。

音质档位控制这个：

- **最好**（默认）—— 32 抽头窗函数 sinc
- **好** —— 16 抽头，质量接近，CPU 减半
- **线性** —— 交给引擎自带的线性插值
- **关** —— 同上，不启用采集侧重采样

实测 CPU 开销（最好档）：单核约 8%。
改档位需要重连一次才生效（重采样器在会话建立时初始化）。

## 关于延迟

端到端延迟可调（60-1000ms），默认 150ms。

几个参考数字：

- 虚拟声卡本身只增加约 **10ms**（实测 CABLE 的缓冲周期是 10ms，跟物理声卡一样）
- 大头在接收端缓冲 —— AirPlay 2 用 RTP 时间戳 + PTP 时钟同步做播放调度，必须留提前量
- 官方支持的最低值受设备固件限制，只能实测标定

想找最优值：从低往高试，直到不爆音为止 —— 那一档就是你这套网络/设备组合的下限。

## 关于 TuneBlade 失效这件事

如果你是因为 TuneBlade 连不上 HomePod 才找到这里：

根因是 HomePod OS 27 破坏了 AirPlay 1（RAOP）的握手路径。这不是配置问题，
是 Apple 侧的改动 —— Rogue Amoeba 也确认了（他们的 Airfoil 同样失效，
bug report #FB24452968）。

TuneBlade 是闭源的、2018 年就停更了，没有修复的可能。本项目的价值在于
它是走 AirPlay 2 的开源实现，可以持续跟进 Apple 的变化。

## 设置会记住

音量、延迟、音质、上次连的设备都写在配置里，下次开机连上还是这些值。
启动时日志会打一行「已加载设置: ...」，可以确认。

## 日志

位置：`%LOCALAPPDATA%\AirPlayBridgeridge.log`（托盘菜单有「打开日志文件夹」）。

自动清理，两条规则任一满足就重开：超过 30 天没动过，或超过 512KB。

## 已知限制

- 接管模式依赖虚拟声卡；没装的话只能用监听复制模式
- 延迟改档需要重连一次才生效（写在握手参数里，无法中途改）
- 事件通道断线不自动重连（有上层 6 秒重连兜底）
- 只在 Windows 上测过（WASAPI 是 Windows 独有的）

## 改代码

构建步骤见上一节；踩过的坑和验收流程在 [AGENT.md](AGENT.md)（那份是写给 AI 的，内容是准的）。

## 依赖

- [airplay2-sender-cpp](https://github.com/akustikrausch/airplay2-sender-cpp) — AirPlay 2 协议实现（Apache-2.0）
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) — 加密（由上游自动拉取）

协议层带有一个本地补丁（`patches/`），内容是在瞬态配对路径上补齐
事件通道的密钥派生 —— 缺了它会连上约 30 秒后被 HomePod 主动断开。
补丁固定在某个上游 commit 上，所以上游更新不会自动破坏它；
但反过来说，**协议跟进依赖上游继续维护**。

## 许可证

Apache-2.0
