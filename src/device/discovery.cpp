// SPDX-License-Identifier: Apache-2.0
//
// discovery.cpp -- 发现局域网里的 AirPlay 设备。
//
// 重要：很多 Windows WiFi 网卡/路由器会拦掉 mDNS 组播（224.0.0.251），
// 但单播 mDNS 查询是通的（实测 HomePod 会正常应答）。
// 所以这里的策略是：
//   1. 扫本网段，找出 7000 端口开着的 IP（AirPlay 的标志性端口）
//   2. 对每个候选 IP 发**单播** mDNS 查询，拿完整设备信息
//
// 不依赖 Bonjour，也不需要组播。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "discovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace fxchain {
namespace {

// ---- DNS 报文工具 ----

void put16b(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x & 0xFF));
}

void putName(std::vector<uint8_t>& v, const std::string& name) {
    size_t i = 0;
    while (i < name.size()) {
        size_t dot = name.find('.', i);
        if (dot == std::string::npos) dot = name.size();
        size_t len = dot - i;
        if (len > 63) len = 63;
        v.push_back(static_cast<uint8_t>(len));
        for (size_t k = 0; k < len; ++k) v.push_back(static_cast<uint8_t>(name[i + k]));
        i = dot + 1;
    }
    v.push_back(0);
}

uint16_t r16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }

std::string readName(const uint8_t* buf, size_t len, size_t& pos, int depth = 0) {
    std::string out;
    if (depth > 8) return out;
    size_t p = pos;
    bool jumped = false;
    size_t after = pos;

    while (p < len) {
        uint8_t l = buf[p];
        if (l == 0) { ++p; if (!jumped) after = p; break; }
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= len) break;
            size_t off = ((l & 0x3F) << 8) | buf[p + 1];
            if (!jumped) { after = p + 2; jumped = true; }
            if (off >= len) break;
            std::string tail = readName(buf, len, off, depth + 1);
            if (!out.empty() && !tail.empty()) out += '.';
            out += tail;
            p += 2;
            break;
        }
        ++p;
        if (p + l > len) break;
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(buf + p), l);
        p += l;
    }
    pos = jumped ? after : p;
    return out;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string txtGet(const std::vector<uint8_t>& txt, const std::string& key) {
    size_t i = 0;
    while (i < txt.size()) {
        uint8_t l = txt[i++];
        if (i + l > txt.size()) break;
        std::string s(reinterpret_cast<const char*>(txt.data() + i), l);
        i += l;
        auto eq = s.find('=');
        if (eq != std::string::npos && toLower(s.substr(0, eq)) == key)
            return s.substr(eq + 1);
    }
    return {};
}

