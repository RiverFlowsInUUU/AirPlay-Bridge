// SPDX-License-Identifier: Apache-2.0
//
// wasapi_capture.h -- 用 WASAPI 环回模式采集 Windows 的输出声音。
//
// 两种模式：
//   AllDevices  采集"所有活动的输出设备"并混音（默认）—— 这样即使
//               某个应用被单独指到了别的声卡（Windows 支持按应用选输出），
//               它的声音也能被抓到，用户不用去调音量合成器。
//   DefaultOnly 只采集系统默认输出设备（旧行为，保留备用）。
//
// 输出统一为交错 16-bit 立体声。

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
    AllDevices,    // 所有活动输出设备混音（默认，最省心）
    DefaultOnly,   // 仅默认输出设备
};

struct WasapiCaptureConfig {
    // 默认单设备（默认输出设备）。多设备混音实现有未解决的问题，
    // 见 wasapi_capture.cpp 里的说明；接管模式下也不需要。
    CaptureMode mode = CaptureMode::DefaultOnly;
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

    bool isRunning() const { return running_.load(); }

    // 实际打开的设备数（AllDevices 模式下可能大于 1）
    size_t deviceCount() const;

    // 采集到的设备名列表（用于日志）
    std::vector<std::string> deviceNames() const;

    // 所有输出设备共用的采样率（取第一个设备的；不同设备会做简单适配）
    uint32_t deviceSampleRate() const;
    uint32_t deviceChannels() const;

    // 调试用：列出所有活动输出设备名
    static std::vector<std::string> listRenderDevices();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    WasapiCaptureConfig cfg_;
};

} // namespace fxchain
