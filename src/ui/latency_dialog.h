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

} // namespace fxchain
