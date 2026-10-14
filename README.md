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

## 编译

前置：

- CMake 3.16+
- Visual Studio 2022 / 2026（C++20）
- Windows SDK（要有 WASAPI 头文件）

```bash
# 1. 克隆（含上游引擎）
git clone --recursive https://github.com/RiverFlowsInUUU/airplay-bridge.git
cd airplay-bridge

# 2. 给上游引擎打补丁
cd third_party/airplay2-sender-cpp
git apply ../../patches/upstream-airplay2-sender-cpp.patch
cd ../..

# 3. 编译
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

产物：`build/Release/airplay_bridge.exe`

> 首次配置会通过 CMake FetchContent 下载 Mbed TLS（约 190MB）。
> 如果网络不稳，可以先手动 clone 到 `build/_deps/mbedtls-src`，
> 再带 `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` 配置。

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
```

### 辅助工具

```
listaudio.exe          列出所有音频设备及其缓冲参数（诊断用）
setdefault.exe         切换系统默认播放设备
                         不带参数运行会列出所有设备
setdefault.exe "CABLE Input"     切到虚拟声卡
setdefault.exe "Realtek"         切回物理声卡
```

排查"串流没声音"时先跑 `listaudio.exe` —— 确认默认设备是不是你以为的那个。

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

## 已知限制

- 接管模式依赖虚拟声卡；没装的话只能用监听复制模式
- 延迟改档需要重连一次才生效（写在握手参数里，无法中途改）
- 事件通道断线不自动重连（有上层 6 秒重连兜底）
- 只在 Windows 上测过（WASAPI 是 Windows 独有的）

## 改这个项目前先看这里

这一节是给**下一个动这个仓库的人（或 AI）**看的。以下每一条都是实际踩过的坑，
不是理论提醒。按顺序看完能省掉几个小时。

### 1. 改了延迟值，就要同步改测试

默认延迟在 `patches/upstream-airplay2-sender-cpp.patch` 里从上游的 **1.5s（66150 帧）**
调成了 **150ms（6615 帧）**。这个值会同时影响三处 RTP 握手参数：

| 参数 | 上游原值 | 本项目的值（150ms 时） |
|---|---|---|
| RTP 时间戳基准 / `rtptime` | 66150 | 6615 |
| `latencyMax`（允许接收端缓冲上限） | 88200 | `latency_ × 2` = 13230 |
| `latencyMin`（下限） | 11025 | `latency_ ÷ 3` = 2205 |

上游测试 `test/core_tests.cpp` 里这些值是**写死的**。我们已经在 patch 里把它们
改成跟随 `sender->latencyMs()` 动态计算，所以**你再调延迟不用改测试**。
但如果有人把这套动态计算改回去，`ctest` 会立刻报 5 项失败（`226/231`），
症状是 `rtptime` / `latency window` 相关的断言全红。

**改延迟的正确姿势**：只动 `raop_sender.h` 里的 `latency_` 默认值，
其余（RTP 基准、latency 窗口、测试期望）都是派生出来的。

### 2. 别让 `core.autocrlf` 把补丁改坏

`patches/*.patch` 是 unified diff，上下文行必须跟上游源文件**逐字节**吻合。
Windows 上 `core.autocrlf=true`（默认）会在 checkout 时把 patch 转成 CRLF →
`git apply` 直接失败（`patch does not apply`）→ **任何人都编译不了**。

仓库根目录的 `.gitattributes` 已经声明 `*.patch -text` 锁死行尾。
**不要删这个文件**，也不要往 patch 里混入 CRLF。

判断是否中招：

```bash
file patches/upstream-airplay2-sender-cpp.patch
# 正常: ASCII text / UTF-8 text
# 中招: ... with CRLF line terminators
```

### 3. Mbed TLS 为什么是 3.6.0

版本由**上游写死**（`FetchContent` 里 pin 了一个 commit，注释标明 v3.6.0），
不是我们选的。实测 3.6.2 **也能编译并通过全部测试**，但**没必要升**：

