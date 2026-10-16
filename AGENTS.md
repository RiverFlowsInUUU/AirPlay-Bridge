# AGENTS.md

**本仓库唯一的 AI 入口。** 其他工具（Claude Code / Cursor / Copilot / Gemini…）
一律读这一份，**不设平台副本** —— 副本会漂移，而且规范只有一处才权威。

面向 AI agent 的协作契约：这是什么、怎么跑起来、怎么改、什么绝对不能做。
目标是**准确**和**无歧义**，不考虑人类可读性。人类说明见 [README.md](README.md)。

本项目由 agent 维护 —— 代码、文档、发布流程都是。

---

## 这是什么

Windows 托盘程序，把系统声音实时串流到 AirPlay 2 设备（HomePod mini）。
起因是 TuneBlade 在 HomePod OS 27 上失效。

**技术栈**：C++17 / MSVC 2022 / CMake ≥3.20 / Win32（WIN32 子系统，无控制台）。
协议引擎来自 [airplay2-sender-cpp](https://github.com/akustikrausch/airplay2-sender-cpp)
（git submodule）+ 本项目补丁。

**数据流**：WASAPI loopback 采集 → 重采样到 44100 → RAOP 引擎 → 接收端。
分层、模块表、不变量见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)；
为什么这么选见 [docs/DECISIONS.md](docs/DECISIONS.md)。

---

## 快速开始

```bash
git clone --recursive https://github.com/RiverFlowsInUUU/AirPlay-Bridge.git
cd AirPlay-Bridge

# 给上游打补丁（必须干净应用，见 C2）
cd third_party/airplay2-sender-cpp
git apply ../../patches/upstream-airplay2-sender-cpp.patch
cd ../..

# 生成器名以本机实际安装为准，不要照抄
cmake --help | grep -i "visual studio"

cmake -S . -B build -G "<本机生成器>" -A x64
cmake --build build --config Release --target airplay_bridge
```

产物 `build/Release/airplay_bridge.exe`。直接运行即进托盘模式。

**测试**：

```bash
# 上游 231 项（改动 latency_ 相关代码时必跑，见 C1）
cd third_party/airplay2-sender-cpp
cmake -S . -B tbuild -G "<生成器>" -A x64 -DAIRPLAY_BUILD_TESTS=ON
cmake --build tbuild --config Release
ctest --test-dir tbuild -C Release

# 重采样自测（输出信噪比对比表）
cmake --build build --config Release --target resampler_test
./build/Release/resampler_test.exe
```

**验收**（改完照 [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) 的「验收」全流程走）。
运行时验收需要设备在线；日志在 `%LOCALAPPDATA%\AirPlayBridge\bridge.log`。

---

## 工作流

### 改之前

1. **读 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)**，确认改的是哪一层
2. 涉及第三方库的**先读它的 README 和源码注释** —— 上游作者踩过的坑比推理值钱
   （血泪：找了一整天的根因，就写在上游源码第 490 行的注释里，我读了那个文件却没翻到）
3. 改「范围类参数」（延迟 / 音量 / 待机时长）前，**先列出所有涉及它的地方**：
   档位表、默认值、读取时的钳制、UI 范围，至少四处
4. 改「状态字段」前，`grep -n "<字段>.store"` 列出所有写入点，
   逐个确认配套动作（通知、清标志、更新 UI）都做了

### 改之后

**编译通过 ≠ 可以发版。** 至少跑一次：能不能连上、日志有没有异常输出。

```bash
# 冒烟：确认程序真的起来了、日志真的在写
%LOCALAPPDATA%\AirPlayBridge\bridge.log    # 看「已加载设置 / 已连接 / 看门狗」
```

- **验证前先确认被测二进制的版本号** —— 版本不对，后面所有「通过」都是假的
- **脚本改完代码立刻 grep 复核那一行** —— 改动可能被后续操作冲掉，而工具仍报「已替换 1 处」
- **改界面文案 → 全仓库 grep 旧文案**：弹窗正文、日志、滑块窗口标题、注释、
  README、docs。只改菜单一个字，用户会在五个地方看到旧名

### 提交

- **message 用中文**，格式 `前缀：一句话描述`
  （现有前缀：`修复` / `文档` / `纪律` / `清理` / `移除`）
