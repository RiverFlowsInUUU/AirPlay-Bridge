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

bool loadConfig(const std::string& path, BridgeConfig& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string j = ss.str();
    if (j.empty()) return false;

    out.lastDeviceId   = jsonGetString(j, "lastDeviceId");
    out.lastDeviceName = jsonGetString(j, "lastDeviceName");
    out.lastHost       = jsonGetString(j, "lastHost");
    out.volume         = static_cast<int>(jsonGetInt(j, "volume", out.volume));
    out.latencyMs      = static_cast<int>(jsonGetInt(j, "latencyMs", out.latencyMs));
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
        // 再钳到 50-100：老值 0/1/3 会抬到下限 5.0 秒，20 秒这类会压到上限 10.0 秒。
        out.standbyTenths = static_cast<int>(jsonGetInt(j, "standbySecs", 5)) * 10;
    }
    // 50-100 之外的都是脏数据或老配置的越界值，钳回区间。
    // 首次运行引导有没有做过。
    // 走 jsonGetBool，不手写带转义的 key 字面量。
    out.askedAutoStart = jsonGetBool(j, "askedAutoStart", out.askedAutoStart);
    out.muteLocalOnConnect =
        jsonGetBool(j, "muteLocalOnConnect", out.muteLocalOnConnect);
    out.captureDeviceId = jsonGetString(j, "captureDeviceId");
    out.localMutedByUs = jsonGetBool(j, "localMutedByUs", out.localMutedByUs);

    if (out.standbyTenths < 0)   out.standbyTenths = 0;
    if (out.standbyTenths > 200) out.standbyTenths = 200;

    // ignoredDeviceIds: 简单的字符串数组解析
    size_t arr = j.find("\"ignoredDeviceIds\"");
    if (arr != std::string::npos) {
        size_t lb = j.find('[', arr);
        size_t rb = (lb == std::string::npos) ? std::string::npos : j.find(']', lb);
        if (lb != std::string::npos && rb != std::string::npos) {
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
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

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
    }
    f << "  \"ignoredDeviceIds\": [";
    for (size_t i = 0; i < cfg.ignoredDeviceIds.size(); ++i) {
        if (i) f << ", ";
        f << "\"" << escapeJson(cfg.ignoredDeviceIds[i]) << "\"";
    }
    f << "]\n";
    f << "}\n";
    return true;
}

} // namespace fxchain
