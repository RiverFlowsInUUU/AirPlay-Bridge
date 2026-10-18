// SPDX-License-Identifier: Apache-2.0
//
// 界面文案的多语言支持 —— 实现见 i18n.h 顶部的设计说明。
//
// 词条表说明
// ----------
// key   = 中文原文（同时也是中文界面直接使用的文本）
// value = 英文
//
// 新增界面文案时：先在代码里写中文，再回这里补一条英文。
// 忘了补也不会出事 —— tr() 查不到就原样返回中文，界面不会空。

#include "i18n.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

namespace i18n {
namespace {

struct Entry {
    const char* zh;
    const char* en;
};

// ─────────────────────────────────────────────────────────────
// 词条表
// ─────────────────────────────────────────────────────────────
const Entry kEntries[] = {
    // ── 托盘菜单：连接 / 状态 ──
    {"断开", "Disconnect"},
    {"连接", "Connect"},
    {"连接中...", "Connecting..."},
    {"已连接", "Connected"},
    {"未连接", "Not connected"},
    {"尚未扫描到设备", "No devices found yet"},
    {"重新扫描", "Scan again"},

    // ── 托盘菜单：设备 ──
    {"设备", "Devices"},
    {"隐藏设备", "Hide devices"},
    {"采集设备", "Capture from"},
    {"跟随系统默认", "Follow system default"},
    {"(未命名设备)", "(unnamed device)"},

    // ── 托盘菜单：音频参数 ──
    {"延迟", "Latency"},
    {"音量", "Volume"},
    {"音质", "Quality"},
    {"自定义（滑块）...", "Custom (slider)..."},
    {"静音", "Mute"},

    // ── 托盘菜单：更多设置 ──
    {"更多设置", "More settings"},
    {"连接诊断", "Connection check"},
    {"无声音时待机", "Standby when silent"},
    {"关闭（不自动待机）", "Off (never standby)"},
    {"静音本机输出", "Mute this PC"},
    {"开机自启", "Run at startup"},
    {"日志", "Logs"},
    {"语言", "Language"},

    // ── 托盘菜单：其它 ──
    {"关于", "About"},
    {"退出", "Quit"},

    // ── 延迟档位 ──
    {"130ms（最低）", "130 ms (lowest)"},
    {"150ms（低延迟）", "150 ms (low latency)"},
    {"200ms（平衡）", "200 ms (balanced)"},
    {"250ms（推荐）", "250 ms (recommended)"},
    {"300ms（更稳）", "300 ms (most stable)"},

    // ── 音质档位 ──
    {"快速", "Fast"},
    {"标准", "Standard"},
    {"高", "High"},
    {"极高", "Very high"},
    {"至臻", "Maximum"},

    // ── 待机时长 ──
    {"关闭", "Off"},

    // ── 托盘图标 tooltip ──
    {"AirPlay Bridge - 已连接", "AirPlay Bridge - Connected"},
    {"AirPlay Bridge - 未连接", "AirPlay Bridge - Not connected"},
    {"AirPlay Bridge - 正在连接", "AirPlay Bridge - Connecting"},
    {"AirPlay Bridge - 扫描设备", "AirPlay Bridge - Scanning"},
    {"AirPlay Bridge - 连接失败", "AirPlay Bridge - Connection failed"},
    {"AirPlay Bridge - 已断开（手动）", "AirPlay Bridge - Disconnected (manual)"},
    {"AirPlay Bridge - 未找到设备", "AirPlay Bridge - No devices found"},
    {" 台设备", " devices"},
    {"AirPlay Bridge - 扫描中", "AirPlay Bridge - Scanning"},
    {"AirPlay Bridge - 启动失败", "AirPlay Bridge - Startup failed"},
    {"AirPlay Bridge - 找到 ", "AirPlay Bridge - Found "},
    {"AirPlay Bridge ", "AirPlay Bridge "},
    {"AirPlay Bridge（没有虚拟声卡）", "AirPlay Bridge (no virtual audio device)"},
    // ── 状态文案（tooltip 与菜单共用）──
    // 2026-10-18：这几条原来只存在于代码里的裸中文字面量，
    // 于是英文界面的 tooltip 一直是中文（P2-3，外部审查发现）。
    {"连接断开", "Disconnected"},
    {"连接失败: ", "Connection failed: "},
    {"（若持续失败请检查防火墙入站规则）",
     " (if it keeps failing, check the firewall inbound rule)"},
    {"已连接（未装虚拟声卡：电脑的输出设备也会播放）",
     "Connected (no virtual audio device - this PC's output still plays)"},
    {"未能连接（未装虚拟声卡：电脑的输出设备也会播放）",
     "Not connected (no virtual audio device - this PC's output still plays)"},

    // ── 滑块窗口标题 ──
    {"AirPlay Bridge — 延迟调节", "AirPlay Bridge - Latency"},
    {"AirPlay Bridge — 音量调节", "AirPlay Bridge - Volume"},
    {"AirPlay Bridge — 音质调节", "AirPlay Bridge - Quality"},
    {"AirPlay Bridge — 无声音时待机", "AirPlay Bridge - Standby when silent"},

    // ── 「关于」弹窗（「关于」本身在菜单那节已有一条）──
    {"版本", "Version"},
    {"项目主页：", "Project home: "},

    // ── 首次运行：选语言 ──
    {"选择语言", "Choose language"},
    {"选择界面语言，之后可以在托盘菜单的「更多设置 › 语言」里随时更改。",
     "Choose the interface language. You can change it later under "
     "\"More settings > Language\" in the tray menu."},

    // ── 首次运行：开机自启 ──
    {"要开机自动启动吗？", "Start automatically at login?"},
    {"是，开机自动启动", "Yes, start at login"},
    {"不用", "No"},
    {"以后可以在托盘菜单的「更多设置 › 开机自启」里改。",
     "You can change this later under \"More settings > Run at startup\"."},

    // ── 连接诊断 ──
    {"需要的网络放行还没配好", "Network access is not allowed yet"},
    {"点「是」帮你配好（会弹一次管理员确认）",
     "Click Yes to fix it (you'll see one admin prompt)"},
    {"已修复，程序正在重连。", "Fixed. Reconnecting..."},
    {"若仍连不上，请断开后重新连接一次（或重启程序）。",
     "If it still won't connect, disconnect and connect again (or restart)."},
    {"修复失败：无法添加网络放行规则。", "Failed: could not add the network rule."},
    {"请确认在弹出的用户账户控制窗口中选择了「是」。",
     "Make sure you clicked Yes in the User Account Control prompt."},

    // ── 更新 ──
    {"发现新版本", "New version available"},
    {"是否前往下载？", "Open the download page?"},

    // ── 气泡提示（带数值的拆成前后缀两条）──
    {"已连接: ", "Connected: "},
    {"启动失败: ", "Failed to start: "},
    {"没找到 AirPlay 设备。请确认音箱已通电，并且和这台电脑在同一个网络下。",
     "No AirPlay device found. Make sure the speaker is powered on and on the same network as this PC."},
    {"延迟已设为 ", "Latency set to "},
    {"，正在重连…", " - reconnecting..."},
    {"音质已调整，正在重连…", "Quality changed - reconnecting..."},
    {"正在扫描局域网设备…", "Scanning the local network..."},

    // ── 弹窗正文（启动检查 / 单实例 / 关于）──
    {"已有 AirPlay Bridge 在运行。", "AirPlay Bridge is already running."},
    {"同时运行两个会导致声音反复中断，请先关闭另一个再启动本程序。",
     "Running two at once makes the sound drop repeatedly. Close the other one first."},
    {"AirPlay Bridge 还没有被防火墙放行。",
     "AirPlay Bridge is not allowed through the firewall yet."},
    {"这会让声音连不上或频繁中断。", "Sound may fail to connect or keep dropping."},
    {"本机还有另一份 AirPlay Bridge，占着网络放行设置：",
     "Another copy of AirPlay Bridge on this PC holds the firewall rule:"},
    {"两个文件不能同时用（放行设置只能指向一个），",
     "Both can't work at once (the rule can only point at one),"},
    {"而且会互相顶掉，所以每次打开都提示。",
     "and they override each other - that's why this keeps appearing."},
    {"请删掉上面那个不用的，再用你常用的这个打开。",
     "Delete the one you don't use, then open the one you do."},
    {"检测到 ", "Found "},
    {" 条多余的网络放行设置（重复添加，或指向已不存在或已更换位置的文件）。",
     " leftover firewall rules (duplicated, or pointing at files that no longer exist or have moved)."},
    {"它们不影响使用，但会让系统里的设置变乱。",
     "They don't break anything, but they clutter your system settings."},
    {"点「是」立即修复（需要管理员权限）。",
     "Click Yes to fix it now (needs admin)."},
    {"也可以稍后从托盘菜单点「更多设置 › 连接诊断」。",
     "You can also do this later from More settings > Connection check."},
    {"点「是」清理，点「否」跳过。", "Click Yes to clean up, No to skip."},

    // ── 防火墙修复/清理的弹窗 ──
    {"已添加防火墙入站规则，允许接收端回连本机做时钟同步。",
     "Added an inbound firewall rule so the receiver can call back for clock sync."},
    {"修复失败：无法添加防火墙入站规则。",
     "Failed: could not add the inbound firewall rule."},
    {"已清理 ", "Cleaned up "},
    {" 条旧的网络放行设置。", " old firewall rules."},
    {"它们指向的文件已经不存在了，清理不影响正在使用的放行设置。",
     "They pointed at files that no longer exist; nothing in use was affected."},
    {"没有可清理的旧设置，或清理未能完成。",
     "Nothing to clean up, or the cleanup didn't complete."},

    // ── 待机时长档位 ──
    {"5 秒", "5 s"},
    {"10 秒", "10 s"},
    {"15 秒", "15 s"},
    {"20 秒", "20 s"},

    // ── 滑块窗口：按钮 / 单位 ──
    {"取消", "Cancel"},
    {"应用", "Apply"},
    // ── 菜单状态行与父项标题（带尾随空格的按原样收录）──
    {"音质  ", "Quality  "},
    {"无声音时待机  ", "Standby when silent  "},
    {"无声音时待机  关闭", "Standby when silent  Off"},
    {"（直通）", "(passthrough)"},
    {"（本机重采样，品质 ", "(resampled on this PC, quality "},
    {"）", ")"},

    {"秒", "s"},
    {" 秒", " s"},

    // ── 滑块窗口：档位评价 ──
    {"可能爆音", "May crackle"},
    {"偶有爆音", "Occasional crackle"},
    {"稳定", "Stable"},
    {"接近下限", "Near the limit"},
    {"推荐", "Recommended"},
    {"更稳（延迟略高）", "Smoothest (slightly more delay)"},
    {"很轻", "Very quiet"},
    {"适中", "Moderate"},
    {"较响", "Loud"},
    {"很响", "Very loud"},
    {"偏激进", "Aggressive"},
    {"较保守", "Conservative"},
    {"最保守", "Most conservative"},

    // ── 滑块窗口：说明文字 ──
    {"拖到不爆音的最低位置就是最优解。低于 150ms 可能出现爆音，越低延迟越小，但接收端缓冲不足会爆音或卡顿。",
     "Drag to the lowest position that doesn't crackle. Below 250 ms the receiver's buffer can run dry."},
    {"声卡通常跑 48kHz，推给设备前要先转到 44.1kHz。越往右高频越干净，CPU 也越贵。",
     "Sound cards usually run at 48 kHz and must be converted to 44.1 kHz. Further right is cleaner but costs more CPU."},
    {"调节设备的播放音量，不影响电脑本身的音量。",
     "Adjusts the playback volume on the device, not on this PC."},
    {"连续静音这么久之后断开与设备的连接，让屏幕熄灭；再有任何声音立刻重连。档位太短会在换歌、安静段落里误断，太长则熄屏迟钝。拖到最左边（0 秒）表示关掉这个功能。",
     "Disconnects from the device after this long without sound so its screen turns off; any sound reconnects it. Too short drops during quiet passages, too long is slow to dim. Far left (0 s) turns this off."},
};

constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

Lang g_lang = Lang::En;

int Utf8ToWide(const char* s, std::wstring& out) {
    if (!s) return 0;
    const int need = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (need <= 1) { out.clear(); return 0; }
    // need 含结尾 NUL 且函数会写入 need 个 wchar_t —— 必须按 need 分配。
    // tr()/trw() 每次都走这里，按 need-1 分配等于每次都越界写 2 字节，
    // 破坏相邻的堆对象（v1.3.1 修过同类 4 处，这处漏了）。
    out.resize(static_cast<size_t>(need));
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), need);
    out.resize(static_cast<size_t>(need - 1));   // 去掉结尾 NUL
    return need - 1;
}

}  // namespace