// ---- 单播 mDNS 查询：向一个 IP 问 _airplay._tcp.local ----
bool queryOne(SOCKET s, const std::string& ip, AirPlayDevice& out, uint32_t waitMs) {
    std::vector<uint8_t> pkt;
    put16b(pkt, 0);            // ID（单播查询用 0 也行）
    put16b(pkt, 0x0000);       // flags
    put16b(pkt, 1);            // QDCOUNT
    put16b(pkt, 0); put16b(pkt, 0); put16b(pkt, 0);

    putName(pkt, "_airplay._tcp.local");
    put16b(pkt, 12);           // PTR
    put16b(pkt, 1);            // IN

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(5353);
    dst.sin_addr.s_addr = inet_addr(ip.c_str());

    if (sendto(s, reinterpret_cast<const char*>(pkt.data()),
               static_cast<int>(pkt.size()), 0,
               reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) == SOCKET_ERROR)
        return false;

    // 收集一段时间的应答，直到拿到 SRV（含端口）和 TXT（含 model）
    bool gotSrv = false, gotTxt = false, gotInstance = false;
    std::string instance;
    // GetTickCount() 是 32 位毫秒、约 49.7 天回绕 —— 回绕瞬间
    // `GetTickCount() + waitMs` 溢出成小值，循环一次都不执行，静默返回空结果。
    // 托盘程序「开机数周不重启」是常态，所以用 64 位版本。
    const ULONGLONG deadline = GetTickCount64() + waitMs;

    while (GetTickCount64() < deadline) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{0, static_cast<long>(waitMs > 200 ? 200 : waitMs) * 1000};
        if (select(0, &rd, nullptr, nullptr, &tv) <= 0) {
            if (gotSrv && gotTxt) break;
            continue;
        }

        uint8_t buf[8192];
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int got = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                           reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got <= 12) continue;

        // ★ 必须核对来源 IP —— 只收我们正在问的那台的应答。
        //
        //   原来 `from` 取出来就扔了，于是任何迟到的应答都会被当成
        //   当前这台设备的答案：循环按 IP 一台台问，上一台的回包晚到
        //   几百毫秒，就落进了下一台的窗口 —— 菜单里出现
        //   「HomePod 的名字配着别的机器的 IP」，点连接必然失败。
        //   （2026-10-23 外部审查 P2-6，2026-10-24 修）
        //
        //   同一台设备有多个 IP（有线 + WiFi）的重复条目问题不在这里：
        //   那要靠"按 deviceId 去重"解决，是另一件事，见 filterDevices。
        if (from.sin_addr.s_addr != dst.sin_addr.s_addr) {
            continue;
        }

        size_t pos = 12;
        uint16_t qd = r16(buf + 4);
        uint16_t an = r16(buf + 6);
        uint16_t ns = r16(buf + 8);
        uint16_t ar = r16(buf + 10);

        for (uint16_t i = 0; i < qd; ++i) { readName(buf, got, pos); pos += 4; }

        const size_t total = static_cast<size_t>(an) + ns + ar;
        for (size_t i = 0; i < total && pos + 10 <= static_cast<size_t>(got); ++i) {
            std::string rname = readName(buf, got, pos);
            if (pos + 10 > static_cast<size_t>(got)) break;
            uint16_t rtype = r16(buf + pos);
            uint16_t rdlen = r16(buf + pos + 8);
            pos += 10;
            if (pos + rdlen > static_cast<size_t>(got)) break;

            const uint8_t* rd = buf + pos;
            pos += rdlen;

            std::string lname = toLower(rname);

            if (rtype == 12) {                       // PTR -> 实例名
                size_t p = 0;
                std::string target = readName(rd, rdlen, p);
                if (!target.empty() && instance.empty()) {
                    instance = target;
                    gotInstance = true;
                    out.name = target;

                    // ★ DNS 名字**大小写不敏感**，设备回什么写法都可能：
                    //   "_AirPlay._TCP.local"（苹果设备常见）、"_airplay._tcp.local"、
                    //   以及各种混写。原来只 find 小写形式，剥不掉尾巴 ——
                    //   菜单里就是一台叫「客厅 HomePod._AirPlay._TCP.local」的设备。
                    //   （2026-10-23 外部审查 P3，2026-10-24 修）
                    std::string lower = out.name;
                    for (char& c : lower) {
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    }
                    auto dot = lower.find("._airplay._tcp.local");
                    if (dot == std::string::npos)
                        dot = lower.find("._raop._tcp.local");
                    // 用 lower 里找到的位置去切原串：两者等长、逐字节对齐。
                    if (dot != std::string::npos) out.name = out.name.substr(0, dot);
                    out.isAirPlay2 = true;
                }
            } else if (rtype == 33 && rdlen >= 6) {  // SRV -> 端口
                out.port = r16(rd + 4);
                gotSrv = true;
            } else if (rtype == 16) {                // TXT -> model / deviceid
                std::vector<uint8_t> txt(rd, rd + rdlen);
                std::string model = txtGet(txt, "model");
                std::string devid = txtGet(txt, "deviceid");
                // 只接受真正的型号标识，忽略 _device-info 里的 BoardID（如 B520AP）
                if (model.rfind("AudioAccessory", 0) == 0 ||
                    model.rfind("AppleTV", 0) == 0) {
                    out.model = model;
                }
                if (!devid.empty()) out.deviceId = devid;
                gotTxt = true;
            }
        }

        if (gotSrv && gotTxt) break;
    }

    out.host = ip;
    if (out.port == 0) out.port = 7000;
    out.isHomePod = out.model.rfind("AudioAccessory", 0) == 0;
    out.isAppleTV = out.model.rfind("AppleTV", 0) == 0;
    return gotInstance || gotSrv || gotTxt;
}

