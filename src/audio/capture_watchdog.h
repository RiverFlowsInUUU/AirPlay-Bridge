// SPDX-License-Identifier: Apache-2.0
//
// 采集看门狗 —— 把「什么时候该断开重连」从 Session 里抽出来的**可测状态机**。
//
// 为什么这么抽（2026-10-19 第七轮复核，把「P2-A 端到端回归」列为最优先）：
//   完整的 fake capture 要给 Session 抽 10 个方法的接口、还得把整个 Session
//   搬出主程序才能测 —— 成本远大于收益。
//   但「看到失败计数 → 决定断线 → 只触发一次」这段是纯状态逻辑，
//   抽出来就能把**整条判定链**钉住，包括最容易改坏的两点：
//     · 运行中失效优先于启动失败（用户提示会不会说错）
//     · 只触发一次（不触发 → 永远「已连接但没声音」；触发多次 → 反复闪断）
//
// Session 里那几行现在只负责「照它的结论做动作」。
#pragma once

#include "capture_health.h"

namespace fxchain {

// 看门狗一次心跳的结论。
struct CaptureWatchdogDecision {
    CaptureFailure failure = CaptureFailure::None;  // 判定出的失败类型
    bool           act     = false;                 // 是否应当断开重连
};

// 采集看门狗。
//
// 用法：每次主循环调 observe()，按返回的 act 决定要不要断开。
class CaptureWatchdog {
public:
    // 新会话开始时调：清掉「已经处理过」的记忆。
    void reset() { handled_ = false; }

    // 观察一次采集状态。
    //
    // 语义：
    //   · 已经处理过就不再重复触发（避免每轮 pump 都断开一次）
    //   · 只报 Runtime 或 AtStart；两者都有时报 Runtime（拔设备时用户
    //     该去插回去，而不是去查驱动）
    CaptureWatchdogDecision observe(int startFailures, int runtimeFailures) {
        CaptureWatchdogDecision d;
        d.failure = captureFailureKind(startFailures, runtimeFailures);
        if (d.failure == CaptureFailure::None) return d;
        if (handled_) {
            // 已经触发过一次：类型照报（供日志/状态用），但不再动作。
            d.act = false;
            return d;
        }
        handled_ = true;
        d.act = true;
        return d;
    }

    bool handled() const { return handled_; }

private:
    bool handled_ = false;
};

}  // namespace fxchain
