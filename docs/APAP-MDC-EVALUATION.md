# APAP / MediaDataControl（type 103）可行性评估

**结论先行：不建议现在做。**

不是因为它做不到，而是因为**我们原先以为的收益（更低延迟）不成立** —— 见
「二、收益核算」。如果目标改成「弱网抗丢包」或「持久连接省掉重连」，结论才会变。

- 评估日期：2026-10-30
- 评估对象：把发送端从 `type 96`（realtime ALAC / RTP over UDP / NTP）
  换成 `type 103`（AAC / APAP over TCP / MediaDataControl / PTP 锚点）
- 证据等级：**代码与公开资料已核实**；协议细节**未经抓包**，凡属推测均已标注

---

## 一、结论与工量

| 阶段 | 内容 | 工量 | 风险 |
|---|---|---|---|
| 0 | 抓包与逆向（APAP 头、MDC 命令、是否需要 PTP） | 3–5 人日 | 需要可镜像端口的网络环境，否则做不了 |
| 1 | TCP 媒体通道 + APAP 分帧 + AAC 编码 | 8–12 人日 | 中，主要是不确定接收端是否接受「无 MDC」的流 |
| 2 | MediaDataControl 最小命令集（`strt` / `anch` / `fshb`） | 10–15 人日 | 高，命令语义无公开文档 |
| 3 | PTP 时序（或找到 RTP 锚点替代方案） | 10–20 人日 | **可能不可行** |

合计 **30–50 人日**，且第 3 阶段有「做完发现必须重写」的可能。

**建议的替代动作**：如果痛点是弱网丢音，先做「UDP 重传补齐 + 缓冲自适应」，
成本约 3–5 人日，收益可量化；这比换传输现实得多。

---

## 二、收益核算（这一步推翻了原有假设）

原本的假设是：Apple 在 27.2 让 Mac→HomePod 的音频「延迟可忽略」，
是因为走了 type 103，所以我们也该走。

**但公开资料显示这条路本身并不低延迟。** shairport-sync 的 AirPlay 2 文档
（`AIRPLAY2.md`）明确写着两种流的典型延迟：

> Realtime streams generally have a latency of about two seconds.
> Buffered Audio streams typically have a latency of half a second or less.

也就是说：

| 路径 | 典型延迟 | 我们 |
|---|---|---|
| Realtime（type 96 / ALAC） | ~2 秒 | **200ms**（可调 130–300ms） |
| Buffered（type 103 / AAC） | ~0.5 秒 | — |

**我们现在已经比这条「更快」的路快一倍以上。** 换过去在延迟上是**倒退**。

那 27.2 的「可忽略延迟」从哪来？现有证据不足以定论，可能是：

1. Mac 侧用了非标准的持久会话（配合 Wi-Fi 6E 硬件），不是通用 type 103；
2. 接收端对 Apple 自家客户端走了不同的缓冲策略；
3. 我们对「可忽略」的体感判断与实际数值不是一回事。

三种都不在「第三方发送端实现 type 103」能拿到的范围内。

**因此 type 103 的真实收益只剩两条**：TCP 重传带来的弱网稳定性，以及持久会话
省掉重连。这两条都不值得 30–50 人日 —— 前者有更便宜的替代方案，后者我们已经有
6 秒重连兜底 + 待机唤醒。

---

## 三、协议要素：已知与未知

### 已知（来源：shairport-sync issue #2303 的抓包、公开实现）

| 项 | 值 |
|---|---|
| 流类型 | `type=103` |
| 编码 | `ct=4`（AAC），`spf=1024`，`sr=44100`，`audioFormatIndex=22` |
| 连接 | `streamConnections: RTP=0 RTCP=0 APAP=1 MediaDataControl=1` |
| APAP 外层 | 4 字节大端总长 + 15 字节头 + 加密载荷 |
| APAP 头内容 | 24 位序列号、媒体时间（每帧步进 1024 采样） |
| 载荷 | AAC access unit |
| MDC 命令 | `amsm` `srat` `fshb` `strt` `anch` `magc` |
| 锚点 | `anch` 建立 media timeline ↔ network time（PTP）映射 |
| 设备侧能力位 | bit 59 `SupportsAudioStreamConnectionSetup`、bit 60 `SupportsAudioMediaDataControl`、扩展 bit 72 `SupportsBufferedAPAP` |

