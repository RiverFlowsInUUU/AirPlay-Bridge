// SPDX-License-Identifier: Apache-2.0
//
// 托盘图标提示（tooltip）里「已连接」那几行的拼装 —— 纯函数。
//
// 为什么单独抽出来：
//   1. **它能被截断，而且截掉的恰好是最要紧的部分**。szTip 是 128 字符的
//      定长缓冲，单行写「已连接（很长的设备名：192.168.1.50）」时，
//      名字一长 IP 就没了 —— 而 IP 正是用来区分同名设备的那一项。
//      这种错误在短名字的机器上永远看不出来。
//   2. 菜单状态区（airplay_bridge.cpp 里手写的那三行）说的是同一件事，
//      两处必须一致；抽成函数后测试能钉住格式。
//
// 换行：Windows Vista 起 szTip 支持多行（NOTIFYICONDATA 的 szTip，
// 上限 128 个字符含换行）。这里就用换行分行，而不是靠截断去省字符。
#pragma once

#include <string>

namespace fxchain {

// 按 UTF-8 字符边界截断：超过 maxBytes 就退到不切断多字节字符的位置，加 "..."。
//
// ★ 不能直接 s.substr(0, n)。中文一个字占 3 字节，切在第 2 个字节上
//   就把这个字砍成半个，界面显示成「客厅 HomePod�...」。
//   菜单状态区和托盘提示都要做同样的截断，所以抽成公共函数 ——
//   之前只有 tooltip 这条路修了，菜单那条漏着（2026-10-23 外部审查发现）。
inline std::string truncateUtf8(const std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    // UTF-8 的后续字节都是 10xxxxxx；往前退到字符起始字节。
    std::size_t cut = maxBytes;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return s.substr(0, cut) + "...";
}

// 拼「已连接」提示的正文（不含 "AirPlay Bridge - " 前缀，那部分由 onStatus 加）。
//
// 输出形如：
//     已连接
//     客厅 HomePod
//     192.168.1.50
//
// 规则：
//   - 名字取 deviceName，空则退回 host（deviceLabel 的既有约定）
//   - 名字超过 maxBytes 就截断加 "..."，避免提示框撑得过宽
//   - 只有**名字和 IP 都有**时才写第二行 IP —— 名字本身就是退回的 IP 时
//     再写一遍就重复了
//
// ★ 截断按**UTF-8 字符边界**退让，不能直接 substr 字节数 —— 见 truncateUtf8。
//   （2026-10-20 实测发现：22 字节切在「设」字中间。）
inline std::string connectedTipBody(const std::string& deviceName,
                                    const std::string& host,
                                    const char* connectedLabel,
                                    std::size_t maxBytes = 22) {
    const std::string who = deviceName.empty() ? host : deviceName;

    std::string out = connectedLabel ? connectedLabel : "";
    if (who.empty()) return out;

    out += '\n';
    out += truncateUtf8(who, maxBytes);

    // 只有「名字」和「IP」是两个不同的东西时才补 IP 行。
    if (!deviceName.empty() && !host.empty()) {
        out += '\n';
        out += host;
    }
    return out;
}

}  // namespace fxchain
