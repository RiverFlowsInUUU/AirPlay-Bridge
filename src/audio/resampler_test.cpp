// SPDX-License-Identifier: Apache-2.0
//
// resampler_test -- 重采样器的自测。
//
// 测两件事：
//   1. 基本正确性：帧数对得上、幅度不失真
//   2. 质量曲线：品质 0-100 各档在高频下的信噪比
//      （用来确认"连续可调"是真有意义，不是摆设）
//
// 测量用单频点 DFT（Goertzel），不用逐点比对 —— 后者测出来的是
// 相位误差不是失真。
//
// 注意：这个测量的噪声底受限于频谱泄漏（探针频率是任意 bin，
// 纯音的泄漏会被算进噪声），所以绝对值偏低。但不同品质之间用
// 同一种方法测，相减比较是有效的。

#include "resampler.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <vector>

using namespace fxchain;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> sine(uint32_t rate, double freq, double seconds, double amp) {
    const size_t frames = static_cast<size_t>(rate * seconds);
    std::vector<int16_t> out(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double v = std::sin(2 * kPi * freq * i / rate) * amp;
        out[i * 2 + 0] = out[i * 2 + 1] = static_cast<int16_t>(v * 32767);
    }
    return out;
}

double goertzel(const std::vector<double>& x, double freq, uint32_t rate) {
    const size_t n = x.size();
    const double k = 0.5 + n * freq / rate;
    const double omega = 2.0 * kPi * k / n;
    const double coeff = 2.0 * std::cos(omega);
    double s0 = 0, s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        s0 = x[i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double real = s1 - s2 * std::cos(omega);
    const double imag = s2 * std::sin(omega);
    return 2.0 * std::sqrt(real * real + imag * imag) / n;
}

double snrDb(const std::vector<int16_t>& out, uint32_t rate, double freq) {
    const size_t frames = out.size() / 2;
    const size_t skip = 2000;
    if (frames < skip * 2) return -99.0;

    std::vector<double> x;
    x.reserve(frames - skip * 2);
    for (size_t i = skip; i < frames - skip; ++i)
        x.push_back(out[i * 2] / 32768.0);

    const double signal = goertzel(x, freq, rate);

    double noise = 0.0;
    for (double f = 100; f < rate / 2 - 100; f += 100) {
        if (std::fabs(f - freq) < 200) continue;
        const double a = goertzel(x, f, rate);
        noise += a * a;
    }
    noise = std::sqrt(noise);

    if (noise < 1e-12) return 99.0;
    return 20.0 * std::log10(signal / noise);
}

} // namespace

int main() {
    const uint32_t inRate = 48000, outRate = 44100;
    const double amp = 0.8;

    std::printf("重采样器自测（%u Hz -> %u Hz）\n\n", inRate, outRate);

    // ---- 1. 基本正确性 ----
    {
        auto in = sine(inRate, 1000, 1.0, amp);
        Resampler r;
        r.configure(inRate, outRate, 100);
        std::vector<int16_t> out;
        const size_t got = r.process(in.data(), in.size() / 2, out);
        const size_t expect = (in.size() / 2) * outRate / inRate;

        double peak = 0;
        for (auto s : out) peak = (std::max)(peak, std::fabs(double(s)) / 32767.0);

        std::printf("1. 基本正确性\n");
        std::printf("   帧数 %zu（期望约 %zu）%s\n", got, expect,
                    (got > expect * 0.99 && got < expect * 1.01) ? " OK" : " FAIL");
        std::printf("   峰值 %.3f（期望 %.3f）%s\n", peak, amp,
                    (peak > amp * 0.95 && peak < amp * 1.05) ? " OK" : " FAIL");
    }

    // ---- 2. 质量曲线 ----
    std::printf("\n2. 品质 -> 质量曲线（19.9kHz 处信噪比）\n");
    std::printf("   %-8s %-8s %12s\n", "品质", "抽头数", "信噪比dB");

    double prev = -999;
    for (int q : {0, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100}) {
        auto in = sine(inRate, 19900, 0.5, amp);
        Resampler r;
        r.configure(inRate, outRate, q);

        if (!r.enabled()) {
            std::printf("   %-8d %-8s %12s\n", q, "-", "关闭");
            continue;
        }

        std::vector<int16_t> out;
        r.process(in.data(), in.size() / 2, out);
        const double snr = snrDb(out, outRate, 19900);

        const int taps = kQualityMinTaps +
                         (kQualityMaxTaps - kQualityMinTaps) * q / 100;
        const char* mark = (prev > -900 && snr - prev < 0.3) ? "  <- 差不多到顶了" : "";
        std::printf("   %-8d %-8d %12.1f%s\n", q, taps, snr, mark);
        prev = snr;
    }

    return 0;
}