本机实测（`--list`）HomePod mini 27.2 的广播确实带这些能力位：
`features=0x4A7FCA00,0x3C356BD0`。

### 未知（**没有抓包就没有发言权，这些只能标 TODO**）

| 未知项 | 影响 | 怎么解决 |
|---|---|---|
| APAP 头 15 字节的完整字段布局 | 分帧写不对就整条流不可用 | 抓包比对 |
| MDC 的密钥派生（`encryptionSeed` 怎么用） | 解不开/发不出控制命令 | 抓包 + 试错 |
| `strt` / `anch` 是否都必须发 | 决定第 3 阶段是否存在 | 抓包看 Apple 实际发的顺序 |
| 是否必须 PTP，还是 `srat`（RTP 时间锚点）可用 | **决定项目可行性** | 抓包确认 |
| 接收端是否接受「无 MDC 的纯 APAP 音频」 | 决定第 1 阶段能否独立验收 | 直接试 |

最后一项是整个评估里最关键的分叉：如果 `srat` 那条 RTP 锚点的路能走通，
就不需要自己实现 PTP，总工量降到 20 人日以内；如果必须 PTP，Windows 上没有现成
协议栈（shairport 用的是独立守护进程 nqptp，靠 raw socket 占 319/320 端口），
自研等于再开一个项目。

---

## 四、接进现有引擎要动哪里

引擎是 sans-i/o 的：`RaopSender` 不持有 socket，只通过 `RaopIo`（六件事：
TCP connect/send/close、UDP bind/send/close）向宿主提要求。这个接缝本身**很干净**。

| 改动点 | 现状 | 需要做什么 | 量级 |
|---|---|---|---|
| `raop_io.h` 的 `RaopTcp` 枚举 | `Control` / `Event` 两个固定槽 | 加 `Media` 槽 | 极小 |
| `RaopLoop`（宿主实现） | 已有 poll 循环管两个 TCP 槽 | 第三个槽接进同一循环 | 小（~50 行） |
| 音频发送 | 一个函数内完成「RTP 头 + ALAC/PCM + ChaCha20 加密 + `udpSend` + 重传队列」 | 并列一条「APAP 头 + AAC + TCP 写」分支 | 中（~100 行） |
| 编码 | `encodeAlacFrame_` 手写 ALAC（~75 行，无外部依赖） | 需要 **AAC 编码器**（当前项目没有） | **大** |
| 时序 | `timingProtocol=NTP`，sync 包 + `rtptime` | MDC 锚点 / PTP | **大且未知** |

### AAC 编码器选型

| 方案 | 优点 | 缺点 |
|---|---|---|
| Windows Media Foundation AAC MFT | 系统自带、无需打包、无授权问题 | COM + 流式 MFT 管道，代码量不小；只在 Windows 可用 |
| fdk-aac | 质量好、跨平台 | 授权条款要单独评估；给单文件 exe 加依赖 |
| 自带简易 AAC-LC | 完全可控 | **不现实**，AAC 的心理声学模型不是顺手能写的 |

本项目是 Windows 单文件程序，**MF 是唯一合理选择**，但要把 MFT 的
`ProcessInput`/`ProcessOutput` 循环、时间戳、输出缓冲全部接进现有的 pacer 节拍。

---

## 五、退路

任何阶段失败都应能回到 type 96，不影响现有用户：

1. 新增路径用**独立的编译开关 + 运行时开关**，默认关闭；
2. `Session::start()` 里只在开关打开时请求 type 103；
3. 接收端不认（SETUP 报错或协商回落）就自动退回 type 96 —— 这条必须在第 1 阶段就做，
   否则测试期间用户会直接没声音。

---

## 六、如果要启动，第一步做什么

**不是写代码，是搭抓包环境。** 具体：

1. 找一台能镜像端口的交换机，或把 iPhone + HomePod 挂在一个可抓包的网桥上；
2. 让 iPhone 播一首歌到 HomePod（会走 type 103），抓下 RTSP + APAP + MDC 三条流；
3. 用抓到的包回答「三、未知」里的五个问题；
4. 只有「是否必须 PTP」这一项有确定答案后，才值得决定做不做。

没有第 2 步的抓包，后面所有工量估算都只是纸面数字。
