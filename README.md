# AirPlay Bridge

<p align="center">
  <img src="src/icons/preview_connected.png" width="128" alt="AirPlay Bridge">
</p>

**English** · [中文](README.zh-CN.md)

Streams Windows system audio to AirPlay 2 devices, from the tray.

A replacement for TuneBlade, which stopped working on HomePod OS 27 —
and a free, open-source option alongside WinAirCast, ToneRelay and SoundHop.

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

**Audio & connection**

- 🆓 Free and open source (Apache-2.0)
- 🎵 AirPlay 2 with secure pairing (plus legacy AirPlay 1) — HomePod,
  Apple TV and other AirPlay receivers
- 🎚 Lossless 16-bit/44.1 kHz; latency (100–300 ms), volume, resampler
  quality and standby are all adjustable
- 💪 Glitch-resistant under load — audio threads run under MMCSS
  Pro Audio scheduling, and the 1 ms timer stays honored on Windows 11
  even when the app is in the background

**Reliability**

- 🔄 Auto-reconnect — after drops, IP changes, or when another device
  steals the receiver
- ⏰ Sleep/wake — streaming resumes on its own after the PC wakes
- 🛟 Force-kill safe — the audio device and mute state are restored on
  next launch
- 💾 Config is written atomically and auto-recovered from a backup if
  the file ever gets corrupted
- 🩺 Capture watchdog — an unplugged or busy capture device falls back
  to the default or reconnects automatically

**Everyday use**

- 🔎 Finds AirPlay devices on the local network
- 🙈 Hide devices you don't want to see
- 🔇 Mutes this PC on connect — and un-mutes the right device, even if
  the default output changed mid-session
- 😴 Standby when silent — device screen off, back on the first sound
- 🖥 Starts at login, remembers your settings, one instance at a time
- 🌐 English or Chinese interface
- ℹ️ Version and update check under **About**

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

| Item | Presets | Range | Applies |
|---|---|---|---|
| Latency | 130 / 150 / **200** / 250 / 300 ms | continuous slider (100–300) | on reconnect |
| Volume | Off / 30% / 50% / 70% / 100% | continuous slider (0–100) | immediately |
| Quality | Fast / Standard / High / Very high / Maximum | continuous slider (0–100) | on reconnect |
| Standby when silent | 5 / 10 / 15 / 20 s | continuous slider (0–20) | immediately |

Bold marks the default shipped with the app. Latency below 250 ms risks
crackling; 250 ms is the tested sweet spot and 130 ms is experimental.
Standby at 0 turns the feature off.

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

## 🧩 Compatibility

| Receiver | Firmware | Status |
|---|---|---|
| HomePod mini (1st gen) | 27.2 | Verified: connect, stream, standby, wake |
| HomePod (1st gen) | 27.2 | Not tested |
| HomePod mini 2 | 27.2 | Not tested |
| HomePod (2nd gen) | 27.2 | Not tested |
| Apple TV 4K | 27.2 | Not tested |

AirPlay 1 (RAOP) stops working on receiver OS 27 and later. This app uses AirPlay 2 and is unaffected.
Receiver firmware 27.0 / 27.1 uses the same AirPlay 2 path; no differences seen.
The receiver firmware version is shown in the About dialog and in the log.

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
