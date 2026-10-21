# AirPlay Bridge

<p align="center">
  <img src="src/icons/preview_connected.png" width="128" alt="AirPlay Bridge">
</p>

**English** · [中文](README.zh-CN.md)

Streams Windows system audio to AirPlay 2 devices, from the tray.

A replacement for TuneBlade, which stopped working on HomePod OS 27.

## 📦 Install

Download `AirPlay-Bridge-Setup-vX.Y.Z.exe` from
[Releases](https://github.com/RiverFlowsInUUU/AirPlay-Bridge/releases/latest)
and run it. No virtual audio device needed.

Windows 10 / 11, 64-bit. The installer bundles its runtime and installs to
`Program Files\AirPlayBridge` — **upgrades need no reconfiguration**, since the
firewall rule and startup entry follow that location.

> Windows may block it once on first run — click **More info** → **Run anyway**.

### 🔊 When you need a virtual audio cable

Install [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (free, reboot after)
when this PC has speakers or headphones that would play the same audio.

On connect the app switches the default playback device to the cable, so system
audio reaches the AirPlay device only.

No audible output? Skip it — a dummy or remote-playback endpoint, a headless mini
PC, or speakers that stay off. The app still streams, and mutes this PC by
default.

One exception: if Windows lists no playback device at all, there is nothing to
capture. The cable provides one.

> The cable never changes what gets streamed — the audio is identical either way.

## 🚀 First run

- Asks for interface language and whether to start at login (once each)
- Scans the local network and connects to what it finds
- Checks the firewall rule, with a one-click fix if it's missing

## ✨ Features

- 🔎 Finds AirPlay devices on the local network
- 🔄 Reconnects after a drop, even if the device changes IP
- 🛟 Restores the audio device on next launch after a force-kill
- 🔇 Mutes this PC on connect — sound comes from the AirPlay device only
- 😴 Standby when silent — device screen off, back on the first sound
- 🎚 Latency / volume / quality / standby / capture device, all adjustable
- 🙈 Hide devices you don't want to see
- 🌐 English or Chinese interface
- 🖥 Starts at login, remembers your settings
- ℹ️ Version and update check under **About**
- 1️⃣ One instance at a time, so two copies can't fight over a device

## 🖱 Usage

Left- or right-click the tray icon.

```
✓ Connected
  <device name>
  <device IP>
────────────────
  Disconnect
  Devices  ›
     <device name>        ← devices found are listed right here
     Scan again
     ──────────────
     Hide devices  ›
     Capture from  ›
  Latency  200ms  ›
  Volume  45%  ›
  Quality  Maximum  ›
────────────────
  More settings  ›
     Connection check
     Standby when silent  ›
     Mute this PC
     Run at startup
     Language  ›
     Logs
  About
────────────────
  Quit
```

When the rule is fine, **More settings › Connection check** says so.

## 🎛 Adjustable

| Item | Range | Applies |
|---|---|---|
| Latency | 130 – 300 ms, 5 presets + continuous slider (100–300) | on reconnect |
| Volume | 0 – 100%, Off / 30% / 50% / 70% / 100% + continuous slider | immediately |
| Quality | 0 – 100%, 5 presets + slider | on reconnect |
| Standby when silent | 0 – 20 s, 4 presets + slider (0 = off) | immediately |

Latency presets: 130 / 150 / 200 / 250 (recommended) / 300 ms. Below 250 ms risks
crackling; 130 ms is experimental.

Volume presets: Off / 30% / 50% / 70% / 100%.

Quality presets: Fast / Standard / High / Very high / Maximum (default).

Standby presets: 5 / 10 / 15 / 20 s (5 s default). The slider is continuous
0 – 20 s; 0 turns it off.

## 🔇 No sound

1. Confirm the tray menu shows **Connected**
2. Check the default playback device in Windows sound settings
3. Tray menu → More settings → Logs

## ⏳ Stuck on "Connecting"

The receiver calls back into this PC for clock sync, so the app needs an inbound
firewall rule. It checks this at startup; if the rule is missing:

| Step | Action |
|---|---|
| 1 | Tray menu → More settings → Connection check |
| 2 | Approve the admin prompt |

By hand: Windows Security → **Firewall & network protection** → **Allow an app
through firewall** → AirPlay Bridge → tick **Private**. Not listed? **Change
settings** → **Allow another app** → **Browse**.

## ⚠️ Known limitations

- Without a virtual audio cable, sound stays on this PC's output device (muted by
  default — switchable from the menu)
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
