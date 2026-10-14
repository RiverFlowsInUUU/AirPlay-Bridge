// SPDX-License-Identifier: Apache-2.0
//
// audiodevices.h -- 列出音频设备（诊断用）。
//
// 原来是个独立的小工具 listaudio.exe，为了让发布只有一个 exe，
// 把功能并进主程序：airplay_bridge --audio-devices
//
// 排查"串流没声音"时先跑这个 —— 确认系统默认设备是不是你以为的那个。
//
// 输出同时写 stdout 和 %LOCALAPPDATA%\AirPlayBridge\audio-devices.log：
// 这是 WIN32 子系统程序，双击或从某些终端调用时 stdout 可能没地方去，
// 落一份文件保证用户永远能看到结果。

#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

namespace fxchain {

// 日志文件的完整路径
inline std::string audioDevLogPath() {
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    std::string dir = (n > 0 && n < MAX_PATH) ? std::string(buf, n) : std::string(".");
    dir += "\\AirPlayBridge\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir + "audio-devices.log";
}

// 输出到 stdout + 日志文件
inline void audioOut(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);

    static FILE* lf = std::fopen(audioDevLogPath().c_str(), "w");
    if (lf) {
        va_list ap2;
        va_start(ap2, fmt);
        std::vfprintf(lf, fmt, ap2);
        va_end(ap2);
        std::fflush(lf);
    }
}

inline std::string audioDevName(IMMDevice* d) {
    ComPtr<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return "(?)";
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string out = "(?)";
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
        char b[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, b, 511, nullptr, nullptr);
        out = b;
    }
    PropVariantClear(&pv);
    return out;
}

inline void printAudioDevices() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) {
        audioOut("(取不到设备枚举器)\n");
        return;
    }

    audioOut("=== 默认播放设备 ===\n");
    struct { const char* label; ERole role; } roles[] = {
        {"Console       ", eConsole},
        {"Multimedia    ", eMultimedia},
        {"Communications", eCommunications},
    };
    for (auto& r : roles) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, r.role, &dev)))
            audioOut("  %s => %s\n", r.label, audioDevName(dev.Get()).c_str());
        else
            audioOut("  %s => (无)\n", r.label);
    }

    audioOut("\n=== 活动输出设备 ===\n");
    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
        audioOut("  (枚举失败)\n");
        return;
    }

    UINT32 n = 0;
    coll->GetCount(&n);
    for (UINT32 i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;

        audioOut("  %s\n", audioDevName(dev.Get()).c_str());

        ComPtr<IAudioClient> client;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            audioOut("       (打不开)\n");
            continue;
        }

        WAVEFORMATEX* fmt = nullptr;
        if (FAILED(client->GetMixFormat(&fmt))) continue;

        REFERENCE_TIME def = 0, minP = 0;
        client->GetDevicePeriod(&def, &minP);
        audioOut("       %u Hz / %u ch  |  默认周期 %.2f ms  最小周期 %.2f ms\n",
                 fmt->nSamplesPerSec, fmt->nChannels,
                 def / 10000.0, minP / 10000.0);
        CoTaskMemFree(fmt);
    }

    audioOut("\n=== 活动输入设备 ===\n");
    ComPtr<IMMDeviceCollection> caps;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &caps))) {
        UINT32 cn = 0;
        caps->GetCount(&cn);
        for (UINT32 i = 0; i < cn; ++i) {
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(caps->Item(i, &dev)))
                audioOut("  %s\n", audioDevName(dev.Get()).c_str());
        }
    }
}

} // namespace fxchain
