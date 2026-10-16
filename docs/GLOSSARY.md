# 术语表

面向 AI agent：本项目的术语、缩写的准确含义。避免同名不同义。

## 协议

**AirPlay 1 / RAOP** — Remote Audio Output Protocol。2010 年代的事实标准，
第三方发送端（TuneBlade、Airfoil、Roon）都走这条。
握手路径被 Apple 改动后已不可用，见 [DECISIONS.md D1](DECISIONS.md)。

**AirPlay 2** — 当前协议。本项目接入的是这一条。

**HAP** — HomeKit Accessory Protocol。AirPlay 2 的配对与加密复用 HAP 的密钥交换。

**瞬态配对（transient pairing）** — HAP 的一种简化配对，无需用户输入 PIN。
本项目走这条路径。

**事件通道（event channel）** — 建立音频流之后反向回连的一条加密通道，
用于接收设备端事件。其读写密钥与音频通道相反，需要单独派生。

**HKDF** — HMAC 密钥派生函数。本项目用 HKDF-SHA512，
`Control-Salt` 用于控制通道，`Events-Salt` 用于事件通道。

**ALAC** — Apple Lossless Audio Codec。名字是「无损」，但在这条链路上有损 ——
音频在到达编码器前已被重采样过一次。

## 音频

**WASAPI 环回（loopback）** — Windows 上采集「正在播放的声音」的方式。
本项目的采集手段，因此不支持其他平台。

**混音格式（mix format）** — 音频设备的内部工作格式。采样率通常是 48000 Hz，
与 AirPlay 流的 44100 Hz 不一致，必须重采样。

**抽头（taps）** — 窗函数 sinc 重采样的滤波器长度。抽头越多质量越高、CPU 越高。
本项目范围 4–32，由 `qualityLevel` 映射。见 [AGENTS.md C7](../AGENTS.md)。

**窗函数 sinc** — 本项目使用的重采样算法。另一条路是线性插值，
高频段失真严重，见 [ARCHITECTURE.md](ARCHITECTURE.md) 实测数据。

**信噪比（dB）** — 本项目衡量重采样质量的指标。
数值越高越接近原始信号；40 dB 左右即可认为该环节透明。

## 模式

**监听复制** — 采集系统默认声卡的输出，复制一份推给设备。
电脑自己的音箱和 AirPlay 设备同时出声。

**接管** — 声音先进入虚拟声卡，由本程序采集后送出，物理音箱静音。
需要预先安装 VB-Audio Virtual Cable。

**CABLE Input / CABLE Output** — VB-Audio Virtual Cable 提供的虚拟声卡的两端。
系统把声音播到 CABLE Input，本程序从 CABLE Output 采集。

**默认播放设备** — Windows 系统中，所有未指定输出的应用共用的播放设备。
接管模式通过切换它来改道。

**IPolicyConfig** — Windows 未公开接口，用于切换默认播放设备。
本项目需要它的两个变体，见 [DECISIONS.md D3](DECISIONS.md)。

## 发现

**mDNS** — 局域网服务发现协议，AirPlay 设备用它广播自己。

**单播 / 组播** — 组播地址是 224.0.0.251。Windows 上组播常被拦，
本项目改用单播逐个询问，见 [DECISIONS.md D2](DECISIONS.md)。

**7000 端口** — HomePod 开放该端口表示 AirPlay 服务可用。
本项目用它做网段扫描的判据。

**deviceid** — 设备的稳定标识。用它而非 IP 记忆上次连接的设备，
设备更换 IP 后仍能找回。

## 运行时

**看门狗（watchdog）** — 盯主循环心跳的独立线程。主循环每轮刷新一个时间戳，
超过 800ms 没刷新就往日志写一条「主线程无响应 N 秒，最后阶段: X」。
判读方法：**有「恢复」= 偶发卡顿；没有「恢复」= 死锁**。

**静音待机（standby）** — 连续一段时间没声音就把会话整个断开（让接收端熄屏），
检测到声音立刻重连。默认开启，时长 5–10 秒可调。
注意它与「掉线」不同：待机是主动断开，不该触发自动重连的退避循环。
