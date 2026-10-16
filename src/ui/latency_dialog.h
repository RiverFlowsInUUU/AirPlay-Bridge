// SPDX-License-Identifier: Apache-2.0
//
// latency_dialog.h -- 通用数值调节滑块窗口。
//
// 托盘原生菜单放不下滑块控件，所以弹一个独立的小窗口。
// 延迟和音量共用这一个实现，参数通过 SliderDialogSpec 传进去。

#pragma once

#include <string>

namespace fxchain {

struct SliderDialogSpec {
    std::string title;       // 窗口标题，例如 "延迟调节"
    std::string hint;        // 顶部说明文字（可含换行）
    int         minValue = 0;
    int         maxValue = 100;
    int         tickFreq = 10;    // 刻度间隔，0 = 不画刻度
    std::string unit;        // 数值后缀，例如 "ms" 或 "%"
    // 内部单位 -> 显示单位的换算。默认 1/0 表示"内部值就是显示值"，
    // 老的三项（延迟 / 音量 / 音质）不受影响。
    // 待机时长用 divisor=10、decimals=1：内部 75 -> 显示 "7.5 秒"。
    int         displayDivisor  = 1;   // 显示时用它除（内部单位 -> 显示单位）
    int         displayDecimals = 0;   // 显示保留几位小数
    // 拖动时吸附到的步长（内部单位）。默认 1 = 不吸附，
    // 老的三项都是整数区间，行为不变；待机那项传 5（= 0.5 秒）。
    int         step = 1;
    // 根据数值返回一句评价（可空）
    const char* (*grade)(int) = nullptr;
};

// 弹出滑块窗口（模态）。
// 返回用户确认的数值；取消则返回 -1。
int showSliderDialog(const SliderDialogSpec& spec, int currentValue);

// 便捷封装：延迟调节（60-1000ms）
int showLatencyDialog(int currentMs);

// 便捷封装：音量调节（0-100%）
int showVolumeDialog(int currentPct);

// 便捷封装：重采样质量（0-100）
int showQualityDialog(int currentLevel);

// 便捷封装：无声音时待机时长（0-200，单位十分之一秒）。
// 返回新的十分之一秒值；取消返回 -1。
int showStandbyDialog(int currentTenths);

// 音质档位名（关闭 / 标准 / 高 / 极高 / 至臻）。
// 菜单和滑块共用，保证两边显示一致。
const char* qualityLevelName(int level);

} // namespace fxchain
