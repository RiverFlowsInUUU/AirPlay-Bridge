// SPDX-License-Identifier: Apache-2.0
//
// config_store.cpp -- 配置读写实现。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "config_store.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace fxchain {
namespace {

// 找 JSON 里 "key" 之后的值起点
size_t findValue(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat);
    if (p == std::string::npos) return std::string::npos;
    size_t colon = json.find(':', p + pat.size());
    if (colon == std::string::npos) return std::string::npos;
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    return i;
}

std::string escapeJson(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:   o += c;
        }
    }
    return o;
}

} // namespace

std::string jsonGetString(const std::string& json, const std::string& key) {
    size_t i = findValue(json, key);
    if (i == std::string::npos || i >= json.size() || json[i] != '"') return {};
    ++i;
    std::string out;
    while (i < json.size() && json[i] != '"') {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char n = json[i + 1];
            if (n == 'n') out += '\n';
            else if (n == 'r') out += '\r';
            else if (n == 't') out += '\t';
            else out += n;
            i += 2;
        } else {
            out += json[i++];
        }
    }
    return out;
}

long long jsonGetInt(const std::string& json, const std::string& key, long long def) {
    size_t i = findValue(json, key);
    if (i == std::string::npos) return def;
    const char* p = json.c_str() + i;
    char* end = nullptr;
    long long v = std::strtoll(p, &end, 10);
    return (end == p) ? def : v;
}

bool jsonGetBool(const std::string& json, const std::string& key, bool def) {
    size_t i = findValue(json, key);
    if (i == std::string::npos) return def;
    if (json.compare(i, 4, "true") == 0) return true;
    if (json.compare(i, 5, "false") == 0) return false;
    return def;
}

// 运行时数据统一放 %LOCALAPPDATA%\\AirPlayBridge\\，
// 这样 exe 是真正零外挂的：放哪儿都能跑，不在自己旁边堆文件。
namespace {
ConfigSaveFailHook g_saveFailHook = nullptr;
}  // namespace

void setConfigSaveFailHook(ConfigSaveFailHook fn) { g_saveFailHook = fn; }

namespace {
void notifySaveFail() { if (g_saveFailHook) g_saveFailHook(); }
}  // namespace

std::string appDataDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    std::string base = (n > 0 && n < MAX_PATH) ? std::string(buf, n)
                                               : std::string(".");
    std::string dir = base + "\\AirPlayBridge\\";
    CreateDirectoryA(dir.c_str(), nullptr);   // 已存在时返回失败，忽略
    return dir;
}

std::string defaultConfigPath() {
    return appDataDir() + "bridge_config.json";
}

namespace {

// 本地编码（A 版 API 的字符串）-> UTF-16。
//
// ★ 必须是 CP_ACP，不是 CP_UTF8（2026-10-18 修正了一个我自己引入的回归）：
//   本文件里的路径来自 appDataDir()，而它用 **GetEnvironmentVariableA** ——
//   返回的是**系统本地编码（ACP）**字节，不是 UTF-8。
//   拿它去按 UTF-8 解码，非法字节会被换成 U+FFFD，得到一条**错误的宽路径**，
//   于是 MoveFileExW / DeleteFileW 全部失败 → 配置写不进去、.tmp 残留，
//   而这 29 个调用点全都忽略返回值 → **完全静默**。
//
//   中文用户名（如“张三”）下必然触发。实测（同一段 ACP 路径）：
//     CP_UTF8 转换 -> MoveFileExW 失败 err=3（路径找不到）
//     CP_ACP  转换 -> MoveFileExW 成功
//
//   注意：另一个文件 airplay_bridge.cpp 里的 acpToWide() 是同一套约定 ——
//   这条链上（路径、防火墙规则名）一律是 ACP。
//
// 注意 need **包含结尾的 NUL**，所以按 need 分配、之后再 resize(need - 1) ——
// 这个坑在本项目踩过三次（MultiByteToWideChar 越界写 2 字节）。
std::wstring acpToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int need = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(need), 0);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, out.data(), need);
    out.resize(static_cast<size_t>(need - 1));
    return out;
}

}  // namespace

