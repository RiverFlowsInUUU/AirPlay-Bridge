// SPDX-License-Identifier: Apache-2.0
//
// capture_health_test -- 采集失败分类判定。
//
// 2026-10-19 第六轮复核 P2-A 要求「补 fake capture 回归」。完整的 fake capture
// 要给 Session 抽接口、把整个 Session 搬出主程序，成本远大于本轮范围；
// 但「两种失败怎么区分」是纯逻辑，抽出来就能测 —— 而它恰好最容易改坏：
// **优先级错了，用户看到的提示就是错的**（拔了声卡却告诉他「采集启动失败」，
// 他会去查驱动，而真正的动作是插回去）。
//
// 这里把那个优先级钉死。

#include "capture_health.h"

#include <cstdio>
#include <string>

using namespace fxchain;

namespace {

int g_fail = 0;
int g_checks = 0;

void expect(int start, int runtime, CaptureFailure want, const char* why) {
    ++g_checks;
    const CaptureFailure got = captureFailureKind(start, runtime);
    if (got != want) {
        ++g_fail;
        std::printf("      FAIL  %s: startFailures=%d runtimeFailures=%d -> %d，期望 %d\n",
                    why, start, runtime, int(got), int(want));
    }
}

}  // namespace

int main() {
    std::printf("\n1. 无失败\n");
    expect(0, 0, CaptureFailure::None, "两个计数都 0");

    std::printf("\n2. 只有启动失败\n");
    expect(1, 0, CaptureFailure::AtStart, "1 路启动失败");
    expect(3, 0, CaptureFailure::AtStart, "3 路启动失败");

    std::printf("\n3. 只有运行中失效\n");
    expect(0, 1, CaptureFailure::Runtime, "1 路运行中失效");
    expect(0, 2, CaptureFailure::Runtime, "2 路运行中失效");

    std::printf("\n4. ★ 两者都有时运行中优先\n");
    // 这是本函数存在的理由：拔声卡的场景里两类计数可能同时非 0，
    // 必须先报「失效」—— 排查方向完全不同。
    expect(1, 1, CaptureFailure::Runtime, "各 1 路");
    expect(5, 1, CaptureFailure::Runtime, "启动 5 路 + 运行中 1 路");

    std::printf("\n5. 负数（不该出现，但不能静默当成失败）\n");
    // 计数器是 int，理论上不会为负；但如果有人误写，这里的行为要明确：
    // 负数不 > 0，所以按「没失败」处理 —— 不会凭空触发重连。
    expect(-1, 0, CaptureFailure::None, "启动计数为负");
    expect(0, -1, CaptureFailure::None, "运行计数为负");

    std::printf("\n");
    if (g_fail) {
        std::printf("   %d/%d 项失败\n", g_fail, g_checks);
        return 1;
    }
    std::printf("   全部通过（%d 项）\n", g_checks);
    return 0;
}