- **补丁有改动时必须先 commit 再验证** —— 见 C2
- 提交前 `git status` 看一眼，**别把自己的临时产物一起提交**

### 发版

```bash
cmake --build build --config Release --target airplay_bridge
# 更新 CHANGELOG.md（文风见下），版本号只改 CMakeLists.txt 的 project(... VERSION x.y.z)
git add -A && git commit && git push
git tag vX.Y.Z && git push origin vX.Y.Z
```

CI 会在 tag 上自动编译并出一个安装包（`dist/AirPlay-Bridge-Setup-vX.Y.Z.exe`），
release notes 从 CHANGELOG 自动提取。

**发布前必须问自己一句：这次的 exe 和上一版有什么区别？答不上来就不发。**

---

## 硬约束

违反任一条会导致构建失败或功能回归。**原理与实测数据见
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) 和 [docs/DECISIONS.md](docs/DECISIONS.md)。**

### C1 — 延迟只能通过 `latencyMs()` 设置

`latency_`（帧 @44.1kHz）在上游 `third_party/airplay2-sender-cpp/src/raop_sender.h`。补丁已把上游硬编码断言改为
按 `latencyMs()` 动态计算 —— **直接写死数值会让测试挂**。
**下限 250ms**：低于它接收端缓冲被掏空（underrun），听感就是爆音。

### C2 — 补丁是相对上游 `fc91350` 的 git diff

`patches/upstream-airplay2-sender-cpp.patch` 覆盖 4 个文件。重新生成：

```bash
cd third_party/airplay2-sender-cpp
git diff > ../../patches/upstream-airplay2-sender-cpp.patch
```

**重新生成后必须先 commit 再验证** —— `git clone` 只认已提交内容。

### C3 — 行尾必须是 LF

`.gitattributes` 已锁 `* text=auto eol=lf` 与 `*.patch -text`。补丁带 CRLF 时
`git apply` 会 reject。自检：`file patches/upstream-airplay2-sender-cpp.patch` ——
出现 `with CRLF line terminators` 即为异常。

### C4 — Mbed TLS 钉在 3.6.0

固定 commit `2ca6c285a0dd3f33982dd57299012dacab1ff206`。网络受限时用
`-DFETCHCONTENT_SOURCE_DIR_MBEDTLS=<目录>` 跳过下载（首次约 190MB）。

### C5 — MSVC 必须加 `/utf-8`

`add_compile_options(/utf-8)`，**必须对所有目标生效，包括上游目标**。
源码含中文注释，MSVC 默认按 GBK 解码，UTF-8 序列末字节恰好是 GBK 前导字节时
会吞掉换行，报「标识符未定义」这类诡异错误。

### C6 — WIN32 子系统下不能用 `__argc` / `__argv`

`WinMain` 中 `__argc` 恒为 0，参数被静默忽略。改用
`CommandLineToArgvW(GetCommandLineW(), &argc)`。

### C6b — 日志清理策略

`logLine()` 中两条规则任一满足就 `remove()` 重开：超过 30 天未写入，
或超过 512KB（挂机时每 2 秒一条心跳，不限制会涨到几十 MB）。

### C7 — 音质档位改的是采集侧重采样

在采集回调里先转到 44100，再 `setInputFormat(44100)` —— 引擎走 pass-through，
完全绕过它自己的重采样。**好处是不动第三方代码，补丁不用跟着更新。**

- 配置字段 `qualityLevel`（0–100）：`taps = 4 + 28 * q / 100`
- 老配置迁移：`quality`（0–3）→ `0→0, 1→30, 2→60, 3→100`。
  **不换算的话老的 3（=最好）会被当成 3%（=很差）**
- 档位名：快速(<10) / 标准(10–29) / 高(30–54) / 极高(55–79) / 至臻(≥80)
- 改档要重连；自测用 `resampler_test` 目标

### C8 — 无控制台程序的诊断输出要落盘

本项目是 WIN32 子系统，`AttachConsole(ATTACH_PARENT_PROCESS)` 在重定向场景下会失败。
诊断类输出必须同时写文件。参考实现：`src/device/audiodevices.h` 的 `audioOut()`。

### C9 — 进程必须声明 DPI 感知

`WinMain` 第一行调 `enableDpiAwareness()`，**必须在创建任何窗口之前**。
不声明的话，非 100% 缩放下 Windows 会把整个界面按比例位图放大，全部发虚。