// 拿本机一个非回环 IPv4 网段。
//
// ★ 为什么仍然是 /24，而不是适配器上报的真实前缀长度（2026-10-18 审查 P3-4）：
//   在 /16 网段上按真实前缀扫一遍 = 65534 个地址 x 7000 端口，
//   而设备发现的预算是 2.5 秒。改成真实前缀会直接把「发现设备」变成
//   「卡几分钟」—— 那比漏掉设备更糟。
//   同网段的家用/办公网络几乎全是 /24；真遇到 /16，用户可手动指定设备 IP。
//
//   但审查的另一半是对的：**多网卡时只取第一个可能选错网段**。
//   所以这里加一层偏好：优先选「有默认网关」的那块网卡（通常就是上网
//   用的那块），而不是枚举顺序里的第一个（可能是虚拟网卡 / VPN）。
bool localSubnet(uint32_t& base, uint32_t& mask) {
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &size) !=
        ERROR_BUFFER_OVERFLOW)
        return false;

    std::vector<uint8_t> buf(size);
    auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, addrs, &size) != NO_ERROR)
        return false;

    // 两轮：第一轮只接受「有默认网关」的网卡，找不到再退回任意一块。
    for (int pass = 0; pass < 2; ++pass) {
        for (auto* a = addrs; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp) continue;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            if (pass == 0 && !a->FirstGatewayAddress) continue;   // 没网关 = 大概不是上网那块
            for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                if (!sa || sa->sin_family != AF_INET) continue;
                uint32_t ip = ntohl(sa->sin_addr.s_addr);
                if ((ip >> 24) == 127) continue;
                if ((ip >> 24) == 169) continue;    // link-local
                base = ip & 0xFFFFFF00u;
                mask = 0xFFFFFF00u;
                return true;
            }
        }
    }
    return false;
}

// 扫 1..254 的 7000 端口
std::vector<std::string> scanPort7000(uint32_t base, uint32_t mask) {
    std::vector<std::string> hits;

    // （这里原来建了一个 probe socket 设成非阻塞、然后就一直没用过，
    //   最后直接 close —— 死代码。真正用到的连接在下面按地址逐个建。
    //   2026-10-18 外部审查发现，删掉。）
    //   注意 nonblock 是**下面每个真实连接**都要用的，保留。
    u_long nonblock = 1;

    // 一次性发起所有连接，再统一收集
    std::vector<SOCKET> socks;
    std::vector<std::string> ips;

    // 本机所有 IPv4 地址 —— 下面跳过它们。
    //
    // ★ 原来的注释写着「跳过本机」，**下面什么都没有**。
    //   后果是每轮扫描都对自己发 254 个 SYN 里的一个，等超时；
    //   更糟的是如果本机恰好有程序监听 7000，它会把自己当成一台
    //   AirPlay 设备出现在列表里，点连接必然失败。
    //   （2026-10-23 外部审查 P3，2026-10-24 修）
    // ★ 用 GetAdaptersAddresses 而不是 gethostname + getaddrinfo。
    //
    //   gethostname 拿的是主机名，再靠 DNS/hosts 解析回来 —— 一旦 hosts
    //   里有覆盖、或者机器有多个网卡而解析顺序只返回其中一个，
    //   就会漏掉本机的某些地址。漏掉的后果正是这段要防的事：
    //   扫描打到自己、如果本机有程序监听 7000 还会把自己列成设备。
    //   （2026-10-24 复查建议；gethostname 那版是 2026-10-24 上午的修法）
    //
    //   GetAdaptersAddresses 直接枚举本机所有接口的全部 IPv4，
    //   不经过任何名字解析，不漏。
    std::vector<uint32_t> selfIps;
    {
        ULONG size = 0;
        // 第一次调用只为拿所需缓冲区大小（预期返回 ERROR_BUFFER_OVERFLOW）
        GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                         GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, nullptr, &size);
        if (size > 0) {
            std::vector<BYTE> buf(size);
            auto* addrs = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
            if (GetAdaptersAddresses(AF_INET,
                                     GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                     GAA_FLAG_SKIP_DNS_SERVER,
                                     nullptr, addrs, &size) == NO_ERROR) {
                for (auto* a = addrs; a; a = a->Next) {
                    for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                        if (!ua->Address.lpSockaddr) continue;
                        if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
                        auto* sa = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                        selfIps.push_back(ntohl(sa->sin_addr.s_addr));
                    }
                }
            }
        }
        // 兜底：枚举失败时退回主机名解析（比"全不跳"强）
        if (selfIps.empty()) {
            char host[256] = {0};
            if (gethostname(host, sizeof(host) - 1) == 0) {
                addrinfo* ai = nullptr;
                if (getaddrinfo(host, nullptr, nullptr, &ai) == 0) {
                    for (addrinfo* p = ai; p; p = p->ai_next) {
                        if (p->ai_family == AF_INET) {
                            auto* sa2 = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                            selfIps.push_back(ntohl(sa2->sin_addr.s_addr));
                        }
                    }
                    freeaddrinfo(ai);
                }
            }
        }
    }

    for (uint32_t h = 1; h < 255; ++h) {
        uint32_t ip = (base & mask) | h;
        // 跳过本机（见上面 selfIps 的说明）
        bool isSelf = false;
        for (uint32_t s : selfIps) {
            if (s == ip) { isSelf = true; break; }
        }
        if (isSelf) continue;

        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons(7000);
        dst.sin_addr.s_addr = htonl(ip);

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        ioctlsocket(s, FIONBIO, &nonblock);
        connect(s, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));

        char ipStr[32];
        std::snprintf(ipStr, sizeof(ipStr), "%u.%u.%u.%u",
                      (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
        socks.push_back(s);
        ips.push_back(ipStr);
    }

    // 等 700ms 让连接落地
    Sleep(700);

    for (size_t i = 0; i < socks.size(); ++i) {
        fd_set wr, ex;
        FD_ZERO(&wr); FD_ZERO(&ex);
        FD_SET(socks[i], &wr);
        FD_SET(socks[i], &ex);
        timeval tv{0, 0};
        if (select(0, nullptr, &wr, &ex, &tv) > 0) {
            int err = 0;
            int len = sizeof(err);
            getsockopt(socks[i], SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
            if (err == 0) hits.push_back(ips[i]);
        }
        closesocket(socks[i]);
    }

    return hits;
}

} // namespace

