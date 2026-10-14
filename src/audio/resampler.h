// SPDX-License-Identifier: Apache-2.0
//
// resampler.h -- 音频重采样（窗函数 sinc 插值）。
//
// 为什么需要它：
//   采集端拿到的是声卡的混音格式（通常是 48000 Hz），而 AirPlay 2
//   的 ALAC 流固定 44100 Hz。引擎内部自带的重采样是两点线性插值，
//   高频段会有明显失真（15kHz 以上折叠）。
//
//   如果我们在采集侧先转好 44100，引擎就走进 pass-through 分支、
//   完全不需要它那个重采样了 —— 这是最干净的改法（不动上游代码）。
//
// 实现：
//   带限插值（windowed sinc）。相对于线性插值，它在截止频率附近
//   有真正的抗混叠能力，代价是每样本要算若干抽头。
//   48k -> 44.1k 的下采样比例接近 1，抽头数不用很多。

#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace fxchain {

// MSVC 默认不定义 kPi，自己来
constexpr double kPi = 3.14159265358979323846;

// 质量是连续的：0-100。
//
//   0      = 关闭（不做重采样，交给引擎自带的线性插值）
//   1-100  = 窗函数 sinc，抽头数从 4 线性升到 64
//
// 抽头越多，抗混叠的过渡带越窄、高频越干净，CPU 也越贵。
// 实测（48k->44.1k，19.9kHz 处信噪比）：
//   抽头  7 -> 27.8dB
//   抽头 10 -> 31.5dB
//   抽头 16 -> 38.3dB
//   抽头 22 -> 39.5dB（到顶）
// 22 往上测不出改善，所以上限定在 32 —— 覆盖整个有效区间，
// 而且让滑块的每一格都有意义（100% 不浪费在没收益的抽头上）。
constexpr int kQualityOff      = 0;
constexpr int kQualityMinTaps  = 4;
constexpr int kQualityMaxTaps  = 32;

// 立体声交错 s16 的重采样器。
//
// 用法：
//   Resampler r;
//   r.configure(48000, 44100, ResampleQuality::Good);
//   r.process(in.data(), inFrames, out);   // out 是累加的，调用方自己 clear
class Resampler {
public:
    Resampler() = default;

    // 设置输入/输出采样率与质量。会清空内部状态。
    // qualityPercent: 0 = 关闭；1-100 映射到抽头数 4..64。
    void configure(uint32_t inRate, uint32_t outRate, int qualityPercent) {
        inRate_ = inRate;
        outRate_ = outRate;
        qualityPercent_ = qualityPercent;
        reset();
        if (inRate_ == 0 || outRate_ == 0) {
            ratio_ = 1.0;
            return;
        }
        ratio_ = static_cast<double>(outRate_) / static_cast<double>(inRate_);

        // 抽头数：连续映射。qualityPercent 越低抽头越少。
        // 下采样时抽头要按比例放大，否则截止频率跟不上（会混叠）。
        int base = 0;
        if (qualityPercent_ > 0) {
            const int q = qualityPercent_ > 100 ? 100 : qualityPercent_;
            base = kQualityMinTaps +
                   (kQualityMaxTaps - kQualityMinTaps) * q / 100;
        }
        if (base > 0 && ratio_ < 1.0) {
            // 下采样：抽头数除以比例，保证过渡带够窄
            taps_ = static_cast<int>(base / ratio_) + base;
            if (taps_ % 2 == 0) ++taps_;   // 用奇数，中心对称
        } else {
            taps_ = base;
        }
        if (taps_ > 1) {
            // 为了下采样时的抗混叠，截止频率取两者采样率的较小值
            cutoff_ = (ratio_ < 1.0) ? ratio_ : 1.0;
            buildWindow();
        }
    }

    void reset() {
        histL_.assign(static_cast<size_t>(taps_ > 0 ? taps_ : 1), 0.0f);
        histR_.assign(static_cast<size_t>(taps_ > 0 ? taps_ : 1), 0.0f);
        pos_ = 0.0;
    }

    bool enabled() const { return taps_ > 0 && ratio_ != 1.0; }