bool loadConfig(const std::string& path, BridgeConfig& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string j = ss.str();
    if (j.empty()) return false;

    // ★ 截断 / 损坏保护：一次写入被中断会留下**半份 JSON**，而它「非空」，
    //   原来会被当成有效配置 —— deviceId 缺失就重新生成、设置全部回落默认。
    //   这与本项目的红线（v1.5.4「fallback 悄悄改掉用户存下的值」）是同一类。
    //
    //   这里只做最小健全性检查：去掉尾部空白后，首尾必须是花括号。
    //   不通过 → 把内容另存一份 .bak 留证，然后拒绝加载。
    //   调用方按「没有配置」处理（保持默认值），不拿半份文件当真。
    //
    //   空白判断用数字字面量（10/13/32/9），不写反斜杠转义 ——
    //   本文件顶部注释说明过：反斜杠转义在这条工具链里会被吃掉。
    {
        std::string t = j;
        while (!t.empty()) {
            const char c = t.back();
            if (c == 10 || c == 13 || c == 32 || c == 9) { t.pop_back(); continue; }
            break;
        }
        const bool complete = t.size() > 1 && t.front() == char(123) && t.back() == char(125);
        if (!complete) {
            const std::string bak = path + ".bak";
            std::ofstream b(bak, std::ios::binary | std::ios::trunc);
            if (b) b << j;                 // 留证：事后能看出是不是被截断的
            return false;
        }
    }

    out.lastDeviceId   = jsonGetString(j, "lastDeviceId");
    out.lastDeviceName = jsonGetString(j, "lastDeviceName");
    out.lastHost       = jsonGetString(j, "lastHost");
    out.volume         = static_cast<int>(jsonGetInt(j, "volume", out.volume));
    out.latencyMs      = static_cast<int>(jsonGetInt(j, "latencyMs", out.latencyMs));
    // 读进来就钳到可用范围。
    // 上游 setVolume/setLatencyMs 各自也会钳，但菜单标题显示的是这里的原值 ——
    // 不钳的话会出现「菜单写着 9999ms、实际按 1000ms 播」，显示和实际不一致。
    // 音量与菜单一致（0-100）；延迟的钳制范围（100-1000）故意比菜单（100-300）
    // 宽 —— 它只是防脏数据的兜底，不是可选项列表。写「与菜单一致」是错的。
    if (out.volume < 0)      out.volume = 0;
    if (out.volume > 100)    out.volume = 100;
    if (out.latencyMs < 100) out.latencyMs = 100;
    if (out.latencyMs > 1000) out.latencyMs = 1000;
    // 新的连续品质字段
    if (j.find("\"qualityLevel\"") != std::string::npos) {
        out.qualityLevel = static_cast<int>(jsonGetInt(j, "qualityLevel", 100));
    } else if (j.find("\"quality\"") != std::string::npos) {
        // 老配置是 0-3 的档位，换算成 0-100。
        // 不换算的话，老的"3=最好"会被当成"3/100=很差"。
        const int old = static_cast<int>(jsonGetInt(j, "quality", 3));
        switch (old) {
            case 0:  out.qualityLevel = 0;   break;   // 关
            case 1:  out.qualityLevel = 30;  break;   // 老的"线性"
            case 2:  out.qualityLevel = 60;  break;   // 老的"好"
            default: out.qualityLevel = 100; break;   // 老的"最好"
        }
    }
    if (out.qualityLevel < 0)   out.qualityLevel = 0;
    if (out.qualityLevel > 100) out.qualityLevel = 100;
    out.deviceId       = jsonGetString(j, "deviceId");
    out.preCaptureDevice = jsonGetString(j, "preCaptureDevice");
    out.autoReconnect  = jsonGetBool(j, "autoReconnect", out.autoReconnect);
    out.hideAppleTV    = jsonGetBool(j, "hideAppleTV", out.hideAppleTV);
    out.standbyEnabled = jsonGetBool(j, "standbyEnabled", out.standbyEnabled);
    // 新的十分之一秒字段优先；没有就按老的整秒字段迁移。
    if (j.find("\"standbyTenths\"") != std::string::npos) {
        out.standbyTenths = static_cast<int>(jsonGetInt(j, "standbyTenths", out.standbyTenths));
    } else if (j.find("\"standbySecs\"") != std::string::npos) {
        // 老配置是整秒（还有已经被砍掉的 0/1/3 秒档位）。乘 10 换算成十分之一秒，
        // 再钳到 0-200（十分之一秒，即 0-20 秒；0 = 关掉静音待机）。
        out.standbyTenths = static_cast<int>(jsonGetInt(j, "standbySecs", 5)) * 10;
    }
    // 0-200（0-20 秒）之外的都是脏数据或老配置的越界值，钳回区间。
    // 首次运行引导有没有做过。
    // 走 jsonGetBool，不手写带转义的 key 字面量。
    out.askedAutoStart = jsonGetBool(j, "askedAutoStart", out.askedAutoStart);
    out.muteLocalOnConnect =
        jsonGetBool(j, "muteLocalOnConnect", out.muteLocalOnConnect);
    out.captureDeviceId = jsonGetString(j, "captureDeviceId");
    out.localMutedByUs = jsonGetBool(j, "localMutedByUs", out.localMutedByUs);
    // 缺键 → 保持传入值（与上面的 volume / latencyMs 一致）；
    // 键在但值为空 → 才回退 "en"（空语言会让界面一条词条都匹配不上）。
    //
    // 原来只写 jsonGetString 再判空，于是「文件里根本没有这个键」也被当成空、
    // 一并强制成 "en" —— 同一个函数里两套语义（数值保传入值、字符串不保）。
    // v1.5.4 那次事故就是「一个 fallback 悄悄改掉了用户存下的值」，同一类问题。
    if (findValue(j, "language") != std::string::npos) {
        out.language = jsonGetString(j, "language");
        if (out.language.empty()) out.language = "en";
    }

    if (out.standbyTenths < 0)   out.standbyTenths = 0;
    if (out.standbyTenths > 200) out.standbyTenths = 200;

    // ignoredDeviceIds: 简单的字符串数组解析
    size_t arr = j.find("\"ignoredDeviceIds\"");
    if (arr != std::string::npos) {
        size_t lb = j.find('[', arr);
        size_t rb = (lb == std::string::npos) ? std::string::npos : j.find(']', lb);
        if (lb != std::string::npos && rb != std::string::npos) {
            // ★ 先清空。其它字段都是赋值语义（覆盖），只有这里是 push_back ——
            //   不清空的话，对同一个对象 load 两次就会把设备列表累积成两倍
            //   （P2-5，外部审查发现）。目前所有调用点都传新对象所以没触发，
            //   但「同名函数里只有这一个字段不是覆盖语义」本身就是个雷。
            out.ignoredDeviceIds.clear();
            std::string body = j.substr(lb + 1, rb - lb - 1);
            size_t p = 0;
            while ((p = body.find('"', p)) != std::string::npos) {
                size_t e = body.find('"', p + 1);
                if (e == std::string::npos) break;
                out.ignoredDeviceIds.push_back(body.substr(p + 1, e - p - 1));
                p = e + 1;
            }
        }
    }
    return true;
}

