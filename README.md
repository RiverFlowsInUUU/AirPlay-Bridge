# AirPlay Bridge

<p align="center">
  <img src="src/icons/preview_connected.png" width="128" alt="AirPlay Bridge">
</p>

把 Windows 的系统声音实时串流到 AirPlay 2 设备，跑在托盘里。

TuneBlade 在 HomePod OS 27 失效后的替代方案。

## 安装

从 [Releases](https://github.com/RiverFlowsInUUU/AirPlay-Bridge/releases/latest) 下载 exe，双击运行。

Windows 10 / 11，64 位。不需要安装，不需要运行库。

> 首次运行 Windows 可能拦一次，点「更多信息」→「仍要运行」。

## 特性

- 自动发现局域网内的设备
- 接管模式：电脑自己静音，声音只从设备出
- 断线自动重连，设备换 IP 也能找回
- 被强杀后，下次启动自动恢复音频设备
- 延迟 / 音量 / 音质可调
- 开机自启，记住上次设置

## 使用

托盘图标左键或右键都能打开菜单。

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

## 两种模式

| 模式 | 需要虚拟声卡 | 电脑音箱 |
|---|---|---|
| 监听复制 | 否 | 和 HomePod 同时出声 |
| 接管 | [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) | 静音 |

接管模式下，程序在连接时把默认播放设备切到虚拟声卡，断开时切回。

## 三个可调项

| 项 | 范围 | 生效 |
|---|---|---|
| 延迟 | 60 – 1000 ms，5 档 + 滑块 | 重连后 |
| 音量 | 5 – 100%，20% 一档 + 滑块 | 立即 |
| 音质 | 0 – 100%，5 档 + 滑块 | 重连后 |

音质档位：快速 / 标准 / 高 / 极高 / 至臻。

## 命令行

| 参数 | 说明 |
|---|---|
| （无） | 托盘模式 |
| `--console` | 命令行模式 |
| `--list` | 列出发现的设备 |
| `<ip>` | 直连指定 IP |
| `--last` | 重连上次的设备 |
| `--install` | 注册开机自启 |
| `--uninstall` | 取消开机自启 |
| `--audio-devices` | 列出音频设备 |
| `--set-default <名>` | 切换默认播放设备 |

## 没声音

1. 菜单里确认显示「已连接」
2. 跑 `airplay_bridge.exe --audio-devices`，确认默认播放设备是你要的那个
3. 看日志

日志在 `%LOCALAPPDATA%\AirPlayBridge\bridge.log`。超过 30 天或 512KB 自动清理。

## 已知限制

- 接管模式需要装虚拟声卡，没装只能用监听复制
- 延迟和音质改档需要重连一次
- 仅支持 Windows
- 事件通道断线不重连，有上层 6 秒兜底

## 相关

- 构建、验收、发布 → [AGENTS.md](AGENTS.md)
- 架构与实测数据 → [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- 设计决策 → [docs/DECISIONS.md](docs/DECISIONS.md)
- 术语表 → [docs/GLOSSARY.md](docs/GLOSSARY.md)
- 变更记录 → [CHANGELOG.md](CHANGELOG.md)

## 致谢

- [airplay2-sender-cpp](https://github.com/akustikrausch/airplay2-sender-cpp) — AirPlay 2 协议实现（Apache-2.0）
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) — 加密
- [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) — 虚拟声卡

## 许可证

Apache-2.0
