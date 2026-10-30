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

static int totalFailures = 0;

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

        // ★ 失败必须计入退出码。以前只 printf FAIL —— 帧数/幅度回归时
        //   CTest 照样绿，第 1 节形同虚设（2026-10-31 外部审查 P2-4）。
        int sec1 = 0;
        if (!(got > expect * 0.99 && got < expect * 1.01)) ++sec1;
        if (!(peak > amp * 0.95 && peak < amp * 1.05)) ++sec1;
        std::printf("1. 基本正确性\n");
        std::printf("   帧数 %zu（期望约 %zu）%s\n", got, expect,
                    (got > expect * 0.99 && got < expect * 1.01) ? " OK" : " FAIL");
        std::printf("   峰值 %.3f（期望 %.3f）%s\n", peak, amp,
                    (peak > amp * 0.95 && peak < amp * 1.05) ? " OK" : " FAIL");
        totalFailures += sec1;
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

        // 打印真实抽头数：下采样时按比例放大（resampler.h:70-77 的公式），
        // 以前用 base 公式打印，48k->44.1k 时低报约一倍（P3-5）。
        const int baseT = kQualityMinTaps +
                          (kQualityMaxTaps - kQualityMinTaps) * q / 100;
        const int taps = (baseT > 0 && outRate < inRate)
                             ? int(baseT * inRate / outRate) + baseT : baseT;
        const char* mark = (prev > -900 && snr - prev < 0.3) ? "  <- 差不多到顶了" : "";
        std::printf("   %-8d %-8d %12.1f%s\n", q, taps, snr, mark);
        prev = snr;
    }

    // ---- 3. 分块不变性（本测试的核心）----
    //
    // 同一条输入流，无论按多大的块喂进来，输出必须逐样点完全相同。
    // 这是流式重采样器最基本的正确性要求 —— 而线上 WASAPI 回调的帧数
    // 本来就不固定（常见 480 帧），所以这不只是理论要求。
    //
    // 历史教训（2026-10-17）：修前「一口气喂完」与「分 480 帧喂」逐样点
    // 最大差 3582（满量程 32767 的 10.9%），80% 的样点差异 > 1000。
    // 根因是历史缓冲长度随分块塌缩（67 -> 1），抽头大量落在窗口外。
{
        std::printf("\n3. 分块不变性（同一条输入，不同分块必须完全一致）\n");

        // ★ 必须遍历多组采样率，不能只测产品当前默认的那一组。
        //
        // 曾经这里硬编码 48000 -> 44100（唯一的生产路径），于是上采样方向
        // 完全没被覆盖 —— 而 taps_ 为偶数时 window_[k + half] 会越界读 1 个元素，
        // 恰好只在「上采样 + taps_ 为偶数」时发生。
        //
        // 教训：测试要覆盖被测对象的全部方向与量程，不能只测当前配置走的那条路。
        struct RateCase { uint32_t in, out; const char* dir; };
        const RateCase rates[] = {
            {48000,  44100, "下采样"},
            {96000,  44100, "下采样"},
            {192000, 44100, "下采样"},
            {44100,  48000, "上采样"},
            {22050,  44100, "上采样"},
            {16000,  44100, "上采样"},
            {8000,   44100, "上采样"},
            {32000,  44100, "上采样"},
        };

        const int chunks[] = {1, 2, 7, 64, 480, 882, 1024, 4096};
        int failures = 0;
        int combos = 0;

        for (const auto& rc : rates) {
            const uint32_t inRate = rc.in, outRate = rc.out;
            const int N = static_cast<int>(inRate);   // 各喂 1 秒

            std::vector<int16_t> in(static_cast<size_t>(N) * 2);
            for (int i = 0; i < N; ++i) {
                // 用带谐波的信号，单频测不出相位错误
                const double t = 2.0 * 3.14159265358979 * i / inRate;
                const double v = 0.5 * std::sin(t * 440.0) +
                                 0.25 * std::sin(t * 3000.0) +
                                 0.1  * std::sin(t * 12000.0);
                in[i * 2] = in[i * 2 + 1] = static_cast<int16_t>(std::lround(v * 32767));
            }

            auto runChunked = [&](int chunk, std::vector<int16_t>& out) {
                Resampler r;
                r.configure(inRate, outRate, 100);
                for (int off = 0; off < N; off += chunk)
                    r.process(in.data() + static_cast<size_t>(off) * 2,
                              static_cast<size_t>(std::min(chunk, N - off)), out);
            };

            std::vector<int16_t> ref;
            runChunked(N, ref);                    // 基准：一口气喂完

            std::printf("   %6u -> %-6u (%s)  基准帧数 %zu\n",
                        rc.in, rc.out, rc.dir, ref.size() / 2);

            for (int c : chunks) {
                std::vector<int16_t> got;
                runChunked(c, got);

                long maxDiff = 0;
                if (got.size() != ref.size()) {
                    maxDiff = 999999;
                } else {
                    for (size_t i = 0; i < ref.size(); ++i) {
                        const long d = std::labs(static_cast<long>(ref[i]) -
                                                 static_cast<long>(got[i]));
                        if (d > maxDiff) maxDiff = d;
                    }
                }
                ++combos;
                if (maxDiff != 0) {
                    ++failures;
                    std::printf("      分块 %-6d 帧数 %-7zu 差异 %ld  FAIL\n",
                                c, got.size() / 2, maxDiff);
                }
            }
            std::printf("      %s\n", failures ? "有分块不一致" : "全部一致");
        }

        if (failures) {
            std::printf("   %d/%d 个组合结果不一致 —— 重采样器会随分块/采样率变音质\n",
                        failures, combos);
            return 1;
        }
        std::printf("   全部一致（%d 个组合）\n", combos);
    }

    // ---- 4. 窗口索引范围（越界读的显式检查）----
    //
    // 为什么「分块不变性」查不出这个：越界读的是 window_[taps_]，
    // 那块内存在两种分块下是同一块堆内存、内容恰好相同，
    // 所以逐样点对比会显示"完全一致"—— 但值本身是垃圾。
    //
    // 唯一可靠的判据是直接检查索引范围：
    //   抽头循环 k = -half..+half，窗口索引 k+half，最大值 2*half
    //   而 window_ 的大小是 taps_，必须 2*half <= taps_-1，即 taps_ 为奇数。
    //
    // 历史：configure() 的奇偶修正曾只放在下采样分支里，
    // 上采样时 taps_ 为偶数 → 每次读 window_[taps_] → 越界 1 个 float。
    // 默认 qualityLevel=100 时 base=32（偶数），必然触发。
    {
        std::printf("\n4. 窗口索引范围（2*(taps/2) 必须 <= taps-1）\n");

        int bad = 0;
        for (uint32_t in : {8000u, 16000u, 22050u, 32000u, 44100u, 48000u, 96000u, 192000u}) {
            for (uint32_t out : {44100u, 48000u}) {
                if (in == out) continue;
                for (int q : {1, 10, 25, 50, 75, 100}) {
                    Resampler r;
                    r.configure(in, out, q);
                    const int taps = r.taps();
                    if (taps <= 0) continue;
                    const int maxWinIdx = 2 * (taps / 2);
                    if (maxWinIdx > taps - 1) {
                        ++bad;
                        std::printf("   %6u -> %-6u q=%-3d taps=%-4d 最大窗口索引=%d > %d  OVERFLOW\n",
                                    in, out, q, taps, maxWinIdx, taps - 1);
                    }
                }
            }
        }
        if (bad) {
            std::printf("   %d 个组合会越界读 window_\n", bad);
            return 1;
        }
        std::printf("   全部安全\n");
    }

    // 各节失败统一计入退出码（P2-4）。
    if (totalFailures) {
        std::printf("\n共 %d 项失败\n", totalFailures);
        return 1;
    }
    return 0;
}
