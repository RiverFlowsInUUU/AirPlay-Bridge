# AGENTS.md

**本仓库唯一的 AI 入口。** 其他工具（Claude Code / Cursor / Copilot / Gemini…）
一律读这一份，**不设平台副本** —— 副本会漂移，规范只有一处才权威。

**这份文件是目录和指路牌，不是内容仓库。**
读完它你应该知道「项目是什么、从哪开始、什么绝对不能做」，
以及**每件事该去哪个文件看**。规范细节一律在 `docs/`。

人类说明见 [README.md](README.md)。本项目的代码、文档、发布流程都由 agent 维护。

---

## 这是什么

Windows 托盘程序，把系统声音实时串流到 AirPlay 2 设备（HomePod mini）。
起因是 TuneBlade 在 HomePod OS 27 上失效。

**技术栈**：C++17 / MSVC 2022 / CMake ≥3.20 / Win32（WIN32 子系统，无控制台）。
协议引擎来自 [airplay2-sender-cpp](https://github.com/akustikrausch/airplay2-sender-cpp)
（git submodule）+ 本项目补丁。

**数据流**：WASAPI loopback 采集 → 重采样到 44100 → RAOP 引擎 → 接收端。

---

## 从零接手（冷启动）

**第一次进入本项目的 AI，按这个顺序走。** 每一步的具体做法在
[docs/WORKFLOW.md](docs/WORKFLOW.md)。

| # | 做什么 | 去哪 |
|---|---|---|
| 1 | 搞清项目在做什么、代码怎么分层 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 2 | 把环境跑起来 —— **这是最先要做的事**，环境不通什么都别谈 | [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) 的「环境」「构建」 |
| 3 | **验证你跑的是你刚编的**（版本号 + 能连上 + 日志有输出） | [docs/WORKFLOW.md](docs/WORKFLOW.md) 的「环境自检」 |
| 4 | 确认这次要改什么、影响哪些层、有哪些相关约束 | [docs/WORKFLOW.md](docs/WORKFLOW.md) 的「第 4 步 — 确认任务」 |
| 5 | 看清红线 —— **有 19 条是踩过坑换来的** | [docs/RULES.md](docs/RULES.md) |
| 6 | 小步改 + 每步验证 + 提交 + 交接 | [docs/WORKFLOW.md](docs/WORKFLOW.md) |

**不要跳过第 2、3 步。** 历史上多次出现「验证通过」实际测的是旧二进制 ——
见 [docs/PITFALLS.md](docs/PITFALLS.md) 的「验证前先核对被测二进制的版本号」。

---

## 任务速查

| 我要做的事 | 先读 |
|---|---|
| 改代码 | 架构 → [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)，红线 → [docs/RULES.md](docs/RULES.md) |
| 加/改界面文案 | [docs/STYLE.md](docs/STYLE.md)（i18n 规则），改完要全仓库 grep 旧名 |
| 改配置项或档位 | [docs/RULES.md](docs/RULES.md) 的「范围类参数」——至少四处 |
| 写第三方库相关代码 | **先读那个库的 README 和源码注释**，别只读代码 |
| 提交 | [docs/WORKFLOW.md](docs/WORKFLOW.md) 的「提交」 |
| 发版 | [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) 的「发布」+ [docs/RULES.md](docs/RULES.md) 的发布纪律 |
| 写 README / 发布说明 | [docs/STYLE.md](docs/STYLE.md) |
| 某条禁令看不懂 | [docs/PITFALLS.md](docs/PITFALLS.md) |
| 查术语 | [docs/GLOSSARY.md](docs/GLOSSARY.md) |

---

## 红线

**完整清单在 [docs/RULES.md](docs/RULES.md)，动手前必须读一遍。**
这里只列最容易犯、代价最大的几条：

- **不许改提示文案冒充修复** —— 说「已修复」前，确认改的是**行为**，不是解释行为的文字。
- **不许因为「只改了文档」就升版** —— 升版 = 给用户新安装包。
  **发版前问：这次的 exe 和上一版有什么区别？答不上来就不发。**
- **改「状态字段」或「范围类参数」前，先列出所有涉及它的地方** ——
  同一个病犯过三次以上，而且不报错、不崩溃。
- **验证前先确认被测二进制的版本号** —— 版本不对，后面所有「通过」都是假的。
- **一台机器上不允许两份副本并存** —— 发布渠道只保留安装版。
- **不许猜** —— 不确定就标 `TODO(需人类确认)`，问人。

---

## 不确定时怎么办

1. **先查代码** —— `grep -rn "<关键字>" src/ docs/`
2. **再查 docs/** —— 架构看 ARCHITECTURE，决策看 DECISIONS，术语看 GLOSSARY
3. **第三方库的问题，先读它的 README + 搜 `TODO` / `FIXME` / `known issues` / `gotcha`**，
   再读代码。上游作者踩过的坑比推理值钱。
4. **用户报的现象和你复现的不一致时，先怀疑「路径不同」**，
   而不是继续在同一个路径上挖代码。
5. **还是不确定 → 标 `TODO(需人类确认)`，问人类。绝不许猜一个看起来合理的答案写进去。**

**多个现象同时出现时，先问「它们有没有共同的上游」** ——
历史上「连不上 + 每次提醒防火墙」听着像两件事，实际是一条链。

---

## 文档地图

**单一事实源。同一件事只写一处，其他处放指针。**

| 文件 | 内容 | 什么时候读 |
|---|---|---|
| [docs/WORKFLOW.md](docs/WORKFLOW.md) | **从零接手 · 改前改后 · 提交 · 交接 · 换 Agent 一致性** | 开始干活前 |
| [docs/RULES.md](docs/RULES.md) | **硬约束 C1–C10 + 19 条禁止事项** | **动手前必读** |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | 分层、模块表、数据流、不变量、协议补丁、实测数据 | 改代码前 |
| [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) | 环境、构建、测试、验收、调试、命令行参数、图标、发布 | 跑环境 / 发版 |
| [docs/DECISIONS.md](docs/DECISIONS.md) | D1–D15 关键决策与理由、被否决的方案 | 想知道「为什么这样」 |
| [docs/STYLE.md](docs/STYLE.md) | 发布说明文风、README 文风、i18n 约定 | 写对外文档 |
| [docs/PITFALLS.md](docs/PITFALLS.md) | 每条禁令为什么存在 —— 踩坑复盘 | 禁令看不懂时 |
| [docs/GLOSSARY.md](docs/GLOSSARY.md) | 术语表 | 遇到不认识的词 |
| [README.md](README.md) / [README.zh-CN.md](README.zh-CN.md) | 人类入口（英/中） | 写给用户看 |
| [CHANGELOG.md](CHANGELOG.md) | 变更记录（双语） | 发版时更新 |

**平台入口**：只此一份 `AGENTS.md`。**不建 CLAUDE.md / .cursorrules /
copilot-instructions.md 等副本** —— 内容全是同一个指针，多建一个就多一处漂移点。

---

## 已知未解问题

| ID | 问题 | 状态 |
|---|---|---|
| K1 | 事件通道断开后不重连 | 修复尝试加成员变量导致堆损坏，已回滚。有上层 6 秒重连兜底 |
| K2 | 多设备混音采集全是静音 | 单设备正常。接管模式下不需要，暂搁置 |
| K3 | `IPolicyConfig` 是未公开接口 | Windows 更新可能失效。已做重试 + 复核 |
| K4 | 虚拟音频设备会让静音判定永远为假 | 详见表下 |

K4 细节：混音里有任何一路在输出（例如 UU远程虚拟声卡的底噪），峰值就超过判定阈值
→ 待机永不触发、接收端屏幕一直亮。**阈值从 300 降到 20 是为解决相反方向的问题**
（小声内容被误判静音），代价是这一侧更容易触发。用户可关掉静音待机规避；
根治要按设备过滤或做真自适应。

## 挂账

| 项 | 说明 | 位置 |
|---|---|---|
| macOS 版 | 不是本项目移植，需求是「开机自动把输出切到 HomePod」。经调研 macOS 不提供该能力（AirPlay 设备自 Big Sur 起不再作为普通 Core Audio 设备暴露，脚本访问权限已被 Apple 移除），界面自动化方案在 macOS 26 上已失效。可自动化的路线反而是自行采集系统音频（Core Audio process taps，需 macOS 14.2+）。暂不启动 | [issue #1](https://github.com/RiverFlowsInUUU/AirPlay-Bridge/issues/1) |