- 3.6.1 / 3.6.2 修的都是 TLS 证书校验、`pk_write_key_*`、`ecdsa_der_to_raw`
  这类接口的漏洞，本项目**一个都没用到**（只用了 HKDF / ChaCha20-Poly1305 /
  HMAC / 大数运算 / DRBG）
- 3.6.2 的 CMakeLists 会**强制要求 `framework` 子模块**存在，少一个就直接
  配置失败 —— 对国内网络是额外的负担
- 擅自改上游的 pin 会跟上游更新脱节

### 4. 首次构建会拉 Mbed TLS（约 190MB）

由上游的 `FetchContent` 自动下载。网络不好时会卡很久（实测直连 GitHub
只有约 50KB/s，且会中途断开重来）。

**绕过的办法**：手动准备一份源码放进 `build/_deps/mbedtls-src/`，配置时加
`-DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<绝对路径>`，这样它就不会联网拉取。

注意两点：

- 只复制 `mbedtls-src` 不够，**`framework/` 子目录也要在**，否则 CMake 配置报
  `framework/CMakeLists.txt not found`
- 复制大目录时 `cp -r` 在 Windows 上可能因为文件锁**静默复制不全**
  （曾出现源 169 个文件、目标 0 个文件）。用 `tar` 管道更可靠：
  `(cd "$SRC" && tar cf - .) | (cd "$DST" && tar xf -)`

### 5. 生成器名字要对上本机

README 的编译命令写的是 `-G "Visual Studio 17 2022"`，但**以本机实际安装为准**：

```bash
cmake --help | grep -i "visual studio"
```

装的是 VS 18 就用 `-G "Visual Studio 18 2026"`，否则会报
`could not find any instance of Visual Studio`。

### 6. 改完上游代码后，patch 要重新生成

补丁是 `third_party/airplay2-sender-cpp/` 相对上游 `fc91350` 的 `git diff`。
改了上游文件后：

```bash
cd third_party/airplay2-sender-cpp
git diff > ../../patches/upstream-airplay2-sender-cpp.patch
```

**两个容易犯的错**：

- 忘了 `git add` / `commit` 就直接去 `clone` 验证 —— clone 只认**已提交**的内容，
  没提交的改动不会出现在新 clone 里，验证会白做
- 新 patch 必须包含**所有**改过的文件。当前含 4 个：
  `CMakeLists.txt`、`src/raop_sender.cpp`、`src/raop_sender.h`、`test/core_tests.cpp`

### 7. 验收标准

改完照这个走一遍，全绿才算完成：

```bash
# 干净克隆 + 子模块
git clone --recursive <repo> && cd <repo>

# 打补丁
cd third_party/airplay2-sender-cpp
git apply ../../patches/upstream-airplay2-sender-cpp.patch
cd ../..

# 编译
cmake -S . -B build -G "<你本机的生成器>" -A x64
cmake --build build --config Release

# 跑上游测试（231 项，必须 100% 通过）
cd third_party/airplay2-sender-cpp
cmake -S . -B tbuild -G "<你本机的生成器>" -A x64 -DAIRPLAY_BUILD_TESTS=ON
cmake --build tbuild --config Release
ctest --test-dir tbuild -C Release
```

## 项目结构

```
airplay_bridge.cpp      主程序（托盘 + 命令行）
wasapi_capture.cpp/h    WASAPI 环回采集（支持多设备混音）
discovery.cpp/h         设备发现（扫网段 + 单播 mDNS）
config_store.cpp/h      JSON 配置读写
tray.cpp/h              Win32 托盘图标与菜单
latency_dialog.cpp/h    数值调节滑块窗口（延迟/音量共用）
sysvolume.cpp/h         系统音量 + 默认音频设备切换
bridge.rc               图标资源（编译进 exe）
tray_icons/             图标源文件
tools/                  辅助工具（诊断、设备切换）
patches/                上游引擎的补丁
third_party/            上游引擎（git submodule）
```

## 依赖

- [airplay2-sender-cpp](https://github.com/akustikrausch/airplay2-sender-cpp) — AirPlay 2 协议实现（Apache-2.0）
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) — 加密（由上游自动拉取）

## 许可证

Apache-2.0