bool saveConfig(const std::string& path, const BridgeConfig& cfg) {
// ★ 原子写：先写同目录下的 .tmp，全部落盘后再整体换名覆盖正式文件。
//
// 原来直接 ofstream(path, trunc) 原地截断 —— 写到一半被杀（用户强杀、
// 断电、崩溃）就留下**半份 JSON**；而 loadConfig 只看「文件非空」，
// 于是半份文件被当成有效配置：deviceId 缺失会被重新生成
// （接收端把这台电脑当新设备），其余设置全部回落默认。
//
// 本程序明确以「用户可能强杀」为设计前提（preCaptureDevice / localMutedByUs
// 的崩溃恢复就是为此），配置这条链不该是唯一没有保护的那个。
const std::string tmpPath = path + ".tmp";
{
    std::ofstream f(tmpPath, std::ios::binary | std::ios::trunc);
    if (!f) { notifySaveFail(); return false; }

    f << "{\n";
    f << "  \"lastDeviceId\": \""   << escapeJson(cfg.lastDeviceId)   << "\",\n";
    f << "  \"lastDeviceName\": \"" << escapeJson(cfg.lastDeviceName) << "\",\n";
    f << "  \"lastHost\": \""       << escapeJson(cfg.lastHost)       << "\",\n";
    // 用 QC 拼引号、char(10) 换行。不用反斜杠转义 —— 它在工具链里会被吃掉。
    {
        const char QC = '"';
        f << "  " << QC << "deviceId" << QC << ": " << QC
          << escapeJson(cfg.deviceId) << QC << "," << char(10);
    }
    f << "  \"volume\": "           << cfg.volume                       << ",\n";
    f << "  \"latencyMs\": "        << cfg.latencyMs                    << ",\n";
    f << "  \"qualityLevel\": "     << cfg.qualityLevel                 << ",\n";
    f << "  \"preCaptureDevice\": \"" << escapeJson(cfg.preCaptureDevice) << "\",\n";
    f << "  \"autoReconnect\": "    << (cfg.autoReconnect ? "true" : "false") << ",\n";
    f << "  \"hideAppleTV\": "      << (cfg.hideAppleTV ? "true" : "false")   << ",\n";
    // 两个待机开关同样用 QC 拼引号、char(10) 换行（见上面的说明）。
    {
        const char QC = '"';
        f << "  " << QC << "standbyEnabled" << QC << ": "
          << (cfg.standbyEnabled ? "true" : "false") << "," << char(10);
        f << "  " << QC << "standbyTenths" << QC << ": "
          << cfg.standbyTenths << "," << char(10);
        f << "  " << QC << "askedAutoStart" << QC << ": "
          << (cfg.askedAutoStart ? "true" : "false") << "," << char(10);
        f << "  " << QC << "muteLocalOnConnect" << QC << ": "
          << (cfg.muteLocalOnConnect ? "true" : "false") << "," << char(10);
        f << "  " << QC << "captureDeviceId" << QC << ": " << QC
          << escapeJson(cfg.captureDeviceId) << QC << "," << char(10);
        f << "  " << QC << "localMutedByUs" << QC << ": "
          << (cfg.localMutedByUs ? "true" : "false") << "," << char(10);
        f << "  " << QC << "language" << QC << ": " << QC
          << escapeJson(cfg.language) << QC << "," << char(10);
    }
    f << "  \"ignoredDeviceIds\": [";
    for (size_t i = 0; i < cfg.ignoredDeviceIds.size(); ++i) {
        if (i) f << ", ";
        f << "\"" << escapeJson(cfg.ignoredDeviceIds[i]) << "\"";
    }
    f << "]\n";
    f << "}\n";
    f.flush();
    if (!f) {                       // 写失败（磁盘满等）—— 不要覆盖正式文件
        f.close();
        DeleteFileW(acpToWide(tmpPath).c_str());
        notifySaveFail();
        return false;
    }
    f.close();                      // 必须先关：下面要独立句柄去刷盘
}
// ★ flush() 只把内容交到操作系统的缓存，**没落盘**。
//   断电时可能出现「目录项换了、内容是空的」—— 那正是本函数想防的
//   「半份文件」，只是触发条件从「进程被杀」变成「断电」。
//   所以：先 FlushFileBuffers 把 tmp 真正写进磁盘，再带 WRITE_THROUGH 换名。
//   （2026-10-18 第四轮外部审查 P3-1）
{
    const std::wstring wtmp = acpToWide(tmpPath);
    HANDLE h = CreateFileW(wtmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(h);        // 失败也继续：换名本身仍能保证「不出现半份」
        CloseHandle(h);
    }
}
// 同一目录内换名是原子的：要么旧文件完整、要么新文件完整，不存在半份。
// MOVEFILE_WRITE_THROUGH：等这次换名真正落到磁盘才返回。
if (!MoveFileExW(acpToWide(tmpPath).c_str(), acpToWide(path).c_str(),
                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(acpToWide(tmpPath).c_str());
    notifySaveFail();
    return false;
}
return true;
}

} // namespace fxchain