Lang current() { return g_lang; }

void setLang(Lang lang) { g_lang = lang; }

Lang fromConfigValue(const std::string& v) {
    // 只认 "zh"，其余一律英文 —— 配置被手改成奇怪的值时退回默认，不报错。
    if (v == "zh") return Lang::Zh;
    return Lang::En;
}

const char* toConfigValue(Lang lang) {
    return lang == Lang::Zh ? "zh" : "en";
}

const char* nativeName(Lang lang) {
    // 用该语言自己的写法 —— 中文用户找「中文」，英文用户找「English」，
    // 都不需要先看懂另一种语言。
    return lang == Lang::Zh ? "中文" : "English";
}

const char* tr(const char* zh) {
    if (!zh) return "";
    if (g_lang == Lang::Zh) return zh;   // 中文原文直接就是中文界面
    for (size_t i = 0; i < kEntryCount; ++i) {
        if (std::strcmp(kEntries[i].zh, zh) == 0) return kEntries[i].en;
    }
    return zh;   // 漏翻：原样返回，界面显示中文而不是空白
}

std::wstring trw(const char* zh) {
    std::wstring w;
    Utf8ToWide(tr(zh), w);
    return w;
}

void coverage(int* translated, int* total) {
    int t = 0;
    for (size_t i = 0; i < kEntryCount; ++i) {
        // 一条英文译文与中文原文不同，就算翻过了
        if (std::strcmp(kEntries[i].zh, kEntries[i].en) != 0) ++t;
    }
    if (translated) *translated = t;
    if (total) *total = static_cast<int>(kEntryCount);
}

}  // namespace i18n
