// SPDX-License-Identifier: Apache-2.0
//
// wasapi_capture.cpp -- WASAPI 环回采集实现（多设备混音）。
//
// AllDevices 模式：枚举所有输出设备，每台开一个环回采集 + 一个采集线程。
// 每台设备的数据进自己的队列，主循环按帧取出、相加、输出。
//
// 关键设计：
//   - 取"各队列里最大的帧数"，不是最小值。取最小值的话，只要有一台设备
//     当前没在渲染音频（虚拟声卡、没插耳机的口），它的队列永远空，
//     最小值恒为 0，整条链路就一个采样都发不出去。
//   - 数据不够的设备补静音，保证多设备时间轴对齐。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "wasapi_capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

namespace fxchain {
namespace {

// 任意格式 -> float（-1.0 .. 1.0）
inline float toFloat(const BYTE* p, const WAVEFORMATEX* fmt, UINT32 frame, UINT32 ch) {
    const UINT32 channels = fmt->nChannels;
    const UINT32 idx      = frame * channels + ch;
    switch (fmt->wBitsPerSample) {
        case 16: {
            int16_t v;
            std::memcpy(&v, p + idx * 2, 2);
            return v / 32768.0f;
        }
        case 24: {
            // 24-bit 小端。必须在无符号域组装再转有符号 ——
            // 直接 (q[2] << 24) 会溢出 INT_MAX，是有符号左移 UB。
            const BYTE* q = p + idx * 3;
            const uint32_t u = (uint32_t(q[0]) << 8)
                             | (uint32_t(q[1]) << 16)
                             | (uint32_t(q[2]) << 24);
            const int32_t v = static_cast<int32_t>(u);
            return (v >> 8) / 8388608.0f;
        }
        case 32: {
            bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
            if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
                isFloat = (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
            }
            if (isFloat) {
                float v;
                std::memcpy(&v, p + idx * 4, 4);
                return v;
            }
            int32_t v;
            std::memcpy(&v, p + idx * 4, 4);
            return v / 2147483648.0f;
        }
        default:
            return 0.0f;
    }
}

inline int16_t clampS16(float v) {
    if (v > 1.0f)  v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

std::string wideToUtf8(const wchar_t* w) {
    if (!w) return {};
    char buf[512] = {};
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
    return buf;
}

std::string appDataDir() {
    char buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
    std::string base = (n > 0 && n < MAX_PATH) ? std::string(buf, n) : std::string(".");
    std::string dir = base + "\\AirPlayBridge\\";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

// ---- 单台设备的采集 ----
struct DeviceCapture {
    ComPtr<IMMDevice>           device;
    ComPtr<IAudioClient>        client;
    ComPtr<IAudioCaptureClient> capture;
    WAVEFORMATEX* mixFormat = nullptr;
    HANDLE        event     = nullptr;
    std::string   name;
    std::atomic<bool> running{false};

    // 诊断统计
    std::atomic<uint64_t> pulled{0};
    std::atomic<uint64_t> starved{0};

    ~DeviceCapture() {
        if (mixFormat) CoTaskMemFree(mixFormat);
        if (event)     CloseHandle(event);
    }

    bool open(const ComPtr<IMMDevice>& dev, std::string& err) {
        device = dev;

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                name = wideToUtf8(pv.pwszVal);
            PropVariantClear(&pv);
        }

        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(hr)) { err = "Activate failed"; return false; }

        hr = client->GetMixFormat(&mixFormat);
        if (FAILED(hr)) { err = "GetMixFormat failed"; return false; }

        const REFERENCE_TIME dur = 30 * 10000;   // 30ms
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_LOOPBACK |
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                dur, 0, mixFormat, nullptr);
        if (FAILED(hr)) { err = "Initialize(loopback) failed"; return false; }

        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) { err = "CreateEvent failed"; return false; }

        hr = client->SetEventHandle(event);
        if (FAILED(hr)) { err = "SetEventHandle failed"; return false; }

        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) { err = "GetService failed"; return false; }

