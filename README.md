# AirPlay Bridge

<p align="center">
  <img src="src/icons/preview_connected.png" width="128" alt="AirPlay Bridge">
</p>

**English** · [中文](README.zh-CN.md)

Streams Windows system audio to AirPlay 2 devices, from the tray. A free,
open-source replacement for TuneBlade after HomePod OS 27 — covers HomePod,
Apple TV and other receivers.

## 📦 Install

Download `AirPlay-Bridge-Setup-vX.Y.Z.exe` from
[Releases](https://github.com/RiverFlowsInUUU/AirPlay-Bridge/releases/latest)
and run it. Windows 10 / 11, 64-bit.

Installs to `Program Files\AirPlayBridge` with its runtime bundled —
**upgrades need no reconfiguration**, since the firewall rule and startup entry
follow that location. Windows may block it once: **More info** → **Run anyway**.

### 🔊 When you need a virtual audio cable

| Your setup | Install it? |
|---|---|
| Speakers or headphones that would play the same audio | **Yes**, otherwise the PC plays too (about 200 ms behind — sounds like an echo) |
| A sound card with nothing plugged in | No, nobody hears it |
| No playback device listed in Windows | **Yes**, there is nothing to capture |

Install [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (free, reboot
after). On connect the app switches the default device to it, so system audio
reaches the AirPlay device only. The cable never changes what gets streamed.

## 🚀 First run

- Asks for interface language and whether to start at login (once each)
- Scans the local network, connects what it finds, and checks the firewall rule
  (one-click fix if it's missing)

## ✨ Features

**Audio & connection**

- 🆓 Free and open source (Apache-2.0)
- 🎵 AirPlay 2 with secure pairing (plus legacy AirPlay 1) — HomePod, Apple TV
  and other receivers
- 🎚 Lossless 16-bit/44.1 kHz; latency (100–300 ms), volume, quality and
  standby are all adjustable
- 💪 Glitch-resistant under load — audio threads run under MMCSS Pro Audio, and
  Windows 11 keeps the 1 ms timer even in the background

**Reliability**

- 🔄 Auto-reconnect — after drops, IP changes, or when another device takes over
- ⏰ Sleep/wake — streaming resumes on its own after the PC wakes
- 🛟 Force-kill safe — audio device and mute state are restored on next launch
- 💾 Config written atomically, auto-recovered from a backup if corrupted
- 🩺 Capture watchdog — an unplugged or busy device falls back or reconnects

**Everyday use**

- 🔎 Finds AirPlay devices, 🙈 hides the ones you don't want
- 🔇 Mutes this PC on connect, and restores the right device afterwards
- 😴 Standby when silent — screen off, back on the first sound
- 🖥 Starts at login, remembers settings, one instance at a time
- 🌐 English or Chinese; ℹ️ version and update check under **About**

## 🖱 Usage

Left- or right-click the tray icon:

```
✓ Connected
  <device name> / <device IP>
────────────────
  Disconnect
  Devices  ›     ← devices found, Scan again, Hide devices, Capture from
  Latency  200ms  ›
  Volume  45%  ›
  Quality  Maximum  ›
────────────────
  More settings  ›  ← Connection check, Standby when silent, Mute this PC,
                      Run at startup, Language, Logs
  About
────────────────
  Quit
```

When the rule is fine, **More settings › Connection check** says so.

## 🎛 Adjustable

| Item | Presets | Range | Applies |
|---|---|---|---|
| Latency | 130 / 150 / **200** / 250 / 300 ms | continuous slider (100–300) | on reconnect |
| Volume | Off / 30% / 50% / 70% / 100% | continuous slider (0–100) | immediately |
| Quality | Fast / Standard / High / Very high / Maximum | continuous slider (0–100) | on reconnect |
| Standby when silent | 5 / 10 / 15 / 20 s | continuous slider (0–20) | immediately |

Bold marks the default. Latency below 250 ms risks crackling; 250 ms is the
tested sweet spot and 130 ms is experimental. Standby at 0 turns it off.

## 🔇 No sound

1. Confirm the tray menu shows **Connected**
2. Check the default playback device in Windows sound settings
3. Tray menu → More settings → Logs

## ⏳ Stuck on "Connecting"

The receiver calls back into this PC for clock sync, so the app needs an inbound
firewall rule. It checks this at startup:

| Step | Action |
|---|---|
| 1 | Tray menu → More settings → Connection check |
| 2 | Approve the admin prompt |

By hand: Windows Security → Firewall & network protection → Allow an app
through firewall → AirPlay Bridge → tick **Private**. Not listed? **Allow
another app** → **Browse**.

## ⚠️ Known limitations

- Without a virtual audio cable, sound stays on this PC's output device (muted
  by default — switchable from the menu)
- Changing latency or quality requires one reconnect
- Windows only
- A few seconds of silence after a drop

## 🔗 Related

- Report a problem → [Issues](https://github.com/RiverFlowsInUUU/AirPlay-Bridge/issues)
- Change log → [CHANGELOG.md](CHANGELOG.md)
- Development docs → [AGENTS.md](AGENTS.md) · [docs/](docs/)

## 🙏 Credits

- [airplay2-sender-cpp](https://github.com/akustikrausch/airplay2-sender-cpp) — AirPlay 2 protocol (Apache-2.0)
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) — cryptography
- [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) — virtual audio device

## 📄 License

Apache-2.0