### C10 — 排查「握手超时 / 一直连接中」时，先确认本程序有入站 UDP 放行规则

接收端会**主动** UDP 回连本机做时钟同步，这些是无主入站报文。没有放行规则时
它们被静默丢弃，接收端不回应 stream SETUP —— 日志停在 `RECORD 200 OK`，
10 秒后 `AirPlay handshake TIMEOUT`，界面永远是「连接中」。
**排查顺序：先看防火墙规则，再看协议。** 详见 [docs/DECISIONS.md](docs/DECISIONS.md) D12。
## 禁止事项

**都是实际踩过的，别重试。** 按主题分组，不是按发生时间。
**为什么有这些规则 → [docs/PITFALLS.md](docs/PITFALLS.md)**（困惑时再读，不必通读）

### ① 状态与参数

- **改任何「状态字段」前，`grep -n "<字段>.store"` 列出所有写入点**，
  逐个确认配套动作（通知、清标志、更新 UI）。漏一处就会出现自相矛盾的行为，
  而且不报错不崩溃。踩过 **三次**（`standbyActive` / `wantReconnect` / `connected`）。
- **改结构体字段时，grep 它在同一函数内的所有赋值点** —— 后面的赋值会把你改的盖掉。
- **改「范围类参数」要覆盖四处**：档位表、默认值、**读取时的钳制**、UI 范围。
  最容易漏钳制 —— 表现为「菜单里选了，重启又变回去」。
- **静音待机的「0 秒」不能参与计时** —— 判据在 N=0 时恒成立，会连上就断、反复振荡。
  统一走 `standbyActiveConfig()`。
- **不要给引擎的类加成员变量** —— 试过两次，堆损坏崩溃。

### ② 路径与系统集成

- **往系统里存路径的功能都必须能自愈**：检查时比对实际路径；启动时发现指向的文件
  已不存在就自动修正。踩过 **两次**（防火墙规则、开机自启项）。
  但**「自动修正」有边界** —— 没这一项就不动、指向另一个存在的文件就不抢，见 PITFALLS。
- **一台机器上不允许两份副本并存** —— 规则按 exe 完整路径记录，两份会互相顶掉，
  永远修不好。**发布渠道只保留安装版。**
- **`netsh advfirewall firewall delete rule name="X"` 删的是全部同名规则** ——
  「只删多余那一条」这种设计从一开始就不成立。加规则前要**先全清**。
- **验证时污染了自己的环境，别把它当成产品缺陷** —— 我曾因此改掉一个原本正确的
  默认值，亲手制造了新的「两份并存」。默认值往往是有理由的。

### ③ 文案与行为

- **改提示文案不算修复 bug。** 说「已修复」之前必须确认改的是**行为**，
  不是**解释行为的文字**。判据：把用户的**操作路径**完整重跑一遍，**现象消失**才算修好。
  只改了文案 / 日志 / 措辞的，一律说「没修，只是让提示更清楚」。
- **同一个现象不许「解释」两次。** 用户第二次报同一问题时，先回看上次究竟动了什么。
- **用户报「X 不被认」，就去读判定 X 的那段代码。** 不要先改「提示」，
  也不要先改「产生 X 的流程」—— 判据错了，前面怎么改都不会对。
  （同一个现象被报了四次，前三次我都在改提示和安装流程。）

### ④ 工具链与脚本

- **不要在生成的源码里写反斜杠转义** —— 会被吃掉或翻倍，**而且照样能编译**，
  只在运行时炸。踩过 **四次**。用 `const char QC = '"';` / `char(10)` /
  `static_cast<char>(0)` 代替。
- **批量替换要排除替换目标自身** —— 否则「所有 A 换成 B」会把 B 的定义也换掉，
  自己调自己 = 无限递归，**而且编译照样通过**。
- **Edit / 脚本改完，立刻 grep 复核。**
- **同一个文件既读又写时，读和写绝不能出现在同一个表达式里** ——
  `open(f,'w').write(open(f).read())` 会先把文件截断成 0 字节。
- **`git add -A` 之前先 `git status`** —— 临时产物不进版本控制。
- **日志的最后一行不可靠**，不要用它推断崩溃点（日志有缓冲，崩溃时不会刷出来）。
  正确做法：在可疑点**前后各加一条打点**。
