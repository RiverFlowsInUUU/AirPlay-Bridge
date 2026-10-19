// SPDX-License-Identifier: Apache-2.0
//
// 采集健康状态的判定 —— 从 airplay_bridge.cpp 里抽出来的**纯函数**。
//
// 为什么值得单独抽出来测（2026-10-19 第六轮复核 P2-A 建议补回归）：
// 完整的 fake capture 需要给 Session 抽出接口、搬出主程序，成本很高；
// 但「两种失败怎么区分」这部分是纯逻辑，抽出来就能测，
// 而且它恰好是最容易被改坏的地方 —— **优先级判断错了，用户看到的
// 提示就是错的**（拔了声卡却告诉他「采集启动失败」）。
#pragma once

namespace fxchain {

enum class CaptureFailure {
    None,      // 没失败
    AtStart,   // Start() 就失败：设备一开始没打开（缺驱动 / 被独占）
    Runtime,   // 播放中失效：拔声卡 / 端点被禁用 / 驱动重置
};

// 判定采集当前的失败类型。
//
// ★ 优先级：**运行中失效优先**。
//   拔声卡时两类计数可能同时非 0（先 Start 成功，之后失效），
//   这时必须报「设备已失效」—— 否则用户看到「启动失败」会去查驱动，
//   而真正的动作是「插回去 / 换一个输出设备」。
inline CaptureFailure captureFailureKind(int startFailures, int runtimeFailures) {
    if (runtimeFailures > 0) return CaptureFailure::Runtime;
    if (startFailures > 0) return CaptureFailure::AtStart;
    return CaptureFailure::None;
}

}  // namespace fxchain
