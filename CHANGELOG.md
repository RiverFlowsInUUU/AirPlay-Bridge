# 更新日志

本项目遵循 [语义化版本](https://semver.org/lang/zh-CN/)，
格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

## [1.5.39] - 2026-10-24

**中文**

### 修复

- 修复点「连接诊断」或「清理防火墙规则」时声音卡住最长 20 秒的问题。
- 修复在禁止修改防火墙规则的电脑上点「连接诊断」，反而把原本能用的
  放行规则删光、彻底连不上的问题。
- 修复设备列表里偶尔出现「某台音箱的名字配着另一台机器的 IP」、
  点了却连不上的问题。

**English**

### Fixed

- Fixed playback stalling for up to 20 seconds after clicking Connection check
  or Clean firewall rules.
- Fixed Connection check wiping out the working firewall rule on PCs where
  rules cannot be modified, leaving the app unable to connect at all.
- Fixed the device list occasionally showing one speaker's name with another
  machine's IP, which failed to connect when clicked.

## [1.5.38] - 2026-10-24

**中文**

### 修复

- 修复命令行模式（`--console` 或直接指定 IP）被强行结束后，电脑可能彻底
  没声音、重启程序也救不回来的问题。

**English**

### Fixed

- Fixed the PC ending up completely silent after console mode (`--console` or
  a direct IP) was killed, unrecoverable by restarting the app.

## [1.5.37] - 2026-10-24

**中文**

### 修复

- 修复日志里同一条诊断每 5 秒重复写一次、长时间收听会把日志刷满的问题。

**English**

### Fixed

- Fixed the same diagnostic being written every 5 seconds, flooding the log
  during long listening sessions.

## [1.5.36] - 2026-10-24

**中文**

### 修复

- 修复部分声卡在静音后收不到声音、周期性断连仍未解决的问题。
- 修复把「连接时静音电脑扬声器」关掉再打开后，自动静音不恢复、需重启的问题。
- 修复待机唤醒后连接失败时，之后不相关的会话会沿用最灵敏判断的问题。
- 修复重连间隔最长会到 80 秒、与说明不符的问题（现在最长 60 秒）。

**English**

### Fixed

- Fixed periodic disconnects on sound cards that stop capturing while muted
  still not being resolved.
- Fixed automatic muting not coming back after toggling Mute local output off
  and on, requiring a restart.
- Fixed sessions unrelated to a failed wake-from-standby reconnect using the
  most sensitive detection afterwards.
- Fixed the reconnect interval capping at 80 s instead of the documented 60 s.

## [1.5.35] - 2026-10-24

**中文**

### 修复

- 修复部分声卡在静音后收不到声音的问题没被检测到、周期性断连依旧的问题。
- 修复撤销静音后重连又立刻静音、造成来回抖动的问题。
- 修复设备掉线后重连间隔不再增长、一直每 10 秒重试的问题。
- 修复英文界面下重新扫描为空时仍显示中文的问题。

**English**

### Fixed

- Fixed periodic disconnects on sound cards that stop capturing while muted
  still not being detected.
- Fixed the mute being re-applied right after reconnecting, causing the two to
  fight each other.
- Fixed the reconnect interval no longer growing after a device dropped off,
  retrying every 10 seconds indefinitely.
- Fixed Chinese still showing in the English UI when a rescan finds nothing.

## [1.5.34] - 2026-10-24

**中文**

### 修复

- 修复「采集设备」子菜单里选具体设备时没有反应的问题。
- 修复未连接时收到通知后，托盘图标一直显示为已连接、且不会自行恢复的问题。
- 修复拔掉 USB 声卡后每次连接都失败、需要手动重选设备的问题。
- 修复「连接诊断」和「清理防火墙规则」在部分机器上无法启动的问题。
- 修复两台设备争用同一个接收端时，每 5 秒断连重连且不会停止的问题。
- 修复部分声卡在静音后收不到声音、导致周期性断连的问题。
- 修复英文界面下仍有 9 处中文提示的问题。
- 修复延迟设置的英文说明与中文阈值不一致的问题。
- 修复其他程序取消注销时，本程序提前退出的问题。
- 修复中文设备名在托盘菜单里被截断成乱码的问题。
- 修复静默卸载在清理失败时会卡住、无人应答导致永久挂起的问题。
- 修复非中文系统安装时默认预选简体中文界面的问题。

**English**

### Fixed

- Fixed picking a specific device in the Capture from submenu doing nothing.
- Fixed the tray icon staying on "connected" after a notification while
  disconnected, with no way to recover.
- Fixed every connection attempt failing after a USB sound card was unplugged.
- Fixed Connection check and Clean firewall rules failing to start on some PCs.
- Fixed reconnecting every 5 seconds without stopping when two devices compete
  for the same receiver.
- Fixed periodic disconnects on sound cards that stop capturing while muted.
- Fixed nine remaining Chinese strings in the English UI.
- Fixed the English latency note disagreeing with the Chinese one.
- Fixed the app closing itself when another program cancelled the shutdown.
- Fixed Chinese device names showing as garbage in the tray menu.
- Fixed silent uninstalls hanging forever when cleanup failed.
- Fixed non-Chinese systems defaulting to the Simplified Chinese installer.

## [1.5.33] - 2026-10-23

**中文**

### 变更

- 鼠标悬停在托盘图标上，现在分行显示状态、设备名和 IP 地址 ——
  连着哪台设备一眼可见，两台同型号设备也能分清。
- 连接状态不再附带「未装虚拟声卡」的括号说明。

**English**

### Changed

- Hovering the tray icon now shows the status, device name and IP on separate
  lines, so you can tell which device is connected — including two units of the
  same model.

- The connected tooltip no longer appends a note about the missing virtual audio
  device.

## [1.5.32] - 2026-10-19

**中文**

### 修复

- 修正一处代码缩进（不影响行为）。

**English**

### Fixed

- Corrected a code indentation (no behaviour change).

## [1.5.31] - 2026-10-19

**中文**

### 修复

- 修正代码注释中一处过时的说明（不影响行为）。

**English**

### Fixed

- Corrected an outdated code comment (no behaviour change).

## [1.5.30] - 2026-10-19

**中文**

### 修复

- 修复播放中音频设备失效后，偶尔需要重启程序才能恢复的问题。

### 安全

- 接收端发来数值非法的内容长度时，事件通道现在会被关闭，不会再被当成
  正常请求处理。

**English**

### Fixed

- Fixed the audio occasionally needing a restart to recover after the audio device
  went away during playback.

### Security

- A content length that is not a valid number now closes the event channel instead
  of being treated as a normal request.

## [1.5.29] - 2026-10-19

**中文**

### 修复

- 修复播放中拔掉声卡或禁用输出设备时，状态栏提示为「采集启动失败」的问题；
  现在会明确提示「音频设备已失效」。
- 修复关闭「连接时静音电脑扬声器」后，若当时恢复失败，退出后手动设置的静音
  会在下次启动时被取消的问题。

**English**

### Fixed

- Fixed the status message saying "capture failed to start" when the audio device
  went away during playback; it now says "audio device disconnected".
- Fixed a mute you set manually being turned off on the next launch after turning
  off "Mute computer speakers on connect" and the restore having failed.

## [1.5.28] - 2026-10-19

**中文**

### 修复

- 发布包里附上 SHA-256 校验和，方便下载后核对文件完整性。

**English**

### Fixed

- The release notes now include the SHA-256 checksum of the installer.

## [1.5.27] - 2026-10-19

**中文**

### 修复

- 修复播放中拔掉声卡、或在系统里禁用当前输出设备后，程序仍显示「已连接」
  但完全没有声音、也不会自动恢复的问题。
- 修复关闭「连接时静音电脑扬声器」开关后，若当时恢复失败就不会再重试的问题。

### 安全

- 接收端发来畸形的长度字段时，现在会直接断开事件通道，而不是忽略它继续。

**English**

### Fixed

- Fixed the app continuing to show "Connected" with no audio, and never recovering,
  after the sound card was unplugged or the current output device was disabled
  while playing.
- Fixed the speaker mute not being retried after turning off "Mute computer
  speakers on connect" when that restore failed.

### Security

- A malformed length field from the receiver now closes the event channel instead
  of being ignored.

## [1.5.26] - 2026-10-18

**中文**

### 修复

- 卸载程序不再删除安装目录里的其它文件。此前如果把程序装进一个已有内容的
  文件夹，卸载会把它整个删掉。
- 修复「无声音时待机」开启时，在待机状态下退出程序会让进程异常终止的问题。
- 修复切换默认播放设备后若采集无法启动，电脑的声音不会切回原设备的问题。
- 修复音频采集无法启动时界面仍显示「已连接」、但完全没有声音的问题；
  现在会提示并自动重连。
- 修复设备名较长时托盘提示整条变空白的问题。

### 安全

- 接收端持续发送异常数据时，事件通道不再无限占用内存。

**English**

### Fixed

- The uninstaller no longer deletes other files from the installation folder.
  Installing into a folder that already had content used to wipe it on uninstall.
- Fixed the process terminating abnormally when quitting while the speaker was in
  standby.
- Fixed the computer audio not switching back to the original device when audio
  capture failed to start.
- Fixed the app showing "Connected" with no audio at all when capture failed to
  start; it now reports the problem and reconnects.
- Fixed the tray tooltip turning blank when a device name was long.

### Security

- The event channel no longer grows without bound when the receiver keeps sending
  malformed data.

## [1.5.25] - 2026-10-18

**中文**

### 修复

- 修复第一次运行时，若配置无法写入磁盘（目录不可写、磁盘已满），
  日志里不会留下任何记录的问题。

**English**

### Fixed

- Fixed the very first run leaving no trace in the log when the configuration
  could not be written to disk (unwritable folder, full disk).

## [1.5.24] - 2026-10-18

**中文**

### 修复

- 修复 Windows 用户名（或安装路径）含中文时，设置改动无法保存的问题。
  此前这种情况下改设置不生效、重启后全部回落默认，且这台电脑会被音箱当成新设备。

**English**

### Fixed

- Fixed settings failing to save when the Windows user name (or the install path)
  contains non-ASCII characters. Settings changes were silently lost, every value
  reverted to its default on restart, and the computer was treated as a new device
  by the speaker.

## [1.5.23] - 2026-10-18

**中文**

### 修复

- 修复英文界面下，把鼠标停在托盘图标上时显示的提示仍为中文的问题。
- 修复设置文件写入过程被打断（强制结束、断电）后，下次启动各项设置回落默认、
  且这台电脑被音箱当成一台新设备的问题。
- 修复设备名或状态文字较长时，托盘提示整条变成空白的问题；现在会截断显示。
- 修复诊断日志里「音频丢帧」计数虚报一倍的问题。

### 安全

- 检查更新时只会打开 github.com 上的链接；接口返回其它地址时忽略。

**English**

### Fixed

- Fixed the tray tooltip still being shown in Chinese when the interface language is
  English.
- Fixed settings falling back to defaults — and the computer being treated as a new
  device — after the settings file was interrupted mid-write.
- Fixed the tray tooltip turning completely blank when a device name or status text
  was long; it is now truncated instead.
- Fixed the dropped-frame count in the diagnostics log being reported at twice the
  real value.

### Security

- The update check now only opens links on github.com and ignores anything else the
  API returns.

## [1.5.22] - 2026-10-18

**中文**

### 修复

- 修复连接音箱时，托盘「已连接」提示自身的系统提示音被一并转发到音箱的问题。
  该提示音此前会混进转发的音频里，在音箱上表现为一声爆音。气泡提示本身不受影响。

**English**

### Fixed

- Fixed the system sound of the tray "connected" notification being forwarded to the
  speaker along with the audio. It used to mix into the forwarded stream and was heard
  as a pop. The notification itself still appears.

## [1.5.21] - 2026-10-18

**中文**

### 修复

- 修复刚连上、刚开始播放时的一声爆音。此前会话建立后的开头几十毫秒送往设备的是纯静音，
  真实音频接上时会出现突变；现在开头会平滑淡入。

**English**

### Fixed

- Fixed a pop heard right after connecting, when playback first starts. The first few
  tens of milliseconds sent to the device used to be pure silence, so the real audio
  arrived as an abrupt step; playback now fades in smoothly.

## [1.5.20] - 2026-10-17

**中文**

### 修复

- 修复低延迟档位下可能出现的零星爆音。此前送往设备的缓冲范围上限会随延迟设置
  等比缩小，最低档位下压缩得过紧；现在改用协议标准范围。

**English**

### Fixed

- Fixed occasional crackling at the low-latency presets. The buffer range sent to
  the device used to shrink along with the latency setting and became too tight at
  the lowest presets; the standard protocol range is now used.

## [1.5.19] - 2026-10-17

**中文**

### 修复

- 修复设备搜索计时在长时间开机后可能失去准确性的隐患。
  改用 64 位计时，不再依赖截断后的数值恰好抵消。

- 诊断日志超过 256 KB 时在启动时轮转一份，避免长期追加无限增长。

### 修改

- 移除一段不会执行到的提示分支，界面文案不变。

**English**

### Fixed

- Fixed a latent timing inaccuracy in device discovery on machines that stay
  powered on for a long time. Timing now uses 64-bit values instead of relying on
  truncation cancelling out.

- The diagnostics log now rotates at 256 KB on startup instead of growing forever.

### Changed

- Removed an unreachable message branch. On-screen wording is unchanged.

## [1.5.18] - 2026-10-17

**中文**

### 修复

- 修复同网段设备较多时，搜索设备可能长时间无响应的问题。
  此前候选设备越多等得越久（实测 254 个候选时最坏超过 2 分钟），
  期间界面和播放都会卡住；现在无论有多少候选，总耗时不超出既定上限。
- 修复同时安装两份副本时，「另一份在哪」的提示永远不出现的问题。
- 修复诊断日志在首次写入失败后永久静默的问题。
- 修复设备搜索在开机多周后可能返回空结果的问题。

**English**

### Fixed

- Fixed long freezes when scanning a busy network with many candidate devices.
  The scan used to scale with the candidate count (worst case over two minutes at
  254 candidates), blocking the interface and playback; total time is now capped.
- Fixed the "where is the other copy" hint never appearing when two copies are
  installed side by side.
- Fixed diagnostic logging going permanently silent after one failed write.
- Fixed device discovery returning nothing after the machine has been running for
  several weeks.

## [1.5.17] - 2026-10-17

**中文**

### 修复

- 修复采集设备采样率低于 44100 Hz 时的音质异常。此前 16 kHz、22.05 kHz、
  32 kHz、8 kHz 等设备（蓝牙免提模式、低端 USB 声卡、部分虚拟声卡）会读到
  无效数据，表现为高频失真。

- 上一版音质提升的实际幅度更大：按实际播放条件（分块送入）测量，
  19.9 kHz 处信噪比从 27.6 dB 提升到 41.7 dB。

**English**

### Fixed

- Fixed distorted audio when the capture device runs below 44100 Hz. Devices at
  16 kHz, 22.05 kHz, 32 kHz or 8 kHz (Bluetooth hands-free mode, low-end USB
  audio, some virtual devices) could pick up invalid data, heard as high-frequency
  distortion.

- The previous release improved audio more than stated: measured under real
  playback conditions (chunked input), signal-to-noise at 19.9 kHz rose from
  27.6 dB to 41.7 dB.

## [1.5.16] - 2026-10-17

**中文**

### 修复

- 修复音质随采集回调帧数变化的问题。此前同一段音频，换一个回调帧数就可能
  产生 10% 量级的样点差异，表现为高频失真和电平起伏；现在任何帧数下的结果
  完全一致，19.9 kHz 处的信噪比也从 39.5 dB 提升到 41.7 dB。

**English**

### Fixed

- Fixed audio quality varying with the capture callback size. The same input could
  differ by as much as 10% between callback sizes, causing high-frequency
  distortion and level fluctuation. Output is now identical at any callback size,
  and signal-to-noise at 19.9 kHz improved from 39.5 dB to 41.7 dB.

## [1.5.15] - 2026-10-17

**中文**

### 修复

- 修复注销或关机时没有把默认播放设备切回去的问题。此前这类退出后，
  下次开机电脑可能没有声音（音频仍送往虚拟声卡）。

**English**

### Fixed

- Fixed the default playback device not being restored on sign-out or shutdown.
  After such an exit the computer could come back silent, with audio still routed
  to the virtual cable.

## [1.5.14] - 2026-10-17

**中文**

### 修复

- 修复设备短暂连上又被断开后，重连间隔固定在 5 秒、反复重试的问题。
  此前一旦出现这种情况，程序会一直重连但始终连不上，直到设备自己释放会话
  （实测约需 90 秒）。现在会逐步拉长重试间隔，让设备有机会恢复。

**English**

### Fixed

- Fixed the reconnect loop getting stuck at a fixed 5-second interval after the
  device accepted a session and then dropped it. The app used to retry forever
  without ever succeeding until the device released the session on its own
  (about 90 seconds in testing). Retries now back off progressively.

## [1.5.13] - 2026-10-17

**中文**

### 修复

- 修复界面文字转换时的缓冲区越界写入。每次刷新菜单、提示或弹窗都会多写 2 字节，
  可能破坏相邻内存，表现为难以复现的随机异常。
- 修复切换采集设备与重连同时发生时，读取设备名可能访问已释放内存的问题。
- 修复配置文件里的音量和延迟超出范围时，菜单显示与实际不符的问题。

**English**

### Fixed

- Fixed an out-of-bounds write when converting interface strings. Every menu refresh,
  tooltip or dialog wrote 2 bytes past the buffer, potentially corrupting adjacent
  memory and causing intermittent, hard-to-reproduce faults.
- Fixed a use-after-free when the capture device was changed while reconnecting.
- Fixed the menu showing an out-of-range volume or latency value that did not match
  what was actually applied.

## [1.5.12] - 2026-10-17

**中文**

### 修改

- 延迟档位调整为 130 / 150 / 200 / 250 / 300 ms，250 ms 标为推荐值。
- 延迟滑块改为 100 – 300 ms 无极调节。
- 音量档位调整为 静音 / 30% / 50% / 70% / 100%。

**English**

### Changed

- Latency presets are now 130 / 150 / 200 / 250 / 300 ms, with 250 ms marked as recommended.
- The latency slider now adjusts freely from 100 to 300 ms.
- Volume presets are now Mute / 30% / 50% / 70% / 100%.

## [1.5.11] - 2026-10-17

**中文**

### 修复

- 修复装了虚拟声卡时，播放会周期性卡顿、断开并切回电脑扬声器的问题。
  启用「无声音时待机」时尤其明显 —— 每几秒断一次。
- 修复窗口被关闭时（任务管理器结束任务、或关机）程序不退出、
  且音频设备未还原的问题。

**English**

### Fixed

- Fixed periodic dropouts when a virtual audio cable is installed: playback
  could cut out and fall back to the PC speakers every few seconds.
- Fixed the app not exiting when its window was closed, leaving the audio
  device in a switched state.

## [1.5.10] - 2026-10-17

**中文**

### 修复

- 修复切换延迟档位后音量被重置的问题。掉线重连、从待机唤醒时同样会重置，
  一并修好。

**English**

### Fixed

- Fixed the volume being reset after changing the latency preset, after a
  reconnect, or when waking from standby.

## [1.5.9] - 2026-10-17

**中文**

### 修改

- 延迟档位调整为 105 / 110 / 120 / 150 / 200 ms，默认 200 ms。
- 滑块范围调整为 100 – 250 ms。
- 档位评语跟随调整（可能爆音 / 偶有爆音 / 稳定 / 推荐）。

**English**

### Changed

- Latency presets are now 105 / 110 / 120 / 150 / 200 ms, default 200 ms.
- Slider range is now 100 - 250 ms.
- Preset descriptions updated accordingly.

## [1.5.8] - 2026-10-17

**中文**

### 修复

- 修复低延迟档位（100 ~ 150ms）下接收端缓冲不足导致的爆音。
  设备此前被允许用比设定值更浅的缓冲播放，现在下限与设定的延迟一致。

**English**

### Fixed

- Fixed crackling at low latency settings (100 - 150 ms): the receiver could
  previously buffer less than the configured latency.

## [1.5.7] - 2026-10-17

**中文**

### 修复

- 修复启动后设备音量不跟随设置的问题 —— 此前每次启动都会以最大音量播放，
  只有在菜单里手动调一次才生效。

**English**

### Fixed

- Fixed the device volume not following your setting after a restart: playback
  started at full volume unless the volume was changed from the menu.

## [1.5.6] - 2026-10-17

**中文**

### 修改

- 音频数据发送得更密集（间隔从中位约 20ms 缩到约 4ms），用于改善
  低延迟档位下的播放稳定性。

**English**

### Changed

- Audio packets are now sent more evenly (median interval from ~20 ms down
  to ~4 ms), improving stability at low latency settings.

## [1.5.5] - 2026-10-17

**中文**

### 修改

- 延迟滑块的下限从 120ms 放宽到 60ms，便于在低延迟下试验；
  菜单里的档位（150 / 200 / 250 / 300ms）保持不变。

**English**

### Changed

- Lowered the latency slider minimum from 120 ms to 60 ms for
  experimentation. The presets in the menu are unchanged.

## [1.5.4] - 2026-10-17

**中文**

### 修复

- 从菜单里选定的延迟档位现在会被记住，重启后不再被改回默认值。
- 已有一个程序在运行时，再次打开弹出的提示会跟随界面语言；
  此前在中文界面下仍显示英文。

**English**

### Fixed

- The latency preset you pick from the menu is now remembered across restarts.
- The "already running" message now follows the interface language.

## [1.5.3] - 2026-10-17

**中文**

### 修改

- 播放时音频发送的节奏更均匀，有助于减少低延迟档位下的偶发卡顿。
  播放结束后自动恢复系统默认设置，空闲时不占额外开销。

**English**

### Changed

- Smoother audio pacing while streaming, which helps reduce occasional
  glitches at low latency settings. Restored automatically when playback stops.

## [1.5.2] - 2026-10-17

**中文**

### 修复

- 修复长时间播放时偶发的声音卡顿（爆音）。
  原因是连接建立期间采集已经开始缓冲，导致本地缓冲长期接近满仓，
  只剩很少余量；任何一次网络抖动都会把它顶出去。

**English**

### Fixed

- Fixed occasional audio glitches during long playback. Audio captured while
  the connection was still being established filled the local buffer, leaving
  almost no headroom for network jitter.

## [1.5.1] - 2026-10-17

**中文**

### 修复

- 修复开机自动启动时弹出一个黑色命令行窗口的问题。
  那个窗口关不掉也等不到，关掉它程序就会退出 —— 现在自启动完全静默。

**English**

### Fixed

- Fixed a stray console window appearing when the app starts at login.
  Closing that window would also close the app; startup is now silent.

## [1.5.0] - 2026-10-16

**中文**

### 修改

- 点击「关于」等菜单项时不再播放系统提示音。
  信息类和提问类的弹窗去掉了图标（Windows 下图标与提示音是绑定的），
  只有真正需要提醒的「连接诊断」警告仍保留提示音。

**English**

### Changed

- Menu dialogs no longer play the system notification sound, except the
  connection-check warning where it is still useful.

## [1.4.9] - 2026-10-16

**中文**

### 修复

- 修复点击托盘菜单里的「关于」「连接诊断」等项时，声音会中断的问题。
  滑块窗口（延迟 / 音量 / 音质 / 待机时长）同样受影响，一并修好。

- **v1.4.8 没有真正修好这个问题** —— 那个版本的改动没能生效，请升级到本版。

**English**

### Fixed

- Fixed audio dropping out when opening About, Connection check, or any
  slider window from the tray menu.

- **v1.4.8 did not actually fix this** - please update to this version.

## [1.4.8] - 2026-10-16

**中文**

### 修复

- 修复点击托盘菜单里的「关于」「连接诊断」等项时，声音会短暂中断的问题。
  滑块窗口（延迟 / 音量 / 音质 / 待机时长）同样受影响，一并修好。

**English**

### Fixed

- Fixed audio briefly dropping out when opening About, Connection check,
  or any slider window from the tray menu.

## [1.4.6] - 2026-10-16

**中文**

### 修复

- 修复切换成英文后，托盘菜单里仍有部分文字保持中文的问题（顶部状态行、「音质」、
  「无声音时待机」、设备列表里的「(未命名设备)」等）。

**English**

### Fixed

- Fixed parts of the tray menu staying in Chinese after switching to English
  (the status line at the top, Quality, Standby when silent, and "(unnamed device)"
  in the device list).

## [1.4.5] - 2026-10-16

**中文**

### 修改

- 系统通知改用本程序自己的图标，不再显示成灰色的默认图标。
- 「更多设置」里「语言」移到「日志」上方。

**English**

### Changed

- System notifications now use the app's own icon instead of a grey default.
- Moved Language above Logs in More settings.

## [1.4.4] - 2026-10-16

**中文**

### 修改

- 「关于」弹窗、连接诊断弹窗、授权提示等界面文案接入多语言。
- 托盘菜单和滑块窗口里的档位名（延迟 / 音质 / 待机时长）跟随界面语言。
- 「关于」弹窗标题用英文。

**English**

### Changed

- Translated the About dialog, the connection-check dialogs, and the
  admin-prompt text.
- Preset names in the tray menu and slider windows (latency / quality /
  standby) now follow the interface language.
- The About dialog title is now in English.

## [1.4.3] - 2026-10-16

### Added

- 界面支持中文与英文。首次运行时可选择，之后可在「更多设置 › 语言」里随时切换。
- README 提供中英双语版本，顶部可互相切换。

### Changed

- 「高级设置」改名为「更多设置」。
- 「设备」菜单里直接列出扫到的设备，不再需要多点一层。

## [1.4.2] - 2026-10-16

### Changed

- 「高级设置」里的「登录时打开」改回「开机自启」。

## [1.4.1] - 2026-10-16

### Changed

- 「连接诊断」「无声音时待机」「静音本机输出」「登录时打开」「日志」收进「高级设置」。
- 「关于」不再直接跳转网页：点开先看版本信息，有新版本时才提示，并可直接前往下载。
- 菜单用词统一为系统设置的习惯说法（原来「修复连接问题（需要管理员权限）」
  「连接时静音电脑扬声器」这类句子过长）。

## [1.4.0] - 2026-10-16

### Changed

- 托盘菜单里的「选择设备」「隐藏设备」「采集设备」合并为一项「设备」，
  一级菜单更短。三者在程序里都是设一次就不用再动的低频操作，
  收进二级不影响日常使用。

## [1.3.3] - 2026-10-16

### Fixed

- 移除 1.3.2 中遗留的一行调试输出。

## [1.3.2] - 2026-10-16

### Fixed

- 修复虚拟声卡模式下指定了采集设备时听不到声音的问题。该模式下声音已被送到虚拟声卡，
  此时应始终采集它，而不是用户指定的那个端点。
- 修复程序被强制结束时电脑扬声器可能停留在静音状态的问题。
- 命令行模式不再绕过单实例限制，避免两个实例争抢同一台设备。

### Changed

- 说明文档跟进当前功能与延迟档位范围。

## [1.3.1] - 2026-10-16

### Fixed

- 修复覆盖安装后网络放行规则没有配置成功的问题。此前升级时会清除原有规则却加不回新的，
  导致连不上、并且每次打开都提示未被防火墙放行。

## [1.3.0] - 2026-10-16

### Added

- 连接时自动静音电脑自己的扬声器，断开时恢复。可在托盘菜单里关闭。
  此前电脑接了音箱的话，会和 AirPlay 设备同时出声。
  （若声音已被送到虚拟声卡、电脑本来就听不到，则不做这个动作。）
- 托盘菜单新增「隐藏设备」：可以把不想看到的设备从「选择设备」列表里去掉，
  随时从同一处放回来。
- 托盘菜单新增「采集设备」：可以指定从哪一个输出端点采声音，
  默认跟随系统默认输出设备。

### Changed

- 延迟档位改为 150ms（低延迟）/ 200ms（较平衡）/ 250ms（推荐）/ 300ms（更稳）。
## [1.2.61] - 2026-10-16

### Changed

- 升级后延迟若低于 250ms 会自动调整为 250ms，无需手动设置。
  低于该值时接收端缓冲区不足，会出现爆音（延迟越低越明显）。仍可在菜单里改回任意值。

## [1.2.60] - 2026-10-16

### Fixed

- 修复机器上存在多份程序时，只有其中一份能用的问题：网络放行设置此前会被拥有它的那一份独占，
  另一份每次打开都提示未被放行、并且连不上，反复点修复也没用。现在各份都能正常工作。
- 清理仍然只针对指向已删除文件的失效设置，不会影响正在使用的那些。

## [1.2.59] - 2026-10-16

### Changed

- 延迟档位调整：150ms / 200ms / 250ms（推荐）/ 300ms，默认 250ms，可调范围 120–400ms。
  低于 250ms 时接收端缓冲区不足以吸收抖动，会出现爆音（延迟越低越明显）。

### Fixed

- 修复延迟偏低时的爆音：此前默认 150ms 且最低可到 120ms，均在可靠范围之外。

## [1.2.58] - 2026-10-16

### Fixed

- 修复升级时不再沿用原先的安装位置、而是强制装到默认目录的问题。
  此前如果程序装在别的位置，升级后会出现两份并存，导致网络放行设置互相冲突，
  表现为连不上、并且每次打开都提示未被防火墙放行。

## [1.2.57] - 2026-10-16

### Fixed

- 修复安装后网络放行规则没有自动配置的问题。此前如果程序正在运行，安装的最后一步
  会被当成重复启动而退出，导致装完仍然连不上、并且每次打开都提示未被防火墙放行。

## [1.2.56] - 2026-10-16

### Fixed

- 修复旧版本正在运行时的升级：此前可能因旧实例未被结束而跳过覆盖程序文件，
  表现为「装了新版，打开还是旧版」。

## [1.2.55] - 2026-10-16

### Fixed

- 修复安装时可能装到上次的目录、而原位置的文件没被更新的问题（已装了旧版本的机器会表现为
  「升级后还是老样子」）。现在始终安装到默认位置。

## [1.2.54] - 2026-10-16

### Fixed

- 修复安装程序在旧版本仍在运行时会卡住的问题。此前会停在「请关闭 AirPlay Bridge」提示上，
  点了确定也过不去，导致装到一半中断、网络放行规则没配上，装完连不上并且每次启动都提示。
- 放行设置被另一个副本占用时，提示里直接给出那个文件的位置，而不是只说「建议只保留一份」。

### Removed

- 停止发布免安装版，只提供安装版。
  系统集成（网络放行、开机自启）按 exe 完整路径记录，两份副本会互相顶掉对方的设置，
  谁点一次修复就把对方顶掉，两份轮流提示、永远修不好。

## [1.2.53] - 2026-10-16

### Added

- 日志新增「采集电平」：每 5 秒记录一次采集到的最大峰值，用于定位杂音、爆音与误判静音。

## [1.2.52] - 2026-10-16

### Fixed

- 修复音频丢帧记录在未连接时误报的问题（会刷出「每 5 秒丢 4 秒音频」这种无意义警报）。
- 安装版和免安装版同时存在时，会说明放行设置被另一份占用，而不是只说「未被放行」。

## [1.2.51] - 2026-10-16

### Added

- 日志会记录音频链路的关键指标（采样率、丢帧、削波），便于定位杂音与爆音。

## [1.2.50] - 2026-10-16

### Changed

- 安装版在安装过程中就把网络放行配好，装完即可使用，不需要再手动点一次。

## [1.2.49] - 2026-10-16

### Added

- 提供安装版。装到固定位置后，以后升级不需要重新配置网络放行和开机自启。
- 卸载时会自动清理留在系统里的网络放行设置、启动项。

### Note

- 免安装版仍然提供，适合放 U 盘或临时使用。

## [1.2.48] - 2026-10-16

### Changed

- 未安装虚拟声卡时的提示改用更准确的说法：系统声音仍会送往电脑的输出设备。

## [1.2.47] - 2026-10-16

### Changed

- 静音自动待机的滑块改为 0 – 20 秒无级调节，拖到 0 即关闭该功能。
- 默认时长由 10 秒改为 5 秒。

## [1.2.46] - 2026-10-16

### Fixed

- 修复安静但有声音的内容（例如球赛解说停顿时的现场环境声）被误判成静音、
  导致频繁断连的问题。

## [1.2.45] - 2026-10-16

### Changed

- 静音自动待机的档位改为 5 / 10 / 15 / 20 秒，滑块范围同步放宽到 5–20 秒。
  默认时长由 5 秒改为 10 秒（此前保存过设置的不受影响）。

## [1.2.44] - 2026-10-16

### Changed

- 启动后立即连接上次用过的音箱。此前首次运行时需要等待数秒才开始连接。

### Added

- 首次运行时会询问是否开机自动启动。
- 首次运行没找到音箱时会给出提示，说明需要检查什么。

## [1.2.43] - 2026-10-16

### Fixed

- 修复托盘图标与菜单状态不一致的问题。此前连接中断后图标仍显示为已连接，
  菜单里却是未连接。

## [1.2.42] - 2026-10-16

### Fixed

- 修复更换程序存放位置或重命名后「开机自启」失效的问题。此前开机不会
  自动启动，菜单里却仍显示为已开启。

### Added

- 启动时会记录「开机自启」的实际状态，便于确认是否已启用。

## [1.2.41] - 2026-10-15

### Fixed

- 统一了两处网络放行检查的口径，避免同一个问题出现两种结论。

## [1.2.40] - 2026-10-15

### Fixed

- 修复放行检查可能误判、以及重连间隔不再逐次拉长的问题。

### Changed

- 去掉一轮多余的设备扫描，重连时不再做无用功。

## [1.2.39] - 2026-10-15

### Fixed

- 启动检查不再把正在使用的网络放行设置误判为遗留配置；重复的设置会被
  合并为一条，不再越积越多。

## [1.2.38] - 2026-10-15

### Added

- 启动时检测遗留的旧网络放行设置，并可就地清理，避免系统里堆积无效配置。

## [1.2.37] - 2026-10-15

### Added

- 启动时会检查网络放行是否有效，失效则直接提示并可就地修复。
  更换文件位置或重命名后不会再用到旧的放行设置。

## [1.2.36] - 2026-10-15

### Fixed

- 修复下载新版本后「修复连接问题」提示已配置、程序却仍连不上的问题。
  更换文件位置或重命名后，旧设置不再被误认为仍然有效。

## [1.2.35] - 2026-10-15

### Added

- 托盘菜单新增「检查更新」，可直接查看是否有新版本并前往下载。

### Fixed

- 修正「关于」里显示的版本号。

## [1.2.34] - 2026-10-15

### Added

- 启动时检查是否已有一个 AirPlay Bridge 在运行。已有的话会提示并退出，
  避免两个实例互相打断声音。

## [1.2.33] - 2026-10-15

### Added

- 托盘菜单新增「关于」，显示当前版本号，点击可打开项目主页。

## [1.2.32] - 2026-10-15

### Fixed

- 修复「修复连接问题」在某些系统上执行失败的问题。

## [1.2.31] - 2026-10-15

### Fixed

- 「修复连接问题」现在始终显示在托盘菜单里。此前它只在检测到缺少
  放行规则时才出现，导致刚装好或已经配好的用户完全看不到这个入口。

## [1.2.30] - 2026-10-15

### Added

- 托盘菜单新增「修复连接问题」。一直显示「连接中」时，点它一次即可自动
  完成网络放行，不需要手动操作。

## [1.2.29] - 2026-10-15

### Added

- 一直显示「连接中」时，日志会说明可能的原因和排查方法。
  此前只有一句「连接失败」，无法判断问题出在哪。

## [1.2.28] - 2026-10-15

### Fixed

- 连接失败后的重试间隔不再始终停留在几秒。此前会每 10 秒不停重试，
  接收端持续拒绝而无法恢复；现在重试间隔会逐步拉长，最长 60 秒。

## [1.2.27] - 2026-10-15

### Fixed

- 修复一次连接失败后程序失去响应的问题：托盘菜单不再反应、
  状态一直停在「连接中」，只能重启程序。

## [1.2.26] - 2026-10-15

### Fixed

- 重连时不再每次都重新扫描局域网设备，改为先直连上次用过的设备。
  连接不顺畅时，托盘菜单不再反复卡顿。

## [1.2.25] - 2026-10-15

### Added

- 卡顿会被记录进日志：界面一旦超过约 1 秒没响应，日志会写明卡了多久、
  当时正在做什么。

### Changed

- 反复重连时不再每次都重新扫描局域网设备（30 秒内复用上次结果）。

## [1.2.24] - 2026-10-15

### Changed

- 主循环处理网络事件的最长等待由 50ms 降到 10ms。连接不顺畅时，
  托盘菜单的响应会更快。

## [1.2.23] - 2026-10-15

### Fixed

- 连接失败过一次之后，之后每次连接都会在几秒后被程序自己拆掉，
  表现为一直显示「连接中」而永远连不上 —— 重启设备也没有用。

## [1.2.22] - 2026-10-15

### Fixed

- 点开托盘菜单之后菜单可能失去响应，此后点托盘图标也没有任何反应。
  菜单打开期间用于维持音频的心跳会阻塞菜单自身的消息处理，
  在连接不顺畅时尤其容易触发。

## [1.2.21] - 2026-10-15

### Changed

- 「静音自动待机」的时限改为 5 / 6 / 8 / 10 秒四个档位，另加一个 5–10 秒的
  无极滑块（每 0.5 秒一档）。默认 5 秒。
- 原来的 0 / 1 / 3 秒档位取消 —— 太短，换歌和安静段落会被误判成静音。

## [1.2.20] - 2026-10-15

### Changed

- 「静音自动待机」改为二级菜单：可选 0 / 1 / 3 / 5 / 8 / 10 秒，或关闭。
  默认 1 秒，改动立即生效。

## [1.2.19] - 2026-10-15

### Fixed

- 待机状态下手动点「连接」之后，如果连接真的断掉，不会再自动重连。
  现在只要开始建立连接就会正确退出待机状态。

## [1.2.18] - 2026-10-15

### Fixed

- 静音待机进入后约 5 秒又被自动连回来，设备屏幕反复亮灭。
  现在待机期间不会再被掉线重连逻辑接管。

## [1.2.17] - 2026-10-15

### Added

- 静音自动待机：连续 20 秒没有声音，就断开与设备的连接（设备屏幕随之熄灭）；
  一旦检测到声音立刻自动重连。托盘菜单里可以关闭。

## [1.2.16] - 2026-10-15

### Changed

- 延迟的调节滑块范围收窄到 120–150ms，与预设档位一致。
- 延迟档位的评价文案改为「最低延迟 / 推荐 / 更稳」。

## [1.2.15] - 2026-10-15

### Changed

- 延迟的调节滑块范围由 60–1000ms 收窄到 60–300ms。120–150ms 这一段现在占去
  滑块大部分行程，微调精度明显提高。
- 延迟的档位评价分段随之调整。

### Fixed

- 配置里存着超出滑块范围的值时，打开调节窗口会被重置为最小值。

## [1.2.14] - 2026-10-15

### Changed

- 延迟的预设档位由 60–600ms 收窄到 120–150ms（120 / 130 / 140 / 150）。
  需要更大值的仍可用「自定义」滑块。

## [1.2.13] - 2026-10-15

### Fixed

- 打开托盘菜单时声音会中断。菜单弹出期间主循环被系统的菜单循环占用，
  音频数据送不出去，接收端因此断流。现在菜单打开期间继续正常推流。

## [1.2.12] - 2026-10-15

### Fixed

- 程序的设备标识此前是写死的，同一账号下的多台电脑在接收端看来是同一台设备，
  同时使用时互相顶掉。现在每台机器首次运行生成各自的标识。

## [1.2.11] - 2026-10-15

### Fixed

- 调节窗口的「取消」和「应用」两个按钮宽度计算有误，彼此重叠。
  现在等宽排列，中间留出正常间距。

## [1.2.10] - 2026-10-15

### Changed

- 延迟、音量、音质的调节窗口重做：跟随系统深浅色，当前档位评价用主色标出，
  滑块配色与托盘图标一致。

## [1.2.9] - 2026-10-15

### Changed

- 托盘的小尺寸图标改用简化处理。原图的网布纹理在缩到 16–24px 时会平均成
  一团发闷的橘色，现在这部分单独提纯，颜色更干净、边缘更利落。
  32px 及以上仍保留原始细节。

## [1.2.8] - 2026-10-15

### Added

- 托盘菜单跟随系统深浅色主题。深色模式下菜单为深色，浅色模式下保持原样。

## [1.2.7] - 2026-10-15

### Fixed

- 延迟、音量、音质的子菜单里，用滑块调出来的值不会选中任何一项。
  现在该值不属于任何预设档位时，勾打在「自定义」上。

## [1.2.6] - 2026-10-15

### Fixed

- 系统显示缩放不是 100% 时，托盘菜单、托盘图标和滑块窗口会被系统整体放大，
  显示发虚；菜单在屏幕底部还会被截掉一行。现在按原生分辨率渲染。

## [1.2.5] - 2026-10-15

### Fixed

- 图标四周的透明留白此前未生效，1.2.4 中显示的仍为旧图标。

## [1.2.4] - 2026-10-15

### Changed

- 图标四周的透明留白由约 11% 减少到 1.5%，图标在同尺寸下显示得更大。

## [1.2.3] - 2026-10-15

### Changed

- 延迟、音量、音质、选择设备恢复为从主菜单展开的子菜单。
  此前改为点击后弹出独立菜单，主菜单会消失，展开时位置也发生跳变。

## [1.2.2] - 2026-10-15

### Changed

- 延迟、音量、音质、选择设备四项改为点击后才展开，不再在鼠标经过时自动展开。
- 四项右侧的展开提示改为与其余菜单文字对齐的样式。

## [1.2.1] - 2026-10-14

### Changed

- 延迟、音量、音质改为子菜单，与「选择设备」一致。
  菜单项右侧会显示展开箭头，此前这三项没有该提示，
  用户不易看出可以展开。

## [1.2.0] - 2026-10-14

### Changed

- 设备列表在设备名称后显示局域网 IP，便于区分同型号设备。

## [1.1.9] - 2026-10-14

### Changed

- 设备列表只显示设备名称，不再附带型号标识与协议版本。
  这些信息改为写入日志，需要时可在日志中查看。

## [1.1.8] - 2026-10-14

### Changed

- 设备选择改为子菜单：「选择设备」展开后直接列出已发现的设备，
  当前连接的设备带勾选标记。「重新扫描」移到该子菜单内。
- 移除主菜单中重复的「重新扫描设备」项。

### Fixed

- 此前选择设备时弹出的是独立菜单窗口，而非从主菜单展开的子菜单。

## [1.1.7] - 2026-10-14

### Fixed

- 「重新扫描设备」此前仅在找到两台以上设备时显示设备列表；
  只找到一台会直接连接，一台都没找到则没有任何提示，
  菜单直接关闭。
- 现在无论找到几台都会显示设备列表，需要选择后才发起连接。
  未找到设备时列表中给出排查提示。

### Added

- 扫描期间显示提示，此前扫描耗时一两秒且界面无任何反馈。

## [1.1.6] - 2026-10-14

### Changed

- 左键单击托盘图标同样打开菜单（此前仅响应右键）。
  托盘图标是程序唯一的入口，只认右键会让习惯左键的用户
  以为程序没有响应。

## [1.1.5] - 2026-10-14

### Changed

- 音质最低档由「关闭」改名为「快速」。该档并非不做转换，
  而是采用更省 CPU 的算法；原名称容易被误解为关闭音频输出。
- 档位划分按实测数据调整：快速 / 标准 / 高 / 极高 / 至臻

## [1.1.4] - 2026-10-14

### Changed

- 音质改用档位名显示（关闭 / 标准 / 高 / 极高 / 至臻），不再显示百分比
- 音质菜单的预设档位与滑块使用同一套名称

## [1.1.3] - 2026-10-14

### Changed

- 音质从 4 个固定档改为 0-100 连续可调。0 为关闭（交给系统默认转换），
  1-100 映射到内部滤波器的抽头数 4-32。
- 音质在主菜单中显示为百分比。
- 音质菜单项位置调整到延迟、音量之后。

### Fixed

- 旧版配置中的音质档位（0-3）会被自动换算为新的百分比，
  不会把原来的最高档误判为 3%。

## [1.1.2] - 2026-10-14

### Changed

- 音质菜单项位置移到延迟、音量之后。
- 音质改为子菜单，与延迟、音量保持一致的交互。

## [1.1.1] - 2026-10-14

### Added

- 日志超过 30 天未写入时自动清除。
- 启动时在日志中记录已加载的设置（音量、延迟、音质、设备），
  便于确认设置是否被正确记住。

## [1.1.0] - 2026-10-14

### Added

- 音质档位。可选择重采样质量，改善高频段的清晰度。

### Fixed

- 高频段（15 kHz 以上）的音频失真。原先使用的线性插值在 15 kHz 处
  信噪比仅 14 dB，现改为窗函数插值，提升至约 39 dB。

## [1.0.1] - 2026-10-14

### Changed

- 发布包改为单个可执行文件，不再需要解压。
- 音频设备诊断与默认设备切换功能并入主程序，不再需要单独的工具。

### Fixed

- 命令行参数此前被忽略，现已可用（`--list`、`--last`、`--install`、
  `--uninstall`、`--audio-devices`、`--set-default`）。

## [1.0.0] - 2026-10-14

首个版本。

### Added

- 将 Windows 系统声音串流到 AirPlay 2 设备（HomePod、Apple TV）。
- 自动发现局域网内的 AirPlay 设备。
- 接管模式：配合虚拟声卡，电脑自身不发声，声音只从 HomePod 输出。
  连接时自动切换默认播放设备，退出时切回；程序异常退出后下次启动自动恢复。
- 断线自动重连，按设备标识记忆目标设备。
- 延迟可调（60-1000 ms）。
- 音量可调。
- 开机自启。
- 单文件运行，配置存放于 `%LOCALAPPDATA%\AirPlayBridge\`。
