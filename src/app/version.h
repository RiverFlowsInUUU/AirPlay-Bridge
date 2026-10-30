// SPDX-License-Identifier: Apache-2.0
//
// 版本号比较。
//
// 从 airplay_bridge.cpp 抽出来 —— 它是个**纯函数**（只依赖 <string>/<vector>），
// 但原来埋在主程序里，而主程序带 WinMain、托盘、WASAPI，任何测试都链不进去。
// 抽成一个头文件之后它才第一次有测试（见 version_test.cpp）。
//
// 2026-10-18 外部审查的 P3-3 点名的就是这类：项目里最容易出错、
// 又最容易测的纯函数（版本比较、JSON 取值、mDNS 解析）一个测试都没有。
#pragma once

#include <string>
#include <vector>

namespace fxchain {

// remote 比 local 新则返回 true。
//
// 按 `.` 拆成数字逐段比较。**不能字符串比较** ——
// 那样 "1.2.9" > "1.2.10" 会得到错误结论（'9' > '1'），
// 于是「明明有新版本，程序说已是最新」。
inline bool isNewerVersion(const std::string& remote, const std::string& local) {
    auto parse = [](const std::string& v) {
        std::vector<int> out;
        size_t i = 0;
        // 去掉可能的 v 前缀（GitHub 的 tag 是 v1.5.22 这种）
        size_t start = (i < v.size() && (v[i] == 'v' || v[i] == 'V')) ? 1 : 0;
        int cur = 0;
        bool has = false;
        for (size_t k = start; k < v.size(); ++k) {
            const char c = v[k];
            if (c >= '0' && c <= '9') {
                // 饱和累加：v 来自 GitHub API 的 tag 字符串（外部输入），
                // 超长数字段按普通 int 累加是有符号溢出 UB。
                // 比较语义不变 —— 超过上限后停在 INT_MAX。
                // （2026-10-31 外部审查 P3-21。）
                if (cur > (2147483647 - 9) / 10) cur = 2147483647;
                else cur = cur * 10 + (c - '0');
                has = true;
            } else if (c == '.') {
                out.push_back(has ? cur : 0);
                cur = 0;
                has = false;
            } else {
                break;   // 遇到 -rc / +build 之类就停
            }
        }
        if (has) out.push_back(cur);
        return out;
    };

    const auto a = parse(remote), b = parse(local);
    for (size_t k = 0; k < a.size() || k < b.size(); ++k) {
        const int x = (k < a.size()) ? a[k] : 0;   // 缺的段按 0 算
        const int y = (k < b.size()) ? b[k] : 0;
        if (x != y) return x > y;
    }
    return false;
}

}  // namespace fxchain
