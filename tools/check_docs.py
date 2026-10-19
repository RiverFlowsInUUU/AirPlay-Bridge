#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""核对「文档里的声明」与「代码里的真值」是否一致。

为什么需要它
------------
文档会腐烂，不是因为没人勤奋，而是因为**写下的声明和实际之间没有连接**。
2026-10-18 审计发现：AGENTS.md 写着「有 19 条禁止事项」（实际 32 条）、
「硬约束 C1–C10」（实际到 C13）；ARCHITECTURE.md 写 `main()` 约 960 行
（实际 1483 行）—— 而且这些数字都出现在「动手前必读」的位置。

靠「每次改完记得查一遍文档」是没用的：触发条件太宽（大多数改动不动文档），
一宽就变成噪声，噪声最终被跳过。**所以把能机械核对的都钉在这里。**

设计原则
--------
- **有声明就必须对得上**：找不到声明锚点 = FAIL，不是静默跳过。
  这是故意的 —— 改动了声明的措辞，就该来更新这份清单（跟改接口要改测试同理）。
  否则「把声明删掉」会变成悄悄关掉检查的手段。
- **只核对能机械判断的**：措辞、语气、完整性这些判断类的东西不在这里，
  那属于 [RULES.md](../docs/RULES.md) C14 的「窄触发表」。

用法
----
    python tools/check_docs.py          # 在仓库根目录跑
