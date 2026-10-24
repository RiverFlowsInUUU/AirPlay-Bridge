// SPDX-License-Identifier: Apache-2.0
//
// capture_watchdog_test -- 采集看门狗的判定链。
//
// 2026-10-19 第七轮复核把「拔出设备 → 触发重连」的端到端回归列为最优先。
// 完整的 fake capture 要给 Session 抽 10 个方法的接口、把 Session 搬出主程序，
// 成本远大于收益；但整条判定链本身是纯状态逻辑，抽成 CaptureWatchdog 后
// 这里把它钉死 —— 包括最容易改坏的两点：
//
//   · 只触发一次：不触发 → 永远「已连接但没声音」（原来的 bug）
//                 触发多次 → 每个 pump 都断开，反复闪断
//   · 运行中失效优先：拔设备时该去插回去，不是去查驱动
//
// 复核要求覆盖的五种情形都在下面。

#include "capture_watchdog.h"

#include <cstdio>
#include <string>

using namespace fxchain;

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) {
        ++g_fail;
        std::printf("      FAIL  %s\n", what);
    }
}

}  // namespace

int main() {
    std::printf("\n1. 一切正常：不动作\n");
    {
        CaptureWatchdog w;
        const auto d = w.observe(0, 0);
        check(d.failure == CaptureFailure::None && !d.act, "无失败 -> 不动作");
        check(!w.handled(), "无失败不算处理过");
    }

    std::printf("\n2. ★ 启动失败：动作一次，且只一次\n");
    {
        CaptureWatchdog w;
        const auto d1 = w.observe(1, 0);
        check(d1.failure == CaptureFailure::AtStart && d1.act, "首次 -> 动作");
        // 这就是原 bug 的位置：不动作 = 永远「已连接但没声音」。
        for (int i = 0; i < 5; ++i) {
            const auto dn = w.observe(1, 0);
            check(!dn.act, "后续 -> 不再动作（否则每轮都断开）");
        }
        check(w.handled(), "已标记处理过");
    }

    std::printf("\n3. ★ 运行中失效：动作一次，分类正确\n");
    {
        CaptureWatchdog w;
        const auto d1 = w.observe(0, 2);
        check(d1.failure == CaptureFailure::Runtime && d1.act,
              "首次 -> 动作，且报「运行中失效」");
        for (int i = 0; i < 5; ++i)
            check(!w.observe(0, 2).act, "后续 -> 不再动作");
    }

    std::printf("\n4. ★ 两者都有：运行中优先，且只动作一次\n");
    {
        CaptureWatchdog w;
        const auto d = w.observe(3, 1);
        check(d.failure == CaptureFailure::Runtime,
              "启动 3 路 + 运行中 1 路 -> 报运行中失效（用户该去插回去）");
        check(d.act, "动作");
        check(!w.observe(3, 1).act, "只动作一次");
    }

    std::printf("\n5. ★ 新会话复位后可以再次触发\n");
    {
        CaptureWatchdog w;
        check(w.observe(1, 0).act, "第一轮：动作");
        check(!w.observe(1, 0).act, "第一轮：不重复");
        w.reset();                       // 会话重建
        // ★ 复位必须清掉 handled —— 否则新会话永远不会再触发。
        //   原来那句写成 `!w.handled() || true` —— 恒真，什么都没断言；
        //   断言要放在 observe 之前（act 一旦触发 handled 就又为 true 了）。
        //   （2026-10-24 二次全面复审 T1）
        check(!w.handled(), "复位后 handled 已清");
        check(w.observe(1, 0).act, "复位后：可以再次动作");
    }

    std::printf("\n6. 先正常后失效：中途失效也要能被发现\n");
    {
        CaptureWatchdog w;
        // 模拟 pump 一路跑：前几轮正常
        for (int i = 0; i < 10; ++i)
            check(!w.observe(0, 0).act, "正常轮 -> 不动作");
        // 第 N 轮拔了设备
        check(w.observe(0, 1).act, "中途失效 -> 动作");
        check(!w.observe(0, 1).act, "之后 -> 不重复");
    }

    std::printf("\n7. 触发后计数继续涨：仍不重复动作\n");
    {
        CaptureWatchdog w;
        check(w.observe(1, 0).act, "首次动作");
        check(!w.observe(5, 0).act, "启动失败数涨到 5 -> 不重复");
        check(!w.observe(5, 9).act, "两类都涨 -> 仍不重复");
    }

    std::printf("\n8. ★ 负计数不触发（不该凭空重连）\n");
    {
        CaptureWatchdog w;
        check(!w.observe(-1, 0).act, "启动计数为负 -> 不动作");
        check(!w.observe(0, -1).act, "运行计数为负 -> 不动作");
        check(!w.handled(), "负数不算处理过");
    }

    std::printf("\n");
    if (g_fail) {
        std::printf("   %d/%d 项失败\n", g_fail, g_checks);
        return 1;
    }
    std::printf("   全部通过（%d 项）\n", g_checks);
    return 0;
}
