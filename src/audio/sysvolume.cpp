// SPDX-License-Identifier: Apache-2.0
//
// sysvolume.cpp -- 系统播放音量 + 默认输出设备切换。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "sysvolume.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <vector>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

namespace fxchain {
namespace {

ComPtr<IAudioEndpointVolume> getEndpointVolume() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return nullptr;

    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return nullptr;

    ComPtr<IAudioEndpointVolume> vol;
    if (FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                             &vol))) return nullptr;
    return vol;
}

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    char b[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, 511, nullptr, nullptr);
    return b;
}

// ---- 未公开的 IPolicyConfigVista（切默认设备用，Windows Vista+ 一直可用）----
//
// 注意：这里必须用 IPolicyConfigVista 的方法顺序，它和 IPolicyConfig 不一样 ——
// IPolicyConfigVista 没有 ResetDeviceFormat，所以从 SetDeviceFormat 往后整体前移一格。
// 若照抄 IPolicyConfig 的顺序（多一个 ResetDeviceFormat）来调 Vista 接口，
// SetDefaultEndpoint 会落到 SetEndpointVisibility 的槽位上，调用直接失败。
//
// 这些 IID/CLSID/方法顺序都属于逆向 Windows 得到的接口定义（同一事实有多个独立来源），
// 不是从某个具体项目拷贝的代码，因此与项目的 Apache-2.0 许可证兼容。
struct DECLSPEC_UUID("568b9108-44bf-40b4-9006-86afe5b5a620") IPolicyConfigVista : IUnknown {
    virtual HRESULT GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT SetEndpointVisibility(PCWSTR, INT) = 0;
};

// 老版 IPolicyConfig（有 ResetDeviceFormat），只在老 CLSID 上才用。
struct DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : IUnknown {
    virtual HRESULT GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT SetEndpointVisibility(PCWSTR, INT) = 0;
};

// CLSID 与其配套的 IID 是成对的，不能混搭：
//   CPolicyConfigClient      {870af99c-d171-4d9e-af0d-f62bf91b685d} -> IPolicyConfig      (f8679f50)
//   CPolicyConfigVistaClient {294935ce-f637-4e7c-a41b-ab255460b862} -> IPolicyConfigVista (568b9108)
// Win11 上老 CLSID 未注册（REGDB_E_CLASSNOTREG），只能走 Vista 这一对；
// 老 CLSID 保留作 Vista 之前系统的回退。
const CLSID CLSID_PolicyConfigClient =
    {0x870af99c, 0xd171, 0x4d9e, {0xaf, 0x0d, 0xf6, 0x2b, 0xf9, 0x1b, 0x68, 0x5d}};
const CLSID CLSID_PolicyConfigVistaClient =
    {0x294935CE, 0xF637, 0x4E7C, {0xA4, 0x1B, 0xAB, 0x25, 0x54, 0x60, 0xB8, 0x62}};

// 把某个角色设成 targetId。
// 优先用 Win11 注册的 Vista 版；失败再退回老版 IPolicyConfig。
bool callSetDefaultEndpoint(PCWSTR targetId, ERole role) {
    {
        ComPtr<IPolicyConfigVista> pc;
        HRESULT hr = CoCreateInstance(CLSID_PolicyConfigVistaClient, nullptr, CLSCTX_ALL,
                                      __uuidof(IPolicyConfigVista),
                                      reinterpret_cast<void**>(pc.GetAddressOf()));
        if (SUCCEEDED(hr) && pc)
            return SUCCEEDED(pc->SetDefaultEndpoint(targetId, role));
    }
    {
        ComPtr<IPolicyConfig> pc;
        HRESULT hr = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                                      __uuidof(IPolicyConfig),
                                      reinterpret_cast<void**>(pc.GetAddressOf()));
        if (SUCCEEDED(hr) && pc)
            return SUCCEEDED(pc->SetDefaultEndpoint(targetId, role));
    }
    return false;
}

// 复核：该角色当前默认设备是不是 targetId。
bool isRoleDefault(const std::wstring& targetId, ERole role) {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return false;

    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, role, &dev))) return false;

    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || !id) return false;
    bool same = (targetId == id);
    CoTaskMemFree(id);
    return same;
}

} // namespace

float getSystemVolume() {
    auto vol = getEndpointVolume();
    if (!vol) return -1.0f;
    float v = 0;
    if (FAILED(vol->GetMasterVolumeLevelScalar(&v))) return -1.0f;
    return v;
}