std::vector<AirPlayDevice> discoverAirPlayDevices(uint32_t timeoutMs) {
    std::vector<AirPlayDevice> result;

    WSADATA wsa{};
    bool wsaOwned = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);

    uint32_t base = 0, mask = 0;
    if (!localSubnet(base, mask)) {
        if (wsaOwned) WSACleanup();
        return result;
    }

    // 1) 端口扫描找候选
    auto candidates = scanPort7000(base, mask);

    // 2) 对每个候选做单播 mDNS
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(0);   // 单播不需要绑 5353
        bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local));

        // ★ timeoutMs 是「总预算」，必须当硬性截止时间用。
        //
        // 原来把它除以候选数、再抬到最少 600ms —— 候选一多总耗时就不受控：
        //   10 个候选 → 6.0 秒，20 个 → 12.0 秒（传进去的是 2500）
        // 而这段跑在主线程上，项目自己的约束写着「任何让主循环停摆的东西
        // 都会掐断声音」。所以不能这么算。
        //
        // 现在按「剩余预算」反推每个候选能等多久，用完就停。
        // ★ 全程用 ULONGLONG（64 位），不要截断进 DWORD。
        //
        // 曾经写成 `const DWORD scanStart = GetTickCount64();` 再
        // `static_cast<DWORD>(GetTickCount64() - scanStart)` —— 靠"外层 cast
        // 把内层截断抵消掉"来得到正确结果（数学上确实对），但那是脆弱的写法：
        // 谁删掉外层 cast、或者把 scanStart 升成 64 位混算，就会造出
        // 「开机 49.7 天后设备搜索永远返回空」这种新 bug，而且极难复现。
        // 直接把比对和差值都放在 64 位里，语义就无需解释。
        const ULONGLONG scanStart = GetTickCount64();
        const ULONGLONG totalBudget = timeoutMs;

        for (auto& ip : candidates) {
            const ULONGLONG used = GetTickCount64() - scanStart;
            if (used >= totalBudget) break;              // 预算用尽
            const ULONGLONG left = totalBudget - used;
            const ULONGLONG remain = static_cast<ULONGLONG>(&candidates.back() - &ip) + 1;
            uint32_t per = static_cast<uint32_t>(left / (remain ? remain : 1));
            if (per < 150) per = 150;                    // 太少会查不到，留个下限
            if (per > left) per = static_cast<uint32_t>(left);

            AirPlayDevice d;
            if (queryOne(s, ip, d, per))
                result.push_back(std::move(d));
        }
        closesocket(s);
    }

    if (wsaOwned) WSACleanup();
    return result;
}

} // namespace fxchain