        return true;
    }

    // 把设备当前所有可读的数据抽干，转成交错立体声 s16
    UINT32 pull(std::vector<int16_t>& out) {
        if (!capture || !mixFormat) return 0;

        out.clear();
        UINT32 total = 0;

        for (;;) {
            BYTE*  data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;

            HRESULT hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr) || frames == 0) break;

            const size_t base = out.size();
            out.resize(base + frames * 2);

            int16_t* dst = out.data() + base;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::memset(dst, 0, frames * 2 * sizeof(int16_t));
            } else {
                const UINT32 srcCh = mixFormat->nChannels;
                for (UINT32 f = 0; f < frames; ++f) {
                    float l, r;
                    if (srcCh == 1) {
                        l = r = toFloat(data, mixFormat, f, 0);
                    } else if (srcCh == 2) {
                        l = toFloat(data, mixFormat, f, 0);
                        r = toFloat(data, mixFormat, f, 1);
                    } else {
                        l = toFloat(data, mixFormat, f, 0);
                        r = toFloat(data, mixFormat, f, 1);
                        for (UINT32 c = 2; c < srcCh; ++c) {
                            float v = toFloat(data, mixFormat, f, c) * 0.5f;
                            l += v;
                            r += v;
                        }
                        l *= 0.7f;
                        r *= 0.7f;
                    }
                    dst[f * 2 + 0] = clampS16(l);
                    dst[f * 2 + 1] = clampS16(r);
                }
            }

            capture->ReleaseBuffer(frames);
            total += frames;
        }
        return total;
    }
};

// ---- 每台设备的数据队列（生产者写、主循环读，用锁保护足够） ----
struct DeviceQueue {
    std::mutex           m;
    std::vector<int16_t> buf;                 // 交错立体声
    size_t               maxSamples = 44100 * 2 * 2;   // 最多缓存 2 秒
};

} // namespace

// ============================================================
struct WasapiCapture::Impl {
    ComPtr<IMMDeviceEnumerator> enumerator;
    std::vector<std::unique_ptr<DeviceCapture>> devices;
    std::vector<std::unique_ptr<DeviceQueue>>   queues;
    uint32_t rate = 44100;
};

WasapiCapture::WasapiCapture(WasapiCaptureConfig cfg) : cfg_(cfg) {
    impl_ = std::make_unique<Impl>();
}

WasapiCapture::~WasapiCapture() {
    stop();
}

std::vector<std::string> WasapiCapture::listRenderDevices() {
    std::vector<std::string> out;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) return out;

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll))) return out;

    UINT32 n = 0;
    coll->GetCount(&n);
    for (UINT32 i = 0; i < n; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        ComPtr<IPropertyStore> props;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, &props))) continue;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
            out.push_back(wideToUtf8(pv.pwszVal));
        PropVariantClear(&pv);
    }
    return out;
}

bool WasapiCapture::open(std::string* errOut) {
    auto fail = [&](const char* m) { if (errOut) *errOut = m; return false; };

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&impl_->enumerator));
    if (FAILED(hr)) return fail("CoCreateInstance(MMDeviceEnumerator) failed");

    std::vector<ComPtr<IMMDevice>> targets;

    // 目前固定走单设备模式（默认输出设备）。
    //
    // 多设备混音（AllDevices）的实现保留在下面，但它实测有未解决的问题：
    // 多台设备同时打开时采集到的全是静音，单设备则正常。怀疑是各设备
    // 时钟/缓冲不同步导致的，没查清楚。接管模式下这个功能也用不上
    // （声音都进虚拟声卡了，采一台就够），所以暂不启用。
    if (cfg_.mode == CaptureMode::DefaultOnly) {
        ComPtr<IMMDevice> dev;
        if (FAILED(impl_->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
            return fail("no default playback device");
        targets.push_back(dev);
    } else {
        ComPtr<IMMDeviceCollection> coll;
        if (FAILED(impl_->enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)))
            return fail("EnumAudioEndpoints failed");
        UINT32 n = 0;
        coll->GetCount(&n);
        for (UINT32 i = 0; i < n; ++i) {
            ComPtr<IMMDevice> dev;
            if (SUCCEEDED(coll->Item(i, &dev))) targets.push_back(dev);
        }
        if (targets.empty()) return fail("no active playback device");
    }

    // 逐台打开；打不开的跳过（一台失败不该让整个功能失效）
    for (auto& dev : targets) {
        auto dc = std::make_unique<DeviceCapture>();
        std::string err;
        if (!dc->open(dev, err)) continue;
        impl_->devices.push_back(std::move(dc));
    }

    if (impl_->devices.empty()) return fail("no playback device could be opened");

    impl_->rate = impl_->devices[0]->mixFormat->nSamplesPerSec;
    for (size_t i = 0; i < impl_->devices.size(); ++i)
        impl_->queues.push_back(std::make_unique<DeviceQueue>());

    return true;
}

size_t WasapiCapture::deviceCount() const {
    return impl_ ? impl_->devices.size() : 0;
}

std::vector<std::string> WasapiCapture::deviceNames() const {
    std::vector<std::string> out;
    if (!impl_) return out;
    for (auto& d : impl_->devices) out.push_back(d->name);
    return out;
}