退出码 0 = 全部一致；1 = 有不一致。
"""

import io
import os
import re
import sys

# Windows 上 stdout 默认可能是 cp936，中文输出会炸。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

results = []  # (status, name, detail)


def read(rel):
    path = os.path.join(ROOT, rel)
    if not os.path.exists(path):
        return None
    return io.open(path, encoding="utf-8").read()


def ok(name, detail=""):
    results.append(("OK", name, detail))


def fail(name, detail):
    results.append(("FAIL", name, detail))


def find_any(rel, patterns, name):
    """按顺序试多个锚点，命中任一即可；全都命中不到才判 FAIL。

    中英文 README 的措辞不同，同一个事实要有多个锚点候选。
    """
    text = read(rel)
    if text is None:
        fail(name, "%s 不存在" % rel)
        return None
    for pat in patterns:
        m = re.search(pat, text)
        if m:
            return m
    fail(name, "%s 里找不到任一声明锚点（措辞被改过？去更新 check_docs.py）" % rel)
    return None


# ---------------------------------------------------------------- 真值提取

def code_numbers(rel, array_name):
    """从 C++ 数组里取出所有 `{ 数字, "..." }` 的数字（按出现顺序）。"""
    text = read(rel)
    if text is None:
        return None
    i = text.find(array_name + "[]")
    if i < 0:
        return None
    body = text[i:i + 2000]
    end = body.find("};")
    if end > 0:
        body = body[:end]
    return [int(x) for x in re.findall(r"\{\s*(\d+)\s*,", body)]


def slider_range(rel, after_marker):
    """取某段之后的 spec.minValue / spec.maxValue。"""
    text = read(rel)
    if text is None:
        return None
    i = text.find(after_marker)
    if i < 0:
        return None
    seg = text[i:i + 1500]
    lo = re.search(r"spec\.minValue\s*=\s*(\d+)", seg)
    hi = re.search(r"spec\.maxValue\s*=\s*(\d+)", seg)
    if not lo or not hi:
        return None
    return (int(lo.group(1)), int(hi.group(1)))


def dash_range(near):
    """把 '100–300' / '100-300' 这类区间抓成 (100, 300)。"""
    m = re.search(r"(\d+)\s*[–\-—~]\s*(\d+)", near)
    return (int(m.group(1)), int(m.group(2))) if m else None


CORE = "src/app/airplay_bridge.cpp"
DIALOG = "src/ui/latency_dialog.cpp"


# -------------------------------------------------- ① 延迟档位

truth = code_numbers(CORE, "kLatencyLevels")
if truth is None:
    fail("延迟档位", "在 %s 里找不到 kLatencyLevels" % CORE)
else:
    for rel, pats in (
        ("README.md", [r"Latency presets:\s*([^\n.]*)"]),
        ("README.zh-CN.md", [r"延迟档位：\s*([^\n。]*)"]),
    ):
        m = find_any(rel, pats, "延迟档位 (%s)" % rel)
        if m:
            got = [int(x) for x in re.findall(r"\d+", m.group(1))]
            got = [x for x in got if x >= 50]      # 滤掉 "5 presets" 这类噪声
            if got == truth:
                ok("延迟档位 (%s)" % rel, str(got))
            else:
                fail("延迟档位 (%s)" % rel,
                     "文档写 %s，代码是 %s" % (got, truth))

# -------------------------------------------------- ② 延迟滑块范围

rng = slider_range(DIALOG, "LatencyDialog")
if rng is None:
    fail("延迟滑块范围", "在 %s 里取不到 spec.minValue/maxValue" % DIALOG)
else:
    for rel in ("README.md", "README.zh-CN.md"):
        m = find_any(rel, [
            r"continuous slider\s*[（(]\s*(\d+\s*[–\-—~]\s*\d+)\s*[)）]",
            r"无极滑块[（(]\s*(\d+\s*[–\-—~]\s*\d+)\s*[)）]",
        ], "延迟滑块范围 (%s)" % rel)
        if m:
            got = dash_range(m.group(1))
            if got == rng:
                ok("延迟滑块范围 (%s)" % rel, "%d–%d" % got)
            else:
                fail("延迟滑块范围 (%s)" % rel,
                     "文档写 %s，代码是 %s" % (got, rng))

# -------------------------------------------------- ③ 音量档位

vt = code_numbers(CORE, "kVolumeLevels")
if vt is None:
    fail("音量档位", "在 %s 里找不到 kVolumeLevels" % CORE)
else:
    m = find_any("README.md", [r"Volume presets:\s*([^\n.]*)"],
                 "音量档位 (README.md)")
    if m:
        items = [x.strip() for x in re.split(r"[/、]", m.group(1)) if x.strip()]
        if len(items) == len(vt):
            ok("音量档位 (README.md)", "%d 项" % len(items))
        else:
            fail("音量档位 (README.md)",
                 "文档列 %d 项（%s），代码有 %d 档 %s"
                 % (len(items), items, len(vt), vt))

# -------------------------------------------------- ③b 音量档位（表格行）

# 表格是用户扫读的地方，和下面那行明细说的是同一件事 —— 两处会漂移。
# 2026-10-18 实例：表格写「Off + 30% steps」，而实际档位是 30/50/70/100
# （不是 30% 等距）。明细行是对的，表格是错的 —— 而用户先看的是表格。
if vt is not None:
    want_presets = " / ".join("%d%%" % v for v in vt if v != 0)
    for rel in ("README.md", "README.zh-CN.md"):
        text = read(rel)
        if text is None:
            fail("音量档位表格行 (%s)" % rel, "%s 不存在" % rel)
            continue
        m = re.search(r"^\|\s*(?:Volume|音量)\s*\|([^|]*)\|", text, re.M)
        if not m:
            fail("音量档位表格行 (%s)" % rel,
                 "找不到音量那一行（措辞被改过？去更新 check_docs.py）")
            continue
        row = m.group(1).strip()
        if want_presets in row:
            ok("音量档位表格行 (%s)" % rel, want_presets)
        else:
            fail("音量档位表格行 (%s)" % rel,
                 "表格写「%s」，应含「%s」" % (row[:60], want_presets))

# -------------------------------------------------- ④ 待机档位

st = code_numbers(CORE, "kStandbyLevels")
if st is None:
    fail("待机档位", "在 %s 里找不到 kStandbyLevels" % CORE)
else:
    want = [x // 10 for x in st]          # 代码存的是十分之一秒
    m = find_any("README.md", [r"Standby presets:\s*([^\n.]*)"],
                 "待机档位 (README.md)")
    if m:
        got = [int(x) for x in re.findall(r"\d+", m.group(1))][:len(want)]
        if got == want:
            ok("待机档位 (README.md)", str(got))
        else:
            fail("待机档位 (README.md)",
                 "文档写 %s，代码是 %s" % (got, want))

# -------------------------------------------------- ⑤ 约束编号范围

rules = read("docs/RULES.md")
if rules is None:
    fail("约束编号范围", "docs/RULES.md 不存在")
else:
    nums = [int(x) for x in re.findall(r"^### C(\d+)", rules, re.M)]
    if not nums:
        fail("约束编号范围", "docs/RULES.md 里找不到任何 `### Cn`")
    else:
        hi = max(nums)
        claims = []
        for rel in ("AGENTS.md", "docs/WORKFLOW.md"):
            text = read(rel)
            if not text:
                continue
            for m in re.finditer(r"C1\s*[–\-—~]\s*C(\d+)", text):
                claims.append((rel, int(m.group(1))))
        if not claims:
            fail("约束编号范围",
                 "文档里找不到 `C1–Cn` 声明（若故意去掉，请更新 check_docs.py）")
        else:
            bad = [(r, n) for r, n in claims if n != hi]
            if bad:
                fail("约束编号范围",
                     "文档写 %s，RULES.md 实际到 C%d"
                     % (", ".join("%s→C%d" % t for t in bad), hi))
            else:
                ok("约束编号范围",
                   "C1–C%d（%s）" % (hi, ", ".join(r for r, _ in claims)))

# -------------------------------------------------- ⑥ 补丁覆盖文件数

patch = read("patches/upstream-airplay2-sender-cpp.patch")
if patch is None:
    fail("补丁文件数", "patches/upstream-airplay2-sender-cpp.patch 不存在")
else:
    n_files = len(re.findall(r"^diff --git ", patch, re.M))
    claims = []
    for rel in ("docs/RULES.md", "docs/ARCHITECTURE.md"):
        text = read(rel)
        if not text:
            continue
        for m in re.finditer(r"覆盖\s*(\d+)\s*个文件", text):
            claims.append((rel, int(m.group(1))))
    if not claims:
        fail("补丁文件数", "文档里找不到「覆盖 N 个文件」声明")
    else:
        bad = [(r, n) for r, n in claims if n != n_files]
        if bad:
            fail("补丁文件数",
                 "文档写 %s，补丁实际覆盖 %d 个文件"
                 % (", ".join("%s→%d" % t for t in bad), n_files))
        else:
            ok("补丁文件数", "%d 个（%s）"
               % (n_files, ", ".join(r for r, _ in claims)))

# -------------------------------------------------- ⑦ i18n：用户可见文案不许裸写中文

# 2026-10-18 实例（P2-3）：setTooltip("AirPlay Bridge - 已连接") 三处裸中文，
# 而 i18n 表里**早就有**对应英文词条 —— 词条是死的，英文界面一直显示中文。
# 这类问题机器能查：只看「传给用户可见出口的字面量里有没有中文」。
#
# 出口名单是故意写死的（窄）：新增一个面向用户的出口时，要把它加进来。
_SINKS = ["setTooltip(", "onStatus(", "tray.notify(", "notify(", "MessageBox",
          "promptUser("]
_SRC = ["src/app/airplay_bridge.cpp", "src/ui/tray.cpp",
        "src/ui/latency_dialog.cpp", "src/app/i18n.cpp"]
_CJK = re.compile("[一-鿿]")

_rawhits = []
for _rel in _SRC:
    _text = read(_rel)
    if not _text:
        continue
    if _rel.endswith("i18n.cpp"):
        continue            # 词条表本身就是中文，不查
    _lines_txt = _text.split(chr(10))
    for _ln, _line in enumerate(_lines_txt, 1):
        _st = _line.strip()
        if _st.startswith("//"):
            continue
        for _sink in _SINKS:
            _k = _line.find(_sink)
            if _k < 0:
                continue
            # ★ 跨行调用也要看得到：`setTooltip(` 换行后字面量在下一行。
            #   只看同一行是原版，会漏（2026-10-18 外部审查指出）。
            # ★ 从 sink 出发一直累计到匹配的右括号（最多再看 4 行）。
            #   原来只拼下一行，写成三行（参数 + 字面量各占一行）就漏了
            #   —— 2026-10-18 第三轮外部审查指出。
            _rest = _line[_k + len(_sink):]
            # ★ 计数器从 1 起（sink 名里那个左括号已经被吃掉）。
            #   写成 depth=0 的话，「promptUser( 独占一行」这种最常见的形态
            #   会算出 0 → 循环不进 → 完全漏掉。2026-10-18 我改错过一次，
            #   是反向测试把它揪出来的。
            _depth = 1 + _rest.count("(") - _rest.count(")")
            _j = _ln            # _lines_txt 是 0 基，_ln 是 1 基 → 下一行正是该下标
            while _depth > 0 and _j < len(_lines_txt) and (_j - _ln) < 4:
                _rest += " " + _lines_txt[_j]
                _depth += _lines_txt[_j].count("(") - _lines_txt[_j].count(")")
                _j += 1
            if "i18n::tr" in _rest or "tr(" in _rest:
                continue
            # ★ 宽字符字面量 L"..." 也要认。原来只认不带前缀的 "..."，
            #   而 promptUser 的参数类型是 const wchar_t*，实际写法几乎都是
            #   L"中文" —— 于是这条检查对 promptUser 基本无效。
            #   （2026-10-18 第三轮外部审查的反向测试用例就是 L"裸中文"。）
            for _m in re.finditer(r'(?:L)?"([^"]*)"', _rest):
                if _CJK.search(_m.group(1)):
                    _rawhits.append("%s:%d %s" % (_rel, _ln, _m.group(1)[:30]))
                    break

if _rawhits:
    for _h in _rawhits[:8]:
        fail("i18n 裸中文 (%s)" % _h.split(" ")[0], _h)
else:
    ok("i18n 裸中文扫描", "无（出口：%s）" % ", ".join(_SINKS))

# -------------------------------------------------- ⑧ 技术栈声明（AGENTS vs CMake）

# 2026-10-18 实例（P2-4a）：AGENTS.md 写「C++17 / CMake ≥3.20」，
# 而实际是 C++20 / cmake_minimum_required(3.16) —— 两个数字都反了。
# 这类「自述里的工具链版本」是最容易腐烂的一类（改 CMake 时没人会想到翻 AGENTS）。
_cm = read("CMakeLists.txt") or ""
_agents = read("AGENTS.md") or ""
_std = re.search(r"CMAKE_CXX_STANDARD\s+(\d+)", _cm)
_min = re.search(r"cmake_minimum_required\(VERSION\s+([0-9.]+)", _cm)
if _std:
    _want = "C++" + _std.group(1)
    _m = re.search(r"C\+\+(\d+)", _agents)
    if not _m:
        fail("技术栈 C++ 版本", "AGENTS.md 里找不到 C++NN 声明")
    elif ("C++" + _m.group(1)) == _want:
        ok("技术栈 C++ 版本", _want)
    else:
        fail("技术栈 C++ 版本",
             "AGENTS.md 写 C++%s，CMakeLists 是 %s" % (_m.group(1), _want))
if _min:
    _m = re.search(r"CMake\s*≥\s*([0-9.]+)", _agents)
    if not _m:
        fail("技术栈 CMake 版本", "AGENTS.md 里找不到 CMake 版本声明")
    elif _m.group(1) == _min.group(1):
        ok("技术栈 CMake 版本", "≥" + _min.group(1))
    else:
        fail("技术栈 CMake 版本",
             "AGENTS.md 写 ≥%s，CMakeLists 实际 %s" % (_m.group(1), _min.group(1)))

# -------------------------------------------------- ⑨ 抽头数说明（resampler.h vs 常量）

# 2026-10-18 实例（P2-4b）：注释写「4..64」，实际 kQualityMaxTaps = 32。
# 上一轮外部审查就提过这条，改了别处却漏了它 —— 正好说明需要机器盯。
_rs = read("src/audio/resampler.h") or ""
_mx = re.search(r"kQualityMaxTaps\s*=\s*(\d+)", _rs)
_mn = re.search(r"kQualityMinTaps\s*=\s*(\d+)", _rs)
if _mx:
    # ★ 两种写法都要盯：`抽头数 N..M` 和散文式「抽头数从 N 线性升到 M」。
    #   原来只认前者，而 P2-4 里被点名的恰恰是后者那条散文 ——
    #   机器盯住了另一行。（2026-10-18 外部审查指出覆盖面比安全感窄。）
    _claims = []
    for _m in re.finditer(r"抽头数\s*(\d+)\.\.(\d+)", _rs):
        _claims.append((_m.group(1), _m.group(2), "抽头数 N..M"))
    for _m in re.finditer(r"抽头数从\s*(\d+)\s*线性升到\s*(\d+)", _rs):
        _claims.append((_m.group(1), _m.group(2), "散文式"))
    if not _claims:
        fail("抽头数说明", "resampler.h 里找不到任何一种抽头数声明")
    else:
        _bad = [(a, b, t) for (a, b, t) in _claims if b != _mx.group(1)]
        if _bad:
            fail("抽头数说明",
                 "%s" % "; ".join("%s 写 %s..%s，kQualityMaxTaps 实际 %s"
                                  % (t, a, b, _mx.group(1)) for a, b, t in _bad))
        else:
            ok("抽头数说明", "%d 处声明一致（上限 %s）" % (len(_claims), _mx.group(1)))

# -------------------------------------------------- ⑩ i18n 死词条（零引用）

# 2026-10-18 实例（P3-9）：词条表里有 16 条在全部源码里零引用 ——
# 多是已删功能的残留（首次运行的选语言向导、开机自启引导、早期 tooltip 文案）。
# 第 ⑦ 节只查「裸中文」（漏翻译方向），不查「多余词条」方向，所以 P3-9 一直没闭环。
#
# 做法：把 src/ 里除 i18n.cpp 之外的源码**去掉注释**后拼成一个大串，
# 再把引号与空白全去掉（源码里相邻字面量会被编译器拼接，去空白才对得上），
# 然后看每个词条 key 是否出现。
# 去掉注释是必要的：否则「注释里提到过某个 key」会把死词条误判成活的。
_I18N_SRC = read("src/app/i18n.cpp") or ""
_blob_parts = []
# ★ 覆盖 src/ 下全部 .cpp/.h（只排除 i18n.cpp 自身）。
#   原来写死 6 个文件 —— 若某词条只在被排除的文件里引用，会被误判成死词条。
#   当前 117 条零死词条是"恰好没踩到"，不是"踩不到"。
#   （2026-10-18 第三轮外部审查指出。）
_srclist = []
for _root, _dirs, _files in os.walk("src"):
    for _f in sorted(_files):
        if not _f.endswith((".cpp", ".h")):
            continue
        _rel = os.path.join(_root, _f).replace(os.sep, "/")
        if _rel.endswith("i18n.cpp"):
            continue
        _srclist.append(_rel)
_srclist.sort()
for _rel in _srclist:
    _t = read(_rel)
    if not _t:
        continue
    _t = re.sub(r"/\*.*?\*/", "", _t, flags=re.S)      # 去块注释
    _t = re.sub("//[^" + chr(10) + "]*", "", _t)                       # 去行注释
    _blob_parts.append(_t)
_blob = "".join(_blob_parts)
_blob = _blob.replace(chr(34), "").replace(chr(39), "")
_blob = re.sub(r"\s+", "", _blob)

_entries = re.findall(r"^\s*\{\s*\"([^\"]+)\"\s*,", _I18N_SRC, re.M)
if not _entries:
    fail("i18n 死词条", "在 i18n.cpp 里解析不出任何词条")
else:
    _dead = [k for k in _entries if re.sub(r"\s+", "", k) not in _blob]
    if _dead:
        for _k in _dead[:8]:
            fail("i18n 死词条", "零引用：%s" % _k[:44])
        if len(_dead) > 8:
            fail("i18n 死词条", "…另有 %d 条零引用" % (len(_dead) - 8))
    else:
        ok("i18n 死词条", "无（共 %d 条词条）" % len(_entries))

# -------------------------------------------------- ⑪ 卸载器不许递归删整个 {app}

# 2026-10-18 实例（第四轮审查 P1-1）：[UninstallDelete] 里有
#   Type: filesandordirs; Name: "{app}"
# 这会在用户把程序装进已有内容的目录（如 D:\Tools）时，卸载递归删光那里 ——
# 不可逆的数据丢失。而 [Files] 本来就会自动删自己装的 4 个文件，
# 那条规则唯一的作用就是删掉原本不该删的东西。
_iss = read("installer/airplay-bridge.iss") or ""
if not _iss:
    fail("卸载器递归删除", "读不到 installer/airplay-bridge.iss")
else:
    # 只看非注释行
    _live = [ln for ln in _iss.split(chr(10))
             if not ln.strip().startswith(";")]
    _bad = [ln.strip() for ln in _live
            if "filesandordirs" in ln and "{app}" in ln]
    if _bad:
        fail("卸载器递归删除", "仍然存在: %s" % _bad[0][:60])
    else:
        ok("卸载器递归删除", "无（[UninstallDelete] 不含 filesandordirs {app}）")

# 2026-10-18 实例（第四轮审查 P1-4）：控制通道有 4 MiB 上限，事件通道漏了 ——
# 接收端持续发「没有终止头的头」会让 eventPlainBuf_ 无限增长。
#
# ★ 查 **patches/**（真相源），不查 third_party/ 工作区。
#   原因：CI 检出的是干净子模块（未打补丁），工作区里根本没有那段代码 ——
#   查工作区会在 CI 上误报「没有上限」。补丁才是「用户装到的代码」的依据（C13）。
_patch = read("patches/upstream-airplay2-sender-cpp.patch") or ""
if not _patch:
    fail("事件通道上限", "读不到 patches/upstream-airplay2-sender-cpp.patch")
elif "kMaxEventPlainBuf" in _patch:
    ok("事件通道上限", "有（补丁含 kMaxEventPlainBuf）")
else:
    fail("事件通道上限", "补丁里没有事件通道上限 —— 与 P1-4 同一类问题")


# -------------------------------------------------- ⑬ 静音标记：内存与落盘必须同时清

# 2026-10-19 实例（第六轮复核 P3-B）：清 `g_mutedByUs` 的几处里，
# 有两处只清内存、不清 cfg.localMutedByUs —— 配置文件里留着 true，
# 下次启动会**再取消一次静音**，覆盖用户退出后手动设的静音。
# 这个坑我修了两轮才全改到，所以做成检查。
_ab = read("src/app/airplay_bridge.cpp") or ""
if not _ab:
    fail("静音标记一致性", "读不到 src/app/airplay_bridge.cpp")
else:
    _al = _ab.split(chr(10))
    _bad = []
    for _i, _l in enumerate(_al):
        if "g_mutedByUs.store(false)" in _l:
            _ctx = chr(10).join(_al[_i:_i + 12])
            if "localMutedByUs = false" not in _ctx:
                _bad.append(_i + 1)
    if _bad:
        fail("静音标记一致性",
             "这些行只清内存、没清落盘标记: %s" % _bad)
    else:
        ok("静音标记一致性",
           "每处都同时清了（共 %d 处）"
           % sum(1 for _l in _al if "g_mutedByUs.store(false)" in _l))

# -------------------------------------------------- ⑭ 源码里不许留测试注入标记

# 2026-10-19 实例（第七轮复核后自查）：我做反向测试时会往源码里注入临时改动
# （改优先级、去守卫、改名常量…）。有一次注入后没还原干净，工作区脏着 ——
# 幸好发现得早（那次没提交）。这类残留一旦提交，就是「产品里带着故意写坏的代码」。
#
# 约定：所有临时注入都必须带一个可 grep 的标记，收尾脚本按标记检查。
_INJECT_MARKS = ["TX-VERIFY", "TX-COUNT", "TX-bisect", "BUG-INJECT",
                 "TEMP-REPRO", "DISABLED_BY_TEST", "TX-disabled"]
_hits = []
for _dir in ("src", "third_party/airplay2-sender-cpp/src",
             "third_party/airplay2-sender-cpp/test"):
    for _root, _dirs, _files in os.walk(_dir):
        for _f in _files:
            if not _f.endswith((".cpp", ".h")):
                continue
            _full = os.path.join(_root, _f).replace(os.sep, "/")
            try:
                _txt = io.open(_full, encoding="utf-8", errors="replace").read()
            except OSError:
                continue
            for _m in _INJECT_MARKS:
                if _m in _txt:
                    _hits.append("%s: %s" % (_full, _m))
if _hits:
    for _h in _hits[:6]:
        fail("测试注入残留", _h)
else:
    ok("测试注入残留", "无（检查 %d 个标记）" % len(_INJECT_MARKS))

# ---------------------------------------------------------------- 输出

print("")
print("文档声明 vs 代码真值")
print("=" * 46)
n_fail = 0
for status, name, detail in results:
    if status == "FAIL":
        n_fail += 1
    print("  [%-4s] %-28s %s" % (status, name, detail))
print("=" * 46)
print("  共 %d 项，%d 项不一致" % (len(results), n_fail))
if n_fail:
    print("")
    print("  修法：改文档去对齐代码，或者改代码后同步文档。")
    print("  如果是有意去掉/改写了某条声明，请同步更新 tools/check_docs.py 的锚点。")
print("")
sys.exit(1 if n_fail else 0)
