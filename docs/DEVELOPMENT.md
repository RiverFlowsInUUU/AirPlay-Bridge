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


**改过 `third_party/` 里的文件之后，必须立刻重新生成补丁**（[RULES.md](RULES.md) C13）——
本地编译读的是工作区，CI 打包读的是 `patches/`，不同步就等于「你测的和用户装的不是同一份代码」。

```bash
cd third_party/airplay2-sender-cpp && git diff > ../../patches/upstream-airplay2-sender-cpp.patch
```

**验证不能只看「CI 绿了」** —— 在一份干净的 `git archive HEAD` 上 `git apply` 一遍，
再 grep 关键实现值，确认打出来的和你本地的一样。`check-patch.yml` 里有一份「标志性行」
清单做防陈旧检查，**新增关键实现时要同步更新那份清单**。
cmake -S . -B build -G "<本机生成器>" -A x64
cmake --build build --config Release
```

本机构建产物：`build/Release/airplay_bridge.exe`（单文件，静态链接 CRT）。
**对用户交付的是安装包**，由 CI 打包，见「发布」。

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

**第一步先确认被测二进制的版本号** —— 版本不对，后面所有「通过」都是假的。

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

运行 120 秒，读日志（**路径用资源管理器打开，不要粘进终端**）：

| 看什么 | 在哪 |
|---|---|
| 串流稳定性 | 日志 `bridge.log`（在 `%LOCALAPPDATA%` 下的 `AirPlayBridge` 目录）
  —— 不应出现「连接断开」 |
| 接管模式 | 跑 `--audio-devices`，默认设备应切到 CABLE Input |


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
| `--uninstall-cleanup` | 卸载前的系统集成清理（安装程序调，豁免单实例检测） |
| `--quiet` | 配合上面几个用：不弹提示框，只写日志 |
| `--volume <n>` | 直设音量百分比 |

`--fix-firewall` / `--clean-firewall-rules` / `--uninstall-cleanup` / `--audio-devices` /
`--console` **豁免单实例检测** —— 否则安装在程序正开着时会静默失败。

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

# 4. 打 tag 推上去 —— CI 自动编译并出安装包
git tag vX.Y.Z && git push origin vX.Y.Z
```

`.github/workflows/release.yml` 只在 tag 或手动触发时跑完整构建
（平时不烧分钟数，理由见 check-patch.yml 的注释）。
它会从 `CMakeLists.txt` 读版本号、用 Inno 打安装包、建 release。

**这条流水线自带补丁校验**（LF 行尾 / 能 apply / 覆盖 4 个文件 / 防陈旧标志行），
**在编译之前跑，失败即中止**。所以 tag 推上去不用再靠旁路的 `check-patch.yml`
替它把关 —— 两个 workflow 是相互独立的，一个挂了不拦另一个（2026-10-17 踩过）。

**发布正文是从 `CHANGELOG.md` 本版那一节自动生成的**，并且会把折行的续行接回去。
原因：CHANGELOG 按 ~80 列折行便于阅读，而 GitHub 对 **release 正文** 用的是
「单个换行 = 硬换行」的渲染规则（跟仓库里的 `.md` 文件不同）——
不接回去，页面上就会「没写到边就换行」。**不需要手工维护两份。**

**只改文档 / CI 的提交不要升版**（见 [PITFALLS.md](PITFALLS.md) ⑥「文档改动不升版」）：
打 tag 会产出一个和上一版没有实质区别的包。

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
