// SPDX-License-Identifier: Apache-2.0
//
// config_store.h -- 极简 JSON 配置读写（记忆上次选择的设备、音量、黑名单）。
//
// 刻意不引第三方 JSON 库：只处理我们这个固定结构，手写解析足够且没有依赖。

#pragma once

#include <string>
#include <vector>

namespace fxchain {

struct BridgeConfig {
    // 本机自己的设备标识（形如 02:AB:CD:EF:01:23）。首次运行生成后固定。
    // 每台机器必须不同 —— 写死的话接收端会把所有装了本程序的电脑
    // 当成同一台设备，两台同时连会互相顶掉。
    std::string deviceId;          // 本机自身标识，每台唯一，首次运行生成
    std::string lastDeviceId;      // 上次连接的 deviceid（MAC），优先于 IP
    std::string lastDeviceName;    // 显示名
    std::string lastHost;          // 上次的 IP（deviceid 找不到时的回退）
    int         volume = 45;
    // 端到端延迟（毫秒）。不同网络/设备能承受的最小值不同，
    // 所以做成可调档位，由用户自己找到不爆音的那一档。
    int         latencyMs = 150;
    // 重采样品质 0-100（0=关，1-100 连续映射到抽头数 4..32）。
    // 采集端的设备通常是 48kHz，而 AirPlay 流固定 44.1kHz ——
    // 这个值决定怎么转。默认 100（最高）。
    //
    // 注意：老版本用的是 0-3 的 quality 字段，加载时会自动换算。
    int         qualityLevel = 100;
    // 接管模式：记录切到虚拟声卡之前的默认播放设备名。
    // 正常退出时会清空；如果程序崩了，启动时看到它非空就说明
    // 上次是异常退出，需要自动切回去，否则用户会以为电脑没声音了。
    std::string preCaptureDevice;
    bool        autoReconnect = true;
    bool        hideAppleTV = true;    // 列表里静默 Apple TV，只显示 HomePod
    // 静音自动待机：连续无声音若干秒就断开与接收端的连接，
    // 让 HomePod 屏幕熄灭；一旦再听到声音立刻重连。
    bool        standbyEnabled = true;
    // 静音判定时长，单位十分之一秒（50 = 5.0 秒）。
    // 下限 50：太短的档位（0/1/3 秒）会在换歌、安静段落误断，实测体验很差，
    // 所以整档砍掉，范围固定在 5.0-10.0 秒之间细调。
    // 静音多久之后断开。单位十分之一秒；0 = 不做静音待机。
    int         standbyTenths = 50;    // 默认 5 秒
    std::vector<std::string> ignoredDeviceIds;   // 用户手动忽略的设备
    // 首次运行引导是否已经做过。
    // 只做一次 —— 每次启动都问用户"要不要开机自启"是骚扰。
    bool        askedAutoStart = false;
};

// 静音待机是否真的启用。
//
// 两个条件缺一不可：开关打开，且时长大于 0。
//
// **时长为 0 时绝不能参与「静音超过 N 秒」的判断** —— 那个条件恒成立，
// 连上就立刻断，然后在待机/唤醒之间反复振荡。
// 所以 0 在语义上就是「关闭」，菜单里显示成关闭、和「关闭」那一项共用一个勾。
inline bool standbyActiveConfig(const BridgeConfig& c) {
    return c.standbyEnabled && c.standbyTenths > 0;
}

// 运行时数据目录（%LOCALAPPDATA%\\AirPlayBridge\\），不存在则创建。
std::string appDataDir();

// 默认配置路径
std::string defaultConfigPath();

bool loadConfig(const std::string& path, BridgeConfig& out);
bool saveConfig(const std::string& path, const BridgeConfig& cfg);

// 供外部复用的最小 JSON 取值（很笨但够用）
std::string jsonGetString(const std::string& json, const std::string& key);
long long   jsonGetInt(const std::string& json, const std::string& key, long long def);
bool        jsonGetBool(const std::string& json, const std::string& key, bool def);

} // namespace fxchain
