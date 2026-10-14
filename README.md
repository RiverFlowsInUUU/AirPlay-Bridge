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
- 电脑可完全静音，声音只从设备出
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

## 电脑是否同时发声

程序根据系统里是否装有虚拟声卡自动决定，不需要手动切换。

| 模式 | 虚拟声卡 | 连接时 | 电脑扬声器 |
|---|---|---|---|
| 接管 | 有 | 默认播放设备切到虚拟声卡，断开时切回 | 静音 |
| 监听复制 | 无 | 直接采集默认播放设备的输出 | 与 AirPlay 设备同时发声 |

不希望两边同时出声，装一次 [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) 即可
（免费，装完需重启）。电脑本身没有外放设备时，两种模式听感相同。

## 三个可调项

| 项 | 范围 | 生效 |
|---|---|---|
| 延迟 | 60 – 1000 ms，5 档 + 滑块 | 重连后 |
| 音量 | 5 – 100%，20% 一档 + 滑块 | 立即 |
| 音质 | 0 – 100%，5 档 + 滑块 | 重连后 |

音质档位：快速 / 标准 / 高 / 极高 / 至臻。

## 没声音

1. 托盘菜单里确认显示「已连接」
2. 打开 Windows 的声音设置，确认默认播放设备是正确的那个
3. 托盘菜单 → 打开日志文件夹，看日志

日志超过 30 天或 512KB 自动清理。

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
