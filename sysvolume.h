// SPDX-License-Identifier: Apache-2.0
//
// sysvolume.h -- 读写 Windows 系统播放音量（默认输出设备）。
//
// 用途：启动串流时把系统音量调 0，这样声音只从 HomePod 出；
// 退出时恢复原值。
//
// 注意：这里操作的是"音量"，不是"静音"。
// 静音会让系统停止产生音频流，环回采集就什么都抓不到；
// 音量调 0 只是把增益降到最小，音频流照常存在。

#pragma once

#include <string>
#include <vector>

namespace fxchain {

// 读当前系统音量（0.0 - 1.0）。失败返回 -1。
float getSystemVolume();

// 设置系统音量（0.0 - 1.0）。成功返回 true。
bool setSystemVolume(float v);

// 是否处于静音状态
bool isSystemMuted();

// 设置静音
bool setSystemMuted(bool muted);

// ---- 默认播放设备切换 ----

// 取当前默认播放设备的 ID（设备名中的 GUID 部分）。失败返回空串。
std::string getDefaultRenderDeviceId();

// 取当前默认播放设备的可读名字
std::string getDefaultRenderDeviceName();

// 按设备名子串切换默认播放设备（三个角色一起切）。
// 返回 true 表示成功。
bool setDefaultRenderDevice(const std::string& nameSubstring);

// 列出所有活动输出设备名
std::vector<std::string> listRenderDevices();

} // namespace fxchain