- **测试脚本跑完先确认「被测程序真的起来了、日志真的在写」**，再看结果。
- **看门狗报告里「最后阶段 + 有没有恢复」信息量极大**：有恢复 = 偶发卡顿；
  **没恢复 = 死锁**。

### ⑤ 对外表述

- **对外文案不要写 Apple TV** —— 设备列表默认屏蔽它（`hideAppleTV=true`），
  README、仓库 About、发布说明等所有对外描述只写「AirPlay 2 设备」。
- **示例里不许出现看起来像真实的设备名 / 局域网 IP**，用 `<设备名>` / `<设备 IP>`。
- **执行「以后不要 X」只作用于「以后」，不清理历史** —— 删除类操作尤其：
  动手前先问「我要删的这批里，有没有本来就不是 X 的东西」。
  （曾把 35 个历史 release 删空，不可恢复。）
- **不要在单元测试里依赖 `latency_` 的字面值** —— 见 C1。
- **原生菜单的限制**：不用 `enabled=false` 显示状态（整行会被变灰）；
  不用 emoji / 位图给菜单上色（不生效，只有系统勾选态能上色）；
  不做「点击才展开的独立菜单」（展开时主菜单会消失）。

### ⑥ 发布纪律

- **不许因为「只改了文档」就升版。**（用户明确要求写死）
  升版 = **给用户一个新安装包**。exe 和上一版一模一样时，**那个版本号是假的**。
  判据：这次改了 `src/` 下的代码吗？没改就**只 push**，CHANGELOG 也不记。
  （犯过：README 补了说明就照惯例发了版，用户一句话点破
  「代码没动 为啥升版呢？不是跟上版一致吗？」已撤回。）
- **发版前必须问自己：这次的 exe 和上一版有什么区别？答不上来就不发。**
  既有清单全在回答「这个版本好不好」，没有一项回答「该不该存在」。
- **发版前扫一眼最近的决定/待办** —— 指示落在对话里、不落到行动上就等于没听见。
## 不确定时怎么办

