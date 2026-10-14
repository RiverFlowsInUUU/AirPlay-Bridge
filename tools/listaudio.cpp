// SPDX-License-Identifier: Apache-2.0
//
// listaudio -- 列出 Windows 的音频设备及其参数，用于诊断。
//
// 输出：
//   - 三个角色（Console / Multimedia / Communications）当前的默认播放设备
//   - 所有活动的输出设备，含缓冲周期和格式
//   - 所有活动的输入设备
//
// 排查"串流没声音"时先跑这个：确认默认设备是不是你以为的那个。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <string>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

namespace {

std::string utf8(const wchar_t* w) {
    if (!w) return {};
    char b[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, 511, nullptr, nullptr);
    return b;
}

std::string devName(IMMDevice* d) {
    ComPtr<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return "(?)";
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string out = "(?)";
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        out = utf8(pv.pwszVal);
    PropVariantClear(&pv);
    return out;
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) {
        std::printf("ERROR: MMDeviceEnumerator 创建失败\n");
        return 1;
    }

    // ---- 默认设备（三个角色）----
    std::printf("=== 默认播放设备 ===\n");
    struct { const char* label; ERole role; } roles[] = {
        {"Console       ", eConsole},
        {"Multimedia    ", eMultimedia},
        {"Communications", eCommunications},
    };
    for (auto& r : roles) {
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, r.role, &dev)))
            std::printf("  %s => %s\n", r.label, devName(dev.Get()).c_str());
        else
            std::printf("  %s => (无)\n", r.label);
    }

    // ---- 所有活动输出设备 ----
    std::printf("\n=== 活动输出设备 ===\n");
    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) {
        std::printf("  枚举失败\n");
        CoUninitialize();
        return 1;
    }

    UINT32 n = 0;
    coll->GetCount(&n);
    for (UINT32 i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;

        std::printf("  %s\n", devName(dev.Get()).c_str());

        ComPtr<IAudioClient> client;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) {
            std::printf("       (打不开)\n");
            continue;
        }

        WAVEFORMATEX* fmt = nullptr;
        if (FAILED(client->GetMixFormat(&fmt))) {
            std::printf("       (取格式失败)\n");
            continue;
        }

        REFERENCE_TIME def = 0, minP = 0;
        client->GetDevicePeriod(&def, &minP);

        std::printf("       %u Hz / %u ch  |  默认周期 %.2f ms  最小周期 %.2f ms\n",
                    fmt->nSamplesPerSec, fmt->nChannels,
                    def / 10000.0, minP / 10000.0);
        CoTaskMemFree(fmt);
    }

    // ---- 输入设备（虚拟声卡的 Output 端在这边）----
    std::printf("\n=== 活动输入设备 ===\n");
    ComPtr<IMMDeviceCollection> caps;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE,
                                              caps.GetAddressOf()))) {
        UINT32 cn = 0;
        caps->GetCount(&cn);
        for (UINT32 i = 0; i < cn; ++i) {
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(caps->Item(i, &dev)))
                std::printf("  %s\n", devName(dev.Get()).c_str());
        }
    }

    CoUninitialize();
    return 0;
}