uint32_t WasapiCapture::deviceSampleRate() const {
    return impl_ ? impl_->rate : 44100;
}

uint32_t WasapiCapture::deviceChannels() const { return 2; }

void WasapiCapture::run(const AudioChunkCallback& cb) {
    if (!impl_ || impl_->devices.empty()) return;

    running_.store(true);

    // ---- 每台设备一个采集线程 ----
    std::vector<std::thread> workers;
    workers.reserve(impl_->devices.size());

    for (size_t i = 0; i < impl_->devices.size(); ++i) {
        workers.emplace_back([this, i]() {
            DeviceCapture* dc = impl_->devices[i].get();
            DeviceQueue*   q  = impl_->queues[i].get();
            std::vector<int16_t> local;

            HRESULT hr = dc->client->Start();
            if (FAILED(hr)) return;
            dc->running.store(true);

            while (running_.load() && dc->running.load()) {
                DWORD w = WaitForSingleObject(dc->event, 200);
                if (w != WAIT_OBJECT_0) continue;

                UINT32 got = dc->pull(local);
                if (got == 0) { ++dc->starved; continue; }
                dc->pulled += got;

                // 写进自己的队列；满了丢最老的（实时优先，绝不阻塞采集）
                std::lock_guard<std::mutex> lk(q->m);
                const size_t incoming = local.size();
                if (q->buf.size() + incoming > q->maxSamples) {
                    const size_t drop = q->buf.size() + incoming - q->maxSamples;
                    if (drop >= q->buf.size()) q->buf.clear();
                    else q->buf.erase(q->buf.begin(), q->buf.begin() + drop);
                }
                q->buf.insert(q->buf.end(), local.begin(), local.end());
            }

            dc->client->Stop();
            dc->running.store(false);
        });
    }

    // ---- 主循环：取数据、混音、输出 ----
    std::vector<int32_t> take;
    std::vector<int16_t> out;

    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.bufferMs));

        // 取各队列里最大的帧数：谁有数据按谁来，没数据的补静音。
        // 千万不能取最小值（见文件头说明）。
        size_t frames = 0;
        for (auto& q : impl_->queues) {
            std::lock_guard<std::mutex> lk(q->m);
            frames = (std::max)(frames, q->buf.size() / 2);
        }
        if (frames == 0) continue;

        take.assign(frames * 2, 0);
        for (auto& q : impl_->queues) {
            std::lock_guard<std::mutex> lk(q->m);
            const size_t have = (std::min)(frames * 2, q->buf.size());
            for (size_t k = 0; k < have; ++k) take[k] += q->buf[k];
            q->buf.erase(q->buf.begin(), q->buf.begin() + have);
        }

        // 限幅
        out.resize(frames * 2);
        int peak = 0;
        for (size_t f = 0; f < frames; ++f) {
            int32_t l = take[f * 2 + 0];
            int32_t r = take[f * 2 + 1];
            if (l > 32767)  l = 32767;
            if (l < -32768) l = -32768;
            if (r > 32767)  r = 32767;
            if (r < -32768) r = -32768;
            out[f * 2 + 0] = static_cast<int16_t>(l);
            out[f * 2 + 1] = static_cast<int16_t>(r);

            int al = l < 0 ? -l : l;
            int ar = r < 0 ? -r : r;
            if (al > peak) peak = al;
            if (ar > peak) peak = ar;
        }

        // 电平 + 各设备统计，每 2 秒落一次盘（托盘模式没有控制台）
        {
            static auto lastReport = std::chrono::steady_clock::now();
            static int peakSeen = 0;
            if (peak > peakSeen) peakSeen = peak;

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastReport).count() >= 2) {
                lastReport = now;
                FILE* lf = std::fopen((appDataDir() + "level.txt").c_str(), "w");
                if (lf) {
                    std::fprintf(lf, "peak %d\n", peakSeen);
                    for (size_t di = 0; di < impl_->devices.size(); ++di) {
                        std::fprintf(lf, "%s | pulled=%llu starved=%llu\n",
                                     impl_->devices[di]->name.c_str(),
                                     (unsigned long long)impl_->devices[di]->pulled.load(),
                                     (unsigned long long)impl_->devices[di]->starved.load());
                    }
                    std::fclose(lf);
                }
                peakSeen = 0;
            }
        }

        if (cb) cb(out.data(), frames);
    }

    for (auto& t : workers) if (t.joinable()) t.join();
    running_.store(false);
}

void WasapiCapture::stop() {
    running_.store(false);
    if (impl_) {
        for (auto& d : impl_->devices) d->running.store(false);
    }
}

} // namespace fxchain
