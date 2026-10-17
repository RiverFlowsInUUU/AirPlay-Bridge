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
        } else {
            taps_ = base;
        }
        // ★ 奇偶修正必须对所有路径生效，不能只放在下采样分支里。
        //
        // 抽头循环是 k = -half .. +half，窗口索引取 k + half，最大值
        // 是 2*half。而 window_ 的大小是 taps_。
        //   taps_ 为奇数：half = (taps_-1)/2，2*half = taps_-1  ≤ taps_-1  ✓
        //   taps_ 为偶数：half = taps_/2，   2*half = taps_    >  taps_-1  ✗
        //   → 每次都读 window_[taps_]，越界 1 个 float。
        //
        // 下采样时 taps_ = base/ratio + base，48k->44.1k 恰好是 66（偶数）→
        // 被这里原本的修正救下；而上采样走 else 分支、完全没有修正，
        // 于是 44100->48000 / 22050->44100 / 8000->44100 …… 全部越界。
        // 默认配置 qualityLevel=100 时 base=32（偶数），必然触发。
        //
        // 越界读到的是 vector size() 之外的内存（未初始化值或堆元数据），
        // 每个输出样点都会掺入一个不确定的抽头 → 高频失真；极端情况可能崩。
        // 这是读越界，不写坏内存，所以从未表现为崩溃、一直没被发现。
        if (taps_ > 1 && (taps_ % 2) == 0) ++taps_;
        if (taps_ > 1) {
            // 为了下采样时的抗混叠，截止频率取两者采样率的较小值
            cutoff_ = (ratio_ < 1.0) ? ratio_ : 1.0;
            buildWindow();
        }
        // ★ reset() 必须在这里再调一次 —— 上面第一次调时 taps_ 还是旧值（0），
        //   历史预填长度会按 half=0 算，之后 taps_ 变大就永远凑不够左邻，
        //   表现为「一个样点都产不出」。
        reset();
    }

    void reset() {
        // 预填 half 个零代表「开始之前的静音」，这样第一个样点就有完整左邻。
        const int half = taps_ / 2;
        histL_.assign(static_cast<size_t>(half > 0 ? half : 1), 0.0f);
        histR_.assign(static_cast<size_t>(half > 0 ? half : 1), 0.0f);
        histBaseAbs_ = -(static_cast<long long>(histL_.size()));   // 绝对位置：-half..-1
        posAbs_ = 0.0;
    }

    bool enabled() const { return taps_ > 0 && ratio_ != 1.0; }

    // 供测试检查窗口索引是否越界（见 resampler_test 的「窗口索引范围」一节）。
    // 抽头循环取 k = -half..+half、窗口索引 k+half，最大值 2*half；
    // window_ 的大小是 taps_。所以必须满足 2*(taps_/2) <= taps_-1，
    // 等价于 taps_ 为奇数。这个不变量只靠"听感"或"分块对比"都测不出来，
    // 必须显式检查（越界读到的内存内容在不同分块下可能恰好相同）。
    int taps() const { return taps_; }

    // 处理一帧数据（交错立体声 s16）。输出追加到 out。
    // inFrames 是输入帧数；返回输出帧数。
    //
    // ★ 不变量：同一条输入流，无论在哪个位置切成多少块，输出必须完全相同。
    //
    // 为此这里不用「滑窗 + 每次重算历史」的写法（那会随分块塌缩），
    // 而是维护一条连续的输入历史 + 一个绝对的读取位置：
    //   posAbs_  自会话开始以来、按输入样点计的绝对位置（可能带小数）
    //   histL_   从 histBaseAbs_ 这个绝对位置开始的输入片段
    // 产出条件：posAbs_ 左右各有 taps_/2 个真实样点。
    // 不足就等（下批数据到了再算），绝不跳过抽头 —— 跳过会让幅度随分块变化。
    size_t process(const int16_t* in, size_t inFrames, std::vector<int16_t>& out) {
        if (inFrames == 0) return 0;

        if (!enabled()) {
            const size_t before = out.size();
            out.insert(out.end(), in, in + inFrames * 2);
            return (out.size() - before) / 2;
        }

        // 追加到输入历史（保持与 histBaseAbs_ 的绝对位置一致）
        const size_t oldFrames = histL_.size();
        histL_.resize(oldFrames + inFrames);
        histR_.resize(oldFrames + inFrames);
        for (size_t i = 0; i < inFrames; ++i) {
            histL_[oldFrames + i] = in[i * 2 + 0] / 32768.0f;
            histR_[oldFrames + i] = in[i * 2 + 1] / 32768.0f;
        }

        const size_t outBefore = out.size();
        const long long availEnd = histBaseAbs_ + static_cast<long long>(histL_.size());

        const int half = taps_ / 2;
        const double step = 1.0 / ratio_;

        for (;;) {
            const long long center = static_cast<long long>(std::floor(posAbs_));
            // 左侧要有 half 个真实样点（绝对位置 >= histBaseAbs_ - 0 即为有）
            // 右侧要有 half+1 个（中心 + half 都在可用范围内）
            const long long leftNeed  = center - half;
            const long long rightNeed = center + half + 1;
            if (leftNeed < histBaseAbs_) break;      // 左侧不够 —— 等（不该发生：下面会保留足够历史）
            if (rightNeed > availEnd) break;         // 右侧不够 —— 等下一批数据

            bool allInHist = (leftNeed >= histBaseAbs_) && (rightNeed <= availEnd);
            if (!allInHist) break;

            const double frac = posAbs_ - static_cast<double>(center);
            float accL = 0.0f, accR = 0.0f, wsum = 0.0f;

            if (taps_ <= 2) {
                const size_t i0 = static_cast<size_t>(leftNeed - histBaseAbs_);
                const float f = static_cast<float>(frac);
                accL = histL_[i0] + (histL_[i0 + 1] - histL_[i0]) * f;
                accR = histR_[i0] + (histR_[i0 + 1] - histR_[i0]) * f;
            } else {
                for (int k = -half; k <= half; ++k) {
                    const long long idx = center + k;
                    const size_t i = static_cast<size_t>(idx - histBaseAbs_);
                    const double d = (posAbs_ - static_cast<double>(idx)) * cutoff_;
                    const double w = sinc(d) * window_[k + half];
                    accL += histL_[i] * static_cast<float>(w);
                    accR += histR_[i] * static_cast<float>(w);
                    wsum += static_cast<float>(w);
                }
                if (wsum != 0.0f) { accL /= wsum; accR /= wsum; }
            }

            out.push_back(clampS16(accL));
            out.push_back(clampS16(accR));
            posAbs_ += step;
        }

        // 丢弃「已确定用不到」的历史：下一次产出需要的左邻是
        // floor(posAbs_) - half，早于它的都可以删。
        const long long keepFrom = static_cast<long long>(std::floor(posAbs_)) - half;
        if (keepFrom > histBaseAbs_) {
            const long long dropN = keepFrom - histBaseAbs_;
            const size_t drop = static_cast<size_t>(
                (std::min)(dropN, static_cast<long long>(histL_.size())));
            if (drop > 0) {
                histL_.erase(histL_.begin(), histL_.begin() + static_cast<long>(drop));
                histR_.erase(histR_.begin(), histR_.begin() + static_cast<long>(drop));
                histBaseAbs_ += static_cast<long long>(drop);
            }
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
    long long histBaseAbs_ = 0;   // histL_[0] 对应的绝对样点位置
    double    posAbs_ = 0.0;      // 下一个输出样点在输入时间轴上的绝对位置
};

} // namespace fxchain