bool setSystemVolume(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    auto vol = getEndpointVolume();
    if (!vol) return false;
    return SUCCEEDED(vol->SetMasterVolumeLevelScalar(v, nullptr));
}

bool isSystemMuted() {
    auto vol = getEndpointVolume();
    if (!vol) return false;
    BOOL m = FALSE;
    if (FAILED(vol->GetMute(&m))) return false;
    return m != FALSE;
}

bool setSystemMuted(bool muted) {
    auto vol = getEndpointVolume();
    if (!vol) return false;
    return SUCCEEDED(vol->SetMute(muted ? TRUE : FALSE, nullptr));
}

// ---- 默认设备切换 ----

std::string getDefaultRenderDeviceId() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return {};

    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return {};

    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || !id) return {};
    std::string out = wideToUtf8(id);
    CoTaskMemFree(id);
    return out;
}

std::string getDefaultRenderDeviceName() {
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return {};

    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return {};

    ComPtr<IPropertyStore> ps;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) return {};

    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::string out;
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        out = wideToUtf8(pv.pwszVal);
    PropVariantClear(&pv);
    return out;
}

std::vector<std::pair<std::string, std::string>> listRenderDevicesWithId() {
    std::vector<std::pair<std::string, std::string>> out;

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return out;

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) return out;

    UINT32 n = 0;
    coll->GetCount(&n);
    for (UINT32 i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(coll->Item(i, &d))) continue;

        std::string id, name;
        LPWSTR wid = nullptr;
        if (SUCCEEDED(d->GetId(&wid)) && wid) {
            id = wideToUtf8(wid);
            CoTaskMemFree(wid);
        }
        ComPtr<IPropertyStore> ps;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                pv.vt == VT_LPWSTR)
                name = wideToUtf8(pv.pwszVal);
            PropVariantClear(&pv);
        }
        if (!id.empty()) out.emplace_back(id, name);
    }
    return out;
}

std::vector<std::string> listRenderDevices() {
    std::vector<std::string> out;

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return out;

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) return out;

    UINT32 n = 0;
    coll->GetCount(&n);
    for (UINT32 i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(coll->Item(i, &d))) continue;
        ComPtr<IPropertyStore> ps;
        if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) continue;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            out.push_back(wideToUtf8(pv.pwszVal));
        PropVariantClear(&pv);
    }
    return out;
}

bool setDefaultRenderDevice(const std::string& nameSubstring) {
    if (nameSubstring.empty()) return false;

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return false;

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) return false;

    UINT32 n = 0;
    coll->GetCount(&n);

    std::wstring targetId;
    for (UINT32 i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(coll->Item(i, &d))) continue;

        ComPtr<IPropertyStore> ps;
        if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) continue;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::string nm;
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            nm = wideToUtf8(pv.pwszVal);
        PropVariantClear(&pv);

        if (nm.empty() || nm.find(nameSubstring) == std::string::npos) continue;

        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) {
            targetId = id;
            CoTaskMemFree(id);
            break;
        }
    }

    if (targetId.empty()) return false;

    // 按 nameSubstring 找到目标设备后，用 IPolicyConfig 系列接口把它设成默认。
    //
    // 关键点（Win11 实际行为）：
    //   1. CLSID 必须和配套 IID 一起用。新版 CLSID {294935ce-...} 只暴露
    //      IPolicyConfigVista (568b9108)，用旧的 IID f8679f50 去 QueryInterface
    //      会拿到 REGDB_E_IIDNOTREG，CoCreateInstance 直接失败。
    //   2. 新版接口的 vtable 没有 ResetDeviceFormat，方法顺序不能照抄旧版。
    //   3. eConsole 一旦成功，系统通常会把 eMultimedia 一起带过去，但
    //      eCommunications 是独立的，必须单独设。
    // 因此三个角色都调一遍，最后统一用 GetDefaultAudioEndpoint 复核结果。
    bool anyOk = false;
    for (int role = 0; role <= 2; ++role) {
        const ERole r = static_cast<ERole>(role);
        int tries = 0;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (callSetDefaultEndpoint(targetId.c_str(), r)) { ++tries; break; }
            Sleep(80);
        }
        if (tries > 0) anyOk = true;
    }
    if (!anyOk) return false;

    // 复核：用独立 enumerator 读回来，确认真的切过去了。
    // 有些驱动/USB 声卡切换是异步的，给一点时间。
    for (int i = 0; i < 10; ++i) {
        if (isRoleDefault(targetId, eConsole)) return true;
        Sleep(50);
    }
    return false;
}

} // namespace fxchain
