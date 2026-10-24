// SPDX-License-Identifier: Apache-2.0
//
// wasapi_capture.h -- 用 WASAPI 环回模式采集 Windows 的输出声音。
//
// 两种模式：
//   DefaultOnly 只采集一台输出设备（默认就是系统默认设备；配了
//               captureDeviceId 时用那一台）。**这是当前的默认模式**，
//               也是唯一验证过能正常工作的模式。
//   AllDevices  采集"所有活动的输出设备"并混音 —— 设计意图是
//               某个应用被单独指到别的声卡时也能抓到。**未启用**：
//               实测多台同时打开采到的全是静音（见 wasapi_capture.cpp
//               里的说明），且各设备采样率可能不同、按样本直接相加会出错。
//               要启用必须先做逐路重采样。
//
// 输出统一为交错 16-bit 立体声。
//
// ★ 本文件原来把 AllDevices 标成"（默认）"、把 DefaultOnly 标成"旧行为，
//   保留备用"，与实际相反 —— 那样读代码的人会以为默认走的是多设备混音。
//   （2026-10-23 外部审查 P3，2026-10-24 改正）

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fxchain {

using AudioChunkCallback = std::function<void(const int16_t* data, size_t frames)>;

enum class CaptureMode {
    AllDevices,    // 所有活动输出设备混音（**未启用**，见文件头说明）
    DefaultOnly,   // 仅一台输出设备（默认 / 当前唯一在用）
};

struct WasapiCaptureConfig {
    // 默认单设备（默认输出设备）。多设备混音实现有未解决的问题，
    // 见 wasapi_capture.cpp 里的说明；接管模式下也不需要。
    CaptureMode mode = CaptureMode::DefaultOnly;
    // 指定采集哪个输出端点（设备 id，来自 IMMDevice::GetId）。
    // 空 = 跟随系统默认输出设备。
    //
    // mode 仍保持 DefaultOnly —— 它表达的是「只采一路」，
    // 而不是「必须采默认的那一路」。AllDevices 那条路仍未启用。
    std::wstring deviceId;
    uint32_t    targetSampleRate = 44100;
    uint32_t    bufferMs         = 30;
};

class WasapiCapture {
public:
    explicit WasapiCapture(WasapiCaptureConfig cfg = {});
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&)            = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    // 打开采集。成功返回 true，失败填 errOut。
    // 返回实际采集的设备数量（可通过 deviceCount() 取）。
    bool open(std::string* errOut = nullptr);

    // 开始采集，数据通过 cb 推送。阻塞直到 stop()。
    void run(const AudioChunkCallback& cb);

    void stop();

    // （原来这里有个 isRunning() = running_ && 两类失败都为 0。
    //   没有调用方 —— 上层要判断健康时直接看两个计数更清楚，
    //   而 running_ 在 stop() 之后也会是 false，混进去容易被误读。
    //   2026-10-19 第七轮复核指出后删掉。）
    int  startFailures() const { return startFailures_.load(); }
    int  runtimeFailures() const { return runtimeFailures_.load(); }

    // 实际打开的设备数（AllDevices 模式下可能大于 1）
    size_t deviceCount() const;

    // 采集到的设备名列表（用于日志）
    std::vector<std::string> deviceNames() const;

    // 采样率 / 声道数（取第一台设备的）。
    //
    // ★ 原注释写「不同设备会做简单适配」—— **没有这回事**。
    //   AllDevices 模式下各设备按样本直接相加，采样率不同的设备混出来就是错的
    //   （也正是它没能启用的原因之一）。要启用必须先做逐路重采样。
    //   这句注释的存在会让人以为"混音这块已经处理好了"。
    uint32_t deviceSampleRate() const;
    uint32_t deviceChannels() const;

    // 调试用：列出所有活动输出设备名
    static std::vector<std::string> listRenderDevices();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    // 有多少路设备 Start() 失败了。非 0 表示「跑着但没在收音频」——
    // 原来这种失败被直接 return 吞掉，running_ 仍是 true，
    // 界面显示「已连接」却没有声音，也没有任何提示（P2-1）。
    // ★ 分开计数（2026-10-19 第六轮复核）：
    //   startFailures  = Start() 直接失败（设备一开始就打不开）
    //   runtimeFailures= 播放过程中失效（拔声卡/端点被禁用/驱动重置）
    //   两者对用户的含义完全不同，混在一起会让排障误判。
    std::atomic<int>  startFailures_{0};
    std::atomic<int>  runtimeFailures_{0};
    WasapiCaptureConfig cfg_;
};

} // namespace fxchain
