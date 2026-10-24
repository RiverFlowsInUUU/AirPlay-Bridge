// SPDX-License-Identifier: Apache-2.0
//
// sysvolume.h -- 读写 Windows 系统播放音量（默认输出设备）。
//
// 用途：连上串流时把电脑自己的扬声器**静音**，这样声音只从接收端出；
// 断开时恢复。
//
// ★ 这里操作的是 **SetMute**，不是"音量调到 0"。
//   本头文件原来写着「音量调 0，音频流照常存在」—— 那是设计意图，
//   不是实现。实际一直是 mute，而这句话把风险盖住了：
//   部分声卡在端点静音后会**停止向环回供数**，环回采不到任何东西，
//   表现为周期性断连（P1-4，修了三轮才闭环）。
//   如果当初注释和实现一致，第一轮就会有人问"静音会不会掐掉音源"。
//   （2026-10-23 外部审查 P3，2026-10-24 改正）
//
//   getSystemVolume / setSystemVolume 是**保留但当前无人调用**的接口：
//   当初设想的"调 0"方案需要它们，改用 mute 后就没有调用方了。
//   留着是因为"音量调 0"仍是那个驱动的备选规避手段（见主循环里
//   撤销静音的日志），不是死代码 —— 但也别以为是现役路径。

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

// 输出端点列表，返回 (设备 id, 友好名)。
// 菜单要 id（唯一、可持久化），显示要名字 —— 所以两个都要。
// id 取自 IMMDevice::GetId，形如 "{0.0.0.00000000}.{...}"。
std::vector<std::pair<std::string, std::string>> listRenderDevicesWithId();

} // namespace fxchain
