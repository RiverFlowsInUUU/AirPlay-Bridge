# 关键决策

面向 AI agent：为什么是这样。改代码前先读，避免推翻有理由的设计。
约束逐条在 [AGENTS.md](../AGENTS.md)，模块地图在 [ARCHITECTURE.md](ARCHITECTURE.md)。

## D1 — 用 AirPlay 2，而不是修 AirPlay 1

**背景**：TuneBlade（走 AirPlay 1 / RAOP）在 HomePod OS 27 上失效，
Airfoil、Roon 等一起哑火。Rogue Amoeba 确认为 Apple 侧改动（bug report #FB24452968）。

**决策**：接入走 AirPlay 2 的开源协议实现，不复刻 AirPlay 1。

**理由**：AirPlay 1 的握手路径已经是 Apple 单方面控制的历史包袱，闭源工具没有修复可能。
AirPlay 2 是当前协议，Apple 仍在维护。

**代价**：协议跟进依赖上游 `airplay2-sender-cpp` 继续维护。

## D2 — 设备发现用单播 mDNS

**决策**：先扫网段找 7000 端口开放的 IP，再对每个 IP 发**单播** mDNS 查询。

**理由**：Windows 上组播（224.0.0.251）经常被网卡或路由器拦掉，实测零应答。

**被否决**：直接用组播 mDNS —— 在目标机器上不可用。

**相关事实**：7000 端口开放是 HomePod 的 AirPlay 标志。

## D3 — 接管模式靠切默认播放设备，不写驱动

**决策**：连上设备时把系统默认播放设备切到虚拟声卡（CABLE Input），断开时切回。
使用未公开接口 `IPolicyConfig`。

**理由**：自写虚拟音频驱动需要处理驱动签名，且用户安装成本高。
配合现成的 [VB-Audio Virtual Cable](https://vb-audio.com/Cable/)，用户装一次即可。

**实现陷阱**：`sysvolume.cpp` 定义两个变体 ——

| 接口 | `ResetDeviceFormat` | CLSID | 适用 |
|---|---|---|---|
| `IPolicyConfig` | 有 | `{870af99c-...}` | 旧系统 |
| `IPolicyConfigVista` | 无 | `{294935CE-...}` | Win11（老 CLSID 未注册） |

**两个接口的 vtable 顺序不同**，混用会让 `SetDefaultEndpoint` 落到错误的槽位。
Vista 版还需 IID `{568b9108-44bf-b4-9006-86afe5b5a620}`。

**被否决**：自写虚拟驱动。

## D4 — 崩溃保护靠配置文件留标记

**决策**：切到虚拟声卡**之前**，把原设备名写进 `bridgeConfig.preCaptureDevice`。
启动时若该字段非空且当前默认是 CABLE，判定为上次异常退出，执行恢复。

**理由**：程序被强杀时没有机会执行清理，必须在启动侧兜底。

**关键约束**：**只有恢复成功才清标记**。失败必须保留标记，
否则用户会永久失去声音且程序不再尝试恢复。

## D5 — 重采样做在采集侧，不改协议引擎

**决策**：在采集回调里先把音频转到 44100，再 `setInputFormat(44100)`，
让引擎走 pass-through 分支。

**理由**：引擎自带两点线性插值，15kHz 以上信噪比塌到 6–14dB。
若直接改引擎源码，本地补丁要随上游每次变动重新适配。
做在采集侧**完全不动第三方代码，补丁不需要跟着更新**。

**代价**：必须自己实现并维护重采样器（`src/audio/resampler.h`）。
质量实测见 [ARCHITECTURE.md](ARCHITECTURE.md)。

**被否决**：改引擎的重采样实现。

## D6 — 采集默认单设备

**决策**：`CaptureMode::DefaultOnly`。多设备混音（`AllDevices`）实现保留但不启用。

**理由**：实测多台设备同时打开时采集全是静音，原因未查明（见 [AGENTS.md K2](../AGENTS.md)）。
接管模式下也不需要 —— 声音都进虚拟声卡了。

## D7 — 单文件 exe，静态链接 CRT

**决策**：产物是单个 `.exe`，静态链接 CRT，配置写在 `%LOCALAPPDATA%`。
不提供安装包。

**理由**：用户下载即可运行，不需要装运行库，不需要处理安装/卸载。
代价是体积略大。

## D8 — 菜单状态用系统勾选态，不用颜色

**决策**：显示「已连接」用系统自带的勾选态（`checked=true`，显示为蓝色勾），
不使用 emoji 或自定义位图。

**理由**：Windows 原生菜单不支持 emoji 着色，`SetMenuItemBitmaps` 也不生效。
系统勾选态是唯一可靠的上色方式。

**相关约束**：用 `enabled=false` 显示状态会让整行变灰（连右侧数值一起灰掉），
因此不可选的行应做成「可点但空操作」。

## D9 — 三个可调项都做固定档 + 连续滑块

**决策**：延迟、音量、音质都提供固定档位作为快捷入口，同时提供连续滑块。

**理由**：固定档位让用户快速落在一个合理值；滑块覆盖不同网络与设备组合的差异，
不必为每个场景预设档位。仅做固定档会让精细调节无从下手，
仅做滑块则没有推荐值。