1. **先查代码** —— `grep -rn "<关键字>" src/ docs/`
2. **再查 docs/** —— 架构看 ARCHITECTURE，决策看 DECISIONS，术语看 GLOSSARY
3. **第三方库的问题，先读它的 README + 搜 `TODO` / `FIXME` / `known issues` / `gotcha`**，
   再读代码。上游作者踩过的坑比推理值钱。
4. **用户报的现象和你的复现不一致时，先怀疑「用户走的路径和我不同」**，
   而不是继续在同一个路径上挖代码。
5. **还是不确定 → 用 `TODO(需人类确认)` 标出来，问人类。**
6. **绝不许猜一个看起来合理的答案就写进去。** 宁可标 TODO。

**多个现象同时出现时，先问「它们有没有共同的上游」**，而不是分别去查每个现象。
（血泪：用户报「连不上 + 每次提醒防火墙」，听起来像两件事，
实际是一条链 —— 安装没装完整。）

---

## 交接

半成品不能留在脑子里。收尾时逐条确认：

- [ ] 改代码 → 跑了验收（[docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) 的「验收」）
- [ ] 改行为 → 同步了 ARCHITECTURE 或 DECISIONS
- [ ] 改界面文案 → 全仓库 grep 过旧文案
- [ ] 有代码改动 → 更新了 CHANGELOG.md（双语，中文在前）
- [ ] 验证 → **留下了证据**（日志片段 / 实测数据），不是只说「通过了」
- [ ] 没做完 / 没解决的问题 → 写进「已知未解问题」或「挂账」
- [ ] 临时调试代码（`TEMP-` / `DIAG-`）→ **已删，并 `grep -c` 确认为 0**
## 已知未解问题

| ID | 问题 | 状态 |
|---|---|---|
| K1 | 事件通道断开后不重连 | 修复尝试加成员变量导致堆损坏，已回滚。有上层 6 秒重连兜底 |
| K2 | 多设备混音采集全是静音 | 单设备正常。接管模式下不需要，暂搁置 |
| K3 | `IPolicyConfig` 是未公开接口 | Windows 更新可能失效。已做重试 + 复核 |
| K4 | 虚拟音频设备会让静音判定永远为假 | 混音里有任何一路在输出（例如 UU远程虚拟声卡的底噪），峰值就超过判定阈值 → 待机永不触发、接收端屏幕一直亮。**阈值从 300 降到 20 是为解决相反方向的问题**（小声内容被误判静音），代价是这一侧更容易触发。用户可关掉静音待机规避；根治要按设备过滤或做真自适应 |

## 挂账

| 项 | 说明 | 位置 |
|---|---|---|
| macOS 版 | 不是本项目移植，需求是「开机自动把输出切到 HomePod」。经调研 macOS 不提供该能力（AirPlay 设备自 Big Sur 起不再作为普通 Core Audio 设备暴露，脚本访问权限已被 Apple 移除），界面自动化方案在 macOS 26 上已失效。可自动化的路线反而是自行采集系统音频（Core Audio process taps，需 macOS 14.2+）。暂不启动 | [issue #1](https://github.com/RiverFlowsInUUU/AirPlay-Bridge/issues/1) |

---

## 发布说明文风（硬性）

发布说明是给用户的，不是开发周记。

**必须**：按 `Added` / `Changed` / `Fixed` / `Removed` / `Deprecated` / `Security`
分组；一条 bullet 一个改动；用户视角（写「修复了 X 时会 Y」，不写「重构了 Z 模块」）；
破坏性变更单独一节且位置显眼；可扫读 —— 用户 10 秒内能判断要不要升级。

**禁止**：第一人称与叙事（「我」「我们觉得」「被指出」「之前偷懒了」）；
解释「为什么这么改」的段落；实现细节（文件路径、函数名、commit hash、PR 编号）；
开篇句式「这一版改了什么」。

正确开头就是直接 `## Added` / `## Fixed` 开列。日期用 ISO 格式，同步更新 `CHANGELOG.md`。

**双语（2026-10-16 起）**：CHANGELOG 每个版本写**两块**，**中文在前、英文在后**，
各自带自己的分组标题。

```markdown
## [1.4.4] - 2026-10-16

**中文**

### 修改

- 中文条目

**English**

### Changed

- English entry
```

**整块分开，不是逐条对照** —— 交错读起来是断裂的，分组标题还会重复出现两次。
CI 的 release notes 提取是「从 `## [版本]` 到下一个 `## [`」，两块都在区间里。

---

## README 文风（硬性）

README 是唯一人类入口。**只陈述，不解释。**

**必须**：开头一句话说清是什么；紧跟安装步骤；特性一行一条；
配置 / 命令 / 参数一律用表格；细节链接到 `docs/`。

**禁止**：解释「为什么这么做」；背景故事与心路历程；
段落式散文（连续超过两行的文字就该改成表格或链接出去）；
内部实现细节；**命令行用法**（用户走 GUI，参数说明只属于 DEVELOPMENT.md）。

**判断标准**：读者扫一遍就知道「这是什么、怎么跑起来、去哪深入」。
想解释的冲动全部让给 `docs/`。

**双语**：

- **`README.md` 是英文**（默认，面向英文用户）；**`README.zh-CN.md` 是中文**
- 两份顶部互相链接
- **改一份必须同时改另一份** —— 结构、小节、表格都要对应
- **行数各自控制在 150 以内**（不是两份加起来）

**Emoji 只限句首**（章节标题、特性条目的开头）—— 句子中间、结尾、正文里一律不要。

---

## 文档地图

| 文件 | 内容 |
|---|---|
| [README.md](README.md) | 人类入口（英文） |
| [README.zh-CN.md](README.zh-CN.md) | 人类入口（中文） |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 分层、模块表、数据流、不变量、协议补丁、实测数据 |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | 环境、构建、测试、验收、调试、命令行参数、图标、发布 |
| [docs/DECISIONS.md](docs/DECISIONS.md) | D1–D15 关键决策与理由、被否决的方案 |
| [docs/GLOSSARY.md](docs/GLOSSARY.md) | 术语表 |
| [docs/PITFALLS.md](docs/PITFALLS.md) | 每条禁令「为什么存在」—— 踩坑复盘 |
| [CHANGELOG.md](CHANGELOG.md) | 变更记录（人类可读，双语） |

**文档之间不重复。** 规范在本文；理由在 DECISIONS；原理与实测在 ARCHITECTURE；
操作步骤在 DEVELOPMENT。**同一件事只写一处，其他处放指针。**
