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
