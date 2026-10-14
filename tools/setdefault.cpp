// 切换 Windows 默认播放设备。
//
// 用未公开的 IPolicyConfig 接口（Windows 一直用它，稳定）。
// 用法： setdefault.exe <设备名子串>
// 例：   setdefault.exe "CABLE Input"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>
#include <string>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// ---- IPolicyConfig（未公开，但一直可用）----
interface DECLSPEC_UUID("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig;
const CLSID CLSID_CPolicyConfigClient =
    {0x870af99c, 0xd171, 0x4d9e, {0xaf, 0x0d, 0xf6, 0x2b, 0xf9, 0x1b, 0x68, 0x5d}};

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

static std::string utf8(const wchar_t* w) {
    if (!w) return {};
    char b[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, b, 511, nullptr, nullptr);
    return b;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: setdefault.exe <device name substring>\n");
        std::printf("\nAvailable render devices:\n");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IMMDeviceEnumerator> en;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        ComPtr<IMMDeviceCollection> coll;
        en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
        UINT32 n = 0; coll->GetCount(&n);
        for (UINT32 i = 0; i < n; ++i) {
            ComPtr<IMMDevice> d;
            if (FAILED(coll->Item(i, &d))) continue;
            ComPtr<IPropertyStore> ps;
            d->OpenPropertyStore(STGM_READ, &ps);
            PROPVARIANT pv; PropVariantInit(&pv);
            if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                std::printf("  %s\n", utf8(pv.pwszVal).c_str());
            PropVariantClear(&pv);
        }
        return 1;
    }

    const std::string want = argv[1];

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) {
        std::printf("ERROR: device enumerator failed\n");
        return 1;
    }

    ComPtr<IMMDeviceCollection> coll;
    en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll);
    UINT32 n = 0;
    coll->GetCount(&n);

    std::string foundId;
    std::string foundName;
    for (UINT32 i = 0; i < n; ++i) {
        ComPtr<IMMDevice> d;
        if (FAILED(coll->Item(i, &d))) continue;
        ComPtr<IPropertyStore> ps;
        d->OpenPropertyStore(STGM_READ, &ps);
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::string nm;
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            nm = utf8(pv.pwszVal);
        PropVariantClear(&pv);

        if (!nm.empty() && nm.find(want) != std::string::npos) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) {
                char b[512] = {};
                WideCharToMultiByte(CP_UTF8, 0, id, -1, b, 511, nullptr, nullptr);
                foundId = b;
                foundName = nm;
                CoTaskMemFree(id);
                break;
            }
        }
    }

    if (foundId.empty()) {
        std::printf("ERROR: no device matching '%s'\n", want.c_str());
        return 1;
    }

    // 转成宽字符
    wchar_t wid[512] = {};
    MultiByteToWideChar(CP_UTF8, 0, foundId.c_str(), -1, wid, 511);

    ComPtr<IPolicyConfig> pc;
    HRESULT hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL,
                                  __uuidof(IPolicyConfig),
                                  reinterpret_cast<void**>(pc.GetAddressOf()));
    if (FAILED(hr)) {
        std::printf("ERROR: CoCreateInstance(IPolicyConfig) failed hr=0x%08lX\n",
                    static_cast<unsigned long>(hr));
        return 1;
    }

    // 三个角色都设：Console / Multimedia / Communications
    for (int role = 0; role <= 2; ++role) {
        hr = pc->SetDefaultEndpoint(wid, static_cast<ERole>(role));
        if (FAILED(hr)) {
            std::printf("WARN: SetDefaultEndpoint(role=%d) hr=0x%08lX\n",
                        role, static_cast<unsigned long>(hr));
        }
    }

    std::printf("OK: default -> %s\n", foundName.c_str());
    CoUninitialize();
    return 0;
}
