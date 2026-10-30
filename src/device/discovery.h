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

    // 固件与能力位 —— 只用于诊断显示和日志，**不参与任何协议决策**。
    //
    // 为什么要读：HomePod OS 27 起，接收端的行为（尤其是 27.2 把 Mac 音频
    // 改成「持久连接」之后）跟固件版本强相关。用户报「连上了没声音」时，
    // 第一件要确认的就是「对端是哪一版」—— 而 TXT 里本来就有，不用猜。
    //
    // 空字符串 = 该设备的广播里没有这个字段（老的第三方接收端常见）。
    std::string osvers;      // 固件版本，例如 "27.2"
    std::string srcvers;     // AirPlay 实现版本，例如 "1005.13.2"
    std::string features;    // 能力位掩码，例如 "0x4A7FCA00,0x3C356BD0"
    std::string fex;         // 扩展能力串，例如 "AMp/StBrNTwQoa7YDQ"
    bool        isAirPlay2 = false;   // 是否有 _airplay._tcp 记录（= 支持 AP2）
    bool        isHomePod  = false;   // model 以 AudioAccessory 开头
    bool        isAppleTV  = false;   // model 以 AppleTV 开头
};

// 扫描局域网，timeoutMs 内收集到的设备
std::vector<AirPlayDevice> discoverAirPlayDevices(uint32_t timeoutMs = 2500);

// 只问一台已知 IP 的设备，不做网段扫描。
//
// 用途：程序启动/断线重连时是按 cfg 里记的 IP 直连的（不扫描 —— 扫描要 2.5 秒，
// 主线程停摆会掐断声音）。那条路径拿不到 TXT，固件与能力位就永远是空的，
// 而「关于」和日志正要显示这两项。这里补一次单播查询，只花一次 UDP 往返。
//
// 返回 true 表示问到了（out 有效）。host 为空或没应答返回 false，out 不动。
bool probeAirPlayDevice(const std::string& host, AirPlayDevice& out,
                        uint32_t waitMs = 400);

} // namespace fxchain
