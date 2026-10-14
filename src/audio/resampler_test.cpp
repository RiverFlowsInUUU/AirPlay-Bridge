// SPDX-License-Identifier: Apache-2.0
//
// resampler_test -- 重采样器的自测。
//
// 测量方法用「单频点 DFT」而不是逐点比对 —— 后者会被相位差干扰，
// 测出来的是相位误差不是失真。
//
// 做法：对输出信号做一次目标频点的 DFT 得到信号能量，
// 再把非目标频点的能量加起来算噪声，求信噪比（SNR）。

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

// 在指定频点做 Goertzel 滤波，返回该频点的幅度
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

// 算 SNR：目标频点幅度 vs 其余频点的总能量
double snrDb(const std::vector<int16_t>& out, uint32_t rate, double freq) {
    const size_t frames = out.size() / 2;
    const size_t skip = 2000;
    if (frames < skip * 2) return -99.0;

    std::vector<double> x;
    x.reserve(frames - skip * 2);
    for (size_t i = skip; i < frames - skip; ++i)
        x.push_back(out[i * 2] / 32768.0);

    const double signal = goertzel(x, freq, rate);

    // 扫一遍频谱，把目标频点附近以外的能量当噪声
    double noise = 0.0;
    for (double f = 100; f < rate / 2 - 100; f += 100) {
        if (std::fabs(f - freq) < 200) continue;   // 跳过目标频点
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

    {
        auto in = sine(inRate, 1000, 1.0, amp);
        Resampler r;
        r.configure(inRate, outRate, ResampleQuality::Good);
        std::vector<int16_t> out;
        size_t got = r.process(in.data(), in.size() / 2, out);
        size_t expect = (in.size() / 2) * outRate / inRate;

        double peak = 0;
        for (auto s : out) peak = (std::max)(peak, std::fabs(double(s)) / 32767.0);

        std::printf("1. 基本正确性\n");
        std::printf("   帧数 %zu（期望约 %zu）%s\n", got, expect,
                    (got > expect * 0.99 && got < expect * 1.01) ? " OK" : " FAIL");
        std::printf("   峰值 %.3f（期望 %.3f）%s\n", peak, amp,
                    (peak > amp * 0.95 && peak < amp * 1.05) ? " OK" : " FAIL");
    }

    std::printf("\n2. 信噪比对比（越高越好，单位 dB）\n");
    std::printf("   %-10s %10s %10s\n", "信号频率", "线性插值", "窗函数sinc");

    struct { double freq; const char* label; } cases[] = {
        {1000,  "1 kHz"},
        {5000,  "5 kHz"},
        {10000, "10 kHz"},
        {15000, "15 kHz"},
        {19900, "19.9kHz"},
    };

    for (auto& c : cases) {
        auto in = sine(inRate, c.freq, 0.5, amp);

        double sLin = 0, sSinc = 0;
        {
            Resampler r;
            r.configure(inRate, outRate, ResampleQuality::Linear);
            std::vector<int16_t> out;
            r.process(in.data(), in.size() / 2, out);
            sLin = snrDb(out, outRate, c.freq);
        }
        {
            Resampler r;
            r.configure(inRate, outRate, ResampleQuality::Best);
            std::vector<int16_t> out;
            r.process(in.data(), in.size() / 2, out);
            sSinc = snrDb(out, outRate, c.freq);
        }

        const char* verdict = (sSinc > sLin + 2.0) ? "  <-- sinc 明显更好" : "";
        std::printf("   %-10s %10.1f %10.1f%s\n", c.label, sLin, sSinc, verdict);
    }

    return 0;
}
