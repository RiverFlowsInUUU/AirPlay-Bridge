# 开发

面向 AI agent：环境、构建、测试、调试、发布。逐条约束在 [AGENTS.md](../AGENTS.md)。

## 环境

| 项 | 要求 |
|---|---|
| 系统 | Windows 10 / 11，x64 |
| 编译器 | Visual Studio 2022 或更新（需 C++ 桌面工作负载） |
| 构建 | CMake ≥ 3.20 |
| 图标 | Python 3 + Pillow（仅改图标时需要） |
| 发布 | `gh` CLI（已登录） |

生成器名字以本机实际安装为准，不要照抄：

```bash
cmake --help | grep -i "visual studio"
```

## 构建

```bash
git clone --recursive https://github.com/RiverFlowsInUUU/AirPlay-Bridge.git
cd AirPlay-Bridge

# 打补丁（必须干净应用，见 AGENTS.md C2）
cd third_party/airplay2-sender-cpp
git apply ../../patches/upstream-airplay2-sender-cpp.patch
cd ../..

cmake -S . -B build -G "<本机生成器>" -A x64
cmake --build build --config Release
```

产物：`build/Release/airplay_bridge.exe` —— 单文件，静态链接 CRT，无需运行库。

首次配置会拉取 Mbed TLS（约 190MB）。网络受限时改用本地已有源目录：

```bash
cmake -S . -B build -G "<生成器>" -A x64 \
      -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<mbedtls 源目录>
```

只编主程序（跳过测试目标，快很多）：

```bash
cmake --build build --config Release --target airplay_bridge
```

## 测试

上游自带 231 项测试。

```bash
cd third_party/airplay2-sender-cpp
cmake -S . -B tbuild -G "<生成器>" -A x64 -DAIRPLAY_BUILD_TESTS=ON
cmake --build tbuild --config Release
ctest --test-dir tbuild -C Release
```

期望 231/231 通过。改动 `latency_` 相关代码时必跑，见 [AGENTS.md C1](../AGENTS.md)。

重采样自测（输出信噪比对比表）：

```bash
cmake --build build --config Release --target resampler_test
./build/Release/resampler_test.exe
```

## 验收

改完照这个走，全绿才算完成。

```bash
# 1. 干净克隆到临时目录，确认补丁在「已提交」状态下仍能应用
git clone --recursive <repo> /tmp/verify && cd /tmp/verify
cd third_party/airplay2-sender-cpp
git apply ../../patches/upstream-airplay2-sender-cpp.patch
cd ../..

# 2. 编译，期望无 error
cmake -S . -B build -G "<生成器>" -A x64
cmake --build build --config Release
```

运行时验收（需要设备在线）：

```bash
# 串流稳定性：跑 120 秒，日志不应出现「连接断开」
%LOCALAPPDATA%\AirPlayBridge\bridge.log

# 接管模式：默认设备应自动切到 CABLE Input
./build/Release/airplay_bridge.exe --audio-devices
```

要验「掉线后能不能恢复」，不用真拔网线 —— 先让它连上，再切断到接收端的流量：

```bash
netsh advfirewall firewall add rule name="APB-BLOCK-TEST" dir=out
      action=block remoteip=<接收端 IP>
# 观察 60 秒，然后**务必删掉**，否则接收端一直不可达：
netsh advfirewall firewall delete rule name="APB-BLOCK-TEST"
```

日志里看两件事：**「主线程无响应」有没有「恢复」**（没有恢复 = 死锁），
以及**重试间隔有没有逐次拉长**（恒定不变 = 退避被清零了）。

## 调试

程序是 WIN32 子系统，没有控制台。`AttachConsole(ATTACH_PARENT_PROCESS)`
在重定向场景下会失败 —— 从部分终端调用时 stdout 看不到输出。

### 命令行参数

全部参数只供开发与诊断使用，**不是用户路径** —— 用户走托盘菜单。

| 参数 | 说明 |
|---|---|
| （无） | 托盘模式 |
| `--console` | 命令行模式 |
| `--list` | 列出发现的设备 |
| `<ip>` | 直连指定 IP |
| `--last` | 重连上次的设备 |
| `--install` | 注册开机自启 |
| `--uninstall` | 取消开机自启 |
| `--audio-devices` | 列出音频设备及缓冲参数 |
| `--set-default <名>` | 切换默认播放设备 |
| `--fix-firewall` | 加入站 UDP 放行规则（托盘菜单提权后拉起自己时用） |
| `--clean-firewall-rules` | 删掉指向已失效文件的放行规则（同上） |

```bash
airplay_bridge --audio-devices             列出音频设备及缓冲参数
airplay_bridge --set-default "CABLE Input" 切换默认播放设备
airplay_bridge --list                      列出发现的设备
airplay_bridge --console                   命令行模式（有控制台）
```

`--audio-devices` 的输出会同时写一份到
`%LOCALAPPDATA%\AirPlayBridge\audio-devices.log`（见 [AGENTS.md C8](../AGENTS.md)）。

日志：`%LOCALAPPDATA%\AirPlayBridge\bridge.log`。
启动时打一行「已加载设置: ...」，可用于确认配置是否按预期读入。
清理策略见 [AGENTS.md C6b](../AGENTS.md)。

## 图标

由一张源图生成全部尺寸与灰化版本：

```bash
python tools/gen_icons.py "path/to/source.png"
```

输出直接写入 `src/icons/`（`connected.ico` / `idle.ico` / `app.ico` / 两张预览图）。
**不要再建第二份图标目录** —— 曾经因为生成目录与构建读取目录不一致，
改过的图标始终没进可执行文件。

## 发布

```bash
# 1. 编译
cmake --build build --config Release --target airplay_bridge

# 2. 更新 CHANGELOG.md，文风见 AGENTS.md「发布说明文风」
#    版本号遵循 Semantic Versioning，日期用 ISO 格式
#    版本号只有一处：CMakeLists.txt 的 project(... VERSION x.y.z)，
#    经 APP_VERSION 编译期注入，菜单「关于」自动跟上（源码里不再手写）

# 3. 提交并推送
git add -A && git commit && git push

# 4. 打 tag 推上去 —— CI 会自动编译并出两个产物（exe + 安装包）
git tag vX.Y.Z && git push origin vX.Y.Z
```

`.github/workflows/release.yml` 只在 tag 或手动触发时跑完整构建
（平时不烧分钟数，理由见 check-patch.yml 的注释）。
它会从 `CMakeLists.txt` 读版本号、用 Inno 打安装包、建 release。

不想等 CI 也可以本机打（装了 Inno Setup 6 之后）：

```bash
ISCC.exe /DAppVersion=X.Y.Z installer\airplay-bridge.iss
```

发版前确认 `CHANGELOG.md` 与 release notes 内容一致。

### 安装脚本

`installer/airplay-bridge.iss`。两个容易踩的点，注释里都写清了：

- 卸载前的系统集成清理走 `InitializeUninstall` 事件，**不要用 `[UninstallRun]`**
  —— 实测卸载阶段它的 `waituntilterminated` 不生效，130ms 后就把 exe 删了，
  清理程序来不及跑完
- 静默卸载时不能弹「要不要删配置」的询问框（没人回答，Inno 取默认值会误删）

- 安装时会自动把网络放行规则配好（`[Run]` 段调 `--fix-firewall --quiet`）——
  安装程序本来就有管理员权限，顺手做掉比让用户装完再点一次舒服。
  `--quiet` 让它不弹提示框；失败与否有日志，而且程序启动时的自检还有一道兜底
