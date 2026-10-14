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
    std::string lastDeviceId;      // 上次连接的 deviceid（MAC），优先于 IP
    std::string lastDeviceName;    // 显示名
    std::string lastHost;          // 上次的 IP（deviceid 找不到时的回退）
    int         volume = 45;
    // 端到端延迟（毫秒）。不同网络/设备能承受的最小值不同，
    // 所以做成可调档位，由用户自己找到不爆音的那一档。
    int         latencyMs = 150;
    // 接管模式：记录切到虚拟声卡之前的默认播放设备名。
    // 正常退出时会清空；如果程序崩了，启动时看到它非空就说明
    // 上次是异常退出，需要自动切回去，否则用户会以为电脑没声音了。
    std::string preCaptureDevice;
    bool        autoReconnect = true;
    bool        hideAppleTV = true;    // 列表里静默 Apple TV，只显示 HomePod
    std::vector<std::string> ignoredDeviceIds;   // 用户手动忽略的设备
};

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
