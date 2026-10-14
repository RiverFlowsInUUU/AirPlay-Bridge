// SPDX-License-Identifier: Apache-2.0
//
// discovery.h -- 通过 mDNS 发现局域网里的 AirPlay 设备。
//
// 做法：向 224.0.0.251:5353 发一条 PTR 查询（_airplay._tcp.local 与
// _raop._tcp.local），收集应答里的 SRV/A/TXT 记录，组装成设备列表。
// 不依赖 Bonjour，只用一个 UDP socket。

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fxchain {

struct AirPlayDevice {
    std::string name;        // 显示名，例如 "客厅的 HomePod mini"
    std::string host;        // IPv4 点分十进制
    uint16_t    port = 7000;
    std::string model;       // TXT 里的 model，例如 "AudioAccessory5,1"
    std::string deviceId;    // TXT 里的 deviceid (MAC)
    bool        isAirPlay2 = false;   // 是否有 _airplay._tcp 记录（= 支持 AP2）
    bool        isHomePod  = false;   // model 以 AudioAccessory 开头
    bool        isAppleTV  = false;   // model 以 AppleTV 开头
};

// 扫描局域网，timeoutMs 内收集到的设备
std::vector<AirPlayDevice> discoverAirPlayDevices(uint32_t timeoutMs = 2500);

} // namespace fxchain