    // 处理一帧数据（交错立体声 s16）。输出追加到 out。
    // inFrames 是输入帧数；返回输出帧数。
    size_t process(const int16_t* in, size_t inFrames, std::vector<int16_t>& out) {
        if (inFrames == 0) return 0;

        if (!enabled()) {
            // 直通
            const size_t before = out.size();
            out.insert(out.end(), in, in + inFrames * 2);
            return (out.size() - before) / 2;
        }

        // 把新数据推进历史缓冲（每个通道一条）
        const size_t total = histL_.size() + inFrames;
        std::vector<float> bufL(total, 0.0f), bufR(total, 0.0f);
        std::copy(histL_.begin(), histL_.end(), bufL.begin());
        std::copy(histR_.begin(), histR_.end(), bufR.begin());
        for (size_t i = 0; i < inFrames; ++i) {
            bufL[histL_.size() + i] = in[i * 2 + 0] / 32768.0f;
            bufR[histL_.size() + i] = in[i * 2 + 1] / 32768.0f;
        }

        const size_t histLen = histL_.size();
        const size_t outBefore = out.size();

        // 逐输出样本：在输入时间轴上取 sinc 加权和
        for (;;) {
            const double srcIdx = pos_;
            const long center = static_cast<long>(std::floor(srcIdx));
            if (center + 1 >= static_cast<long>(total)) break;   // 需要下一个样本才能插值

            float accL = 0.0f, accR = 0.0f, wsum = 0.0f;

            if (taps_ <= 2) {
                // 线性
                const double frac = srcIdx - center;
                const size_t i0 = static_cast<size_t>(center);
                accL = static_cast<float>(bufL[i0] + (bufL[i0 + 1] - bufL[i0]) * frac);
                accR = static_cast<float>(bufR[i0] + (bufR[i0 + 1] - bufR[i0]) * frac);
            } else {
                const int half = taps_ / 2;
                for (int k = -half; k <= half; ++k) {
                    const long idx = center + k;
                    if (idx < 0 || idx >= static_cast<long>(total)) continue;
                    const double d = (srcIdx - static_cast<double>(idx)) * cutoff_;
                    const double w = sinc(d) * window_[k + half];
                    accL += static_cast<float>(bufL[idx] * w);
                    accR += static_cast<float>(bufR[idx] * w);
                    wsum  += static_cast<float>(w);
                }
                if (wsum != 0.0f) {
                    accL /= wsum;
                    accR /= wsum;
                }
            }

            out.push_back(clampS16(accL));
            out.push_back(clampS16(accR));

            pos_ += 1.0 / ratio_;
        }

        // 把已消费的部分从历史里滑出去，保留尾部供下次插值
        const long consumed = static_cast<long>(std::floor(pos_));
        if (consumed > 0) {
            const size_t keep = histLen;
            const size_t newStart = static_cast<size_t>(consumed);
            if (newStart <= total) {
                std::vector<float> tailL, tailR;
                const size_t from = (std::min)(newStart, total);
                const size_t cnt = (std::min)(keep, total - from);
                tailL.assign(bufL.begin() + from, bufL.begin() + from + cnt);
                tailR.assign(bufR.begin() + from, bufR.begin() + from + cnt);
                histL_ = tailL;
                histR_ = tailR;
                pos_ -= static_cast<double>(consumed);
            }
        }
        if (histL_.empty()) {
            histL_.assign(1, 0.0f);
            histR_.assign(1, 0.0f);
        }

        return (out.size() - outBefore) / 2;
    }

private:
    static float sinc(double x) {
        if (std::fabs(x) < 1e-9) return 1.0f;
        const double px = kPi * x;
        return static_cast<float>(std::sin(px) / px);
    }

    static int16_t clampS16(float v) {
        if (v > 1.0f)  v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        return static_cast<int16_t>(std::lround(v * 32767.0f));
    }

    // Blackman 窗，压制旁瓣（减少振铃）
    void buildWindow() {
        window_.resize(static_cast<size_t>(taps_));
        const int n = taps_ - 1;
        for (int i = 0; i < taps_; ++i) {
            const double x = static_cast<double>(i) / (n > 0 ? n : 1);
            window_[i] = static_cast<float>(
                0.42 - 0.5 * std::cos(2.0 * kPi * x) + 0.08 * std::cos(4.0 * kPi * x));
        }
    }

    uint32_t inRate_ = 0;
    uint32_t outRate_ = 0;
    int qualityPercent_ = 100;   // 0 = 关，1-100 连续
    double ratio_ = 1.0;
    double cutoff_ = 1.0;
    int taps_ = 0;

    std::vector<float> window_;
    std::vector<float> histL_, histR_;
    double pos_ = 0.0;
};

} // namespace fxchain
