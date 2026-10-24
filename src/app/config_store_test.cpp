// SPDX-License-Identifier: Apache-2.0
//
// config_store_test -- 配置读写 + 钳制的自测。
//
// 为什么专门给这个文件写测试：**配置那条链坏过两次**，
// 而且两次都是同一类病 —— 用户存下的值在某个环节被悄悄改掉：
//   v1.5.4   延迟被一条迁移逻辑强制升到 250ms（"低于可靠下限"），
//            但那个前提（150ms 必然爆音）后来被两个修复推翻了
//   v1.5.10  音量有两份副本，改了一份、另一份没同步
//
// 它同时又有个好性质：**纯文件读写、不需要声卡/网络/设备**，
// 几秒钟就能跑完 —— 这类测试性价比最高。
//
// 核心断言是第 6 节：**界内的值一个都不许被改**。
// 「钳制」和「纠偏」只差一线：出界的拉回来是保护，界内的动一下就是篡改。

#include "config_store.h"

#include <filesystem>
#include <chrono>
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace fxchain;

namespace {

int g_fail = 0;
int g_checks = 0;

void check(bool condValue, const std::string& what) {
    ++g_checks;
    if (!condValue) {
        ++g_fail;
        std::printf("      FAIL  %s\n", what.c_str());
    }
}

void checkInt(int got, int want, const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_fail;
        std::printf("      FAIL  %s：得到 %d，期望 %d\n", what.c_str(), got, want);
    }
}

void checkStr(const std::string& got, const std::string& want,
              const std::string& what) {
    ++g_checks;
    if (got != want) {
        ++g_fail;
        std::printf("      FAIL  %s：得到 \"%s\"，期望 \"%s\"\n",
                    what.c_str(), got.c_str(), want.c_str());
    }
}

void writeRaw(const std::string& path, const std::string& content) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (f) {
        std::fwrite(content.data(), 1, content.size(), f);
        std::fclose(f);
    }
}

void removeFile(const std::string& path) { std::remove(path.c_str()); }

std::string readRaw(const std::string& path) {
    std::string out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

}  // namespace

int main() {
    // ★ 用「临时目录 + 本次进程唯一名」，不用固定的相对文件名。
    //   原来写死在 CWD 里叫 config_store_test_tmp.json —— 两个人（或两个
    //   CI job）同时跑 ctest 会互相删/覆盖同一个文件。实测：
    //   并行跑两个实例 → 14/78 和 29/76 项失败（全是误报）。
    //   2026-10-18 第四轮外部审查 P3-5。
    const std::string kFile = []() {
        std::error_code ec;
        std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
        if (ec) dir = std::filesystem::current_path(ec);
        const auto unique = static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()) ^
            static_cast<unsigned long long>(GetCurrentProcessId());
        std::filesystem::path p = dir / ("airplay_bridge_cfgtest_" +
                                         std::to_string(unique) + ".json");
        // 窄路径：本测试覆盖的就是窄 API 行为，用 .string() 保持一致。
        return p.string();
    }();

    // ---- 1. 往返一致：每个字段都要回来 ----
    std::printf("\n1. 往返一致（save -> load，每个字段）\n");
    {
        BridgeConfig a;
        a.deviceId        = "02:47:5C:A1:D8:3C";
        a.lastDeviceId    = "FE:DF:0E:B4:C9:60";
        a.lastDeviceName  = "阳宝のHomePod mini";
        a.lastHost        = "192.168.2.132";
        a.volume          = 5;
        a.latencyMs       = 200;
        a.qualityLevel    = 100;
        a.preCaptureDevice = "扬声器 (Realtek Audio)";
        a.autoReconnect   = false;
        a.hideAppleTV     = false;
        a.standbyEnabled  = false;
        a.standbyTenths   = 150;
        a.askedAutoStart  = true;
        a.muteLocalOnConnect = false;
        a.captureDeviceId = "\\?\\swd#mmdevapi#mid";
        a.localMutedByUs  = true;
        a.language        = "zh";
        a.ignoredDeviceIds = {"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"};

        check(saveConfig(kFile, a), "saveConfig 应成功");

        BridgeConfig b;   // 全新默认值
        check(loadConfig(kFile, b), "loadConfig 应成功");

        checkStr(b.deviceId, a.deviceId, "deviceId");
        checkStr(b.lastDeviceId, a.lastDeviceId, "lastDeviceId");
        checkStr(b.lastDeviceName, a.lastDeviceName, "lastDeviceName");
        checkStr(b.lastHost, a.lastHost, "lastHost");
        checkInt(b.volume, a.volume, "volume");
        checkInt(b.latencyMs, a.latencyMs, "latencyMs");
        checkInt(b.qualityLevel, a.qualityLevel, "qualityLevel");
        checkStr(b.preCaptureDevice, a.preCaptureDevice, "preCaptureDevice");
        check(b.autoReconnect == a.autoReconnect, "autoReconnect");
        check(b.hideAppleTV == a.hideAppleTV, "hideAppleTV");
        check(b.standbyEnabled == a.standbyEnabled, "standbyEnabled");
        checkInt(b.standbyTenths, a.standbyTenths, "standbyTenths");
        check(b.askedAutoStart == a.askedAutoStart, "askedAutoStart");
        check(b.muteLocalOnConnect == a.muteLocalOnConnect, "muteLocalOnConnect");
        checkStr(b.captureDeviceId, a.captureDeviceId, "captureDeviceId");
        check(b.localMutedByUs == a.localMutedByUs, "localMutedByUs");
        checkStr(b.language, a.language, "language");
        check(b.ignoredDeviceIds.size() == a.ignoredDeviceIds.size(),
              "ignoredDeviceIds 条数");
        if (b.ignoredDeviceIds.size() == a.ignoredDeviceIds.size()) {
            for (size_t i = 0; i < a.ignoredDeviceIds.size(); ++i) {
                checkStr(b.ignoredDeviceIds[i], a.ignoredDeviceIds[i],
                         "ignoredDeviceIds[" + std::to_string(i) + "]");
            }
        }
        removeFile(kFile);
    }

    // ---- 2. 特殊字符：中文 / 引号 / 反斜杠都要能原样回来 ----
    // 设备名是中文（"阳宝のHomePod mini"）—— 转义写错的话名字就毁了。
    std::printf("\n2. 特殊字符（中文 / 引号 / 反斜杠）\n");
    {
        BridgeConfig a;
        a.lastDeviceName = "阳宝\"の\"HomePod \\ mini";   // 含引号与反斜杠
        a.lastHost       = "10.0.0.1";
        saveConfig(kFile, a);

        BridgeConfig b;
        loadConfig(kFile, b);
        checkStr(b.lastDeviceName, a.lastDeviceName, "含引号/反斜杠的设备名");
        removeFile(kFile);
    }

    // ---- 3. 文件不存在：返回 false，且不动 out ----
    std::printf("\n3. 文件不存在（返回 false，out 保持原样）\n");
    {
        removeFile(kFile);
        BridgeConfig def;
        def.volume    = 42;
        def.latencyMs = 177;
        def.language  = "zh";

        BridgeConfig out = def;
        check(!loadConfig(kFile, out), "缺文件应返回 false");
        checkInt(out.volume, 42, "缺文件时 volume 不该被动");
        checkInt(out.latencyMs, 177, "缺文件时 latencyMs 不该被动");
        checkStr(out.language, "zh", "缺文件时 language 不该被动");
    }

    // ---- 4. 空文件：同样返回 false ----
    std::printf("\n4. 空文件\n");
    {
        writeRaw(kFile, "");
        BridgeConfig out;
        check(!loadConfig(kFile, out), "空文件应返回 false");
        removeFile(kFile);
    }

    // ---- 4b. ★ 正式文件损坏时，从 .bak 恢复上一次的完好内容 ----
    //
    // 这是 2026-10-23 外部审查的 P3：原来损坏只留证、不恢复，
    // 于是 deviceId 重新生成（接收端把电脑当新设备）、设置全部回落。
    // 而一份完好的旧配置就在 .bak 里。
    std::printf("\n4b. ★ 损坏时从 .bak 恢复\n");
    {
        removeFile(kFile);
        removeFile(kFile + ".bak");

        // 第一次保存：.bak 还生成不出来（正式文件不存在），正常
        BridgeConfig first;
        first.volume       = 33;
        first.lastDeviceId = "DEV-AAA";
        first.language     = "en";
        check(saveConfig(kFile, first), "第一次保存应成功");

        // 第二次保存：这次会把上一份复制成 .bak
        BridgeConfig second;
        second.volume       = 66;
        second.lastDeviceId = "DEV-BBB";
        second.language     = "zh";
        check(saveConfig(kFile, second), "第二次保存应成功");

        // 现在 .bak 里应该是**第一次**那份（DEV-AAA / volume 33）
        {
            BridgeConfig bak;
            check(loadConfig(kFile + ".bak", bak), ".bak 应可独立加载");
            checkStr(bak.lastDeviceId, "DEV-AAA", ".bak 里是上一份的内容");
            checkInt(bak.volume, 33, ".bak 里是上一份的 volume");
        }

        // 把正式文件写坏（模拟断电留下的半份 JSON）
        writeRaw(kFile, "{\"lastDeviceId\": \"DEV-BBB\", \"volume\": 66");

        BridgeConfig out;
        // ★ 关键断言：能加载，且拿到的是 .bak 里那台设备的身份
        check(loadConfig(kFile, out), "损坏时应从 .bak 恢复成功");
        checkStr(out.lastDeviceId, "DEV-AAA", "恢复出的是 .bak 里的 deviceId");
        checkInt(out.volume, 33, "恢复出的是 .bak 里的 volume");

        // ★ 恢复必须写回主文件 —— 否则下次启动又读到坏文件
        //   （2026-10-24 复查指出"自消耗"，这一条正是它的回归测试）
        //
        //   注意这条断言必须**绕过 .bak** 才有效：只要 .bak 还在，
        //   下次 load 仍能从它恢复，看不出主文件到底修没修好。
        //   所以这里读完 .bak 的内容后把它删掉，再读主文件 ——
        //   模拟"恢复后没再保存过就退出，而 .bak 因为别的原因没了"
        //   那种链式场景。
        {
            // 先看主文件当前的实际字节（不经过任何恢复逻辑）
            std::string mainNow;
            {
                std::ifstream mf(kFile, std::ios::binary);
                std::stringstream ms;
                ms << mf.rdbuf();
                mainNow = ms.str();
            }
            check(mainNow.find("DEV-AAA") != std::string::npos,
                  "恢复后主文件里应写着 DEV-AAA（直接读字节）");

            std::string bakSnap;
            {
                std::ifstream bf(kFile + ".bak", std::ios::binary);
                std::stringstream bs;
                bs << bf.rdbuf();
                bakSnap = bs.str();
            }
            removeFile(kFile + ".bak");   // 备份没了，只剩主文件

            BridgeConfig again;
            check(loadConfig(kFile, again), "备份没了也能从主文件读出来");
            checkStr(again.lastDeviceId, "DEV-AAA",
                     "恢复后的主文件内容就是 .bak 那份");

            // 还原 .bak，后面还要验它没被毁
            writeRaw(kFile + ".bak", bakSnap);
        }

        // ★ .bak 不能被这次恢复毁掉（留证改写到 .corrupt）
        {
            BridgeConfig bak2;
            check(loadConfig(kFile + ".bak", bak2), ".bak 恢复后仍应可用");
            checkStr(bak2.lastDeviceId, "DEV-AAA", ".bak 仍是好的");
        }

        // 坏内容留证在 .corrupt
        {
            std::ifstream cf(kFile + ".corrupt", std::ios::binary);
            check(static_cast<bool>(cf), "坏内容应留在 .corrupt 里");
        }

        removeFile(kFile);
        removeFile(kFile + ".bak");
        removeFile(kFile + ".corrupt");
    }

    // ---- 4c. 没有 .bak 时，损坏仍按失败处理（不硬编造配置）----
    std::printf("\n4c. 无 .bak 时损坏仍失败\n");
    {
        removeFile(kFile);
        removeFile(kFile + ".bak");
        writeRaw(kFile, "{\"volume\": 50");
        BridgeConfig out;
        out.volume = 77;
        check(!loadConfig(kFile, out), "无 .bak 时损坏仍应返回 false");
        checkInt(out.volume, 77, "失败时不该改动传入值");
        removeFile(kFile);
        removeFile(kFile + ".bak");
    }

    // ---- 5. 垃圾内容：不崩，缺的键保持传入值 ----
    std::printf("\n5. 垃圾内容（不崩，缺键保持传入值）\n");
    {
        writeRaw(kFile, "这不是 JSON {{{ 乱码 \x01\x02");
        BridgeConfig out;
        out.volume    = 64;
        out.latencyMs = 188;
        loadConfig(kFile, out);            // 返回值不强求
        checkInt(out.volume, 64, "乱码时 volume 保持传入值");
        checkInt(out.latencyMs, 188, "乱码时 latencyMs 保持传入值");
        removeFile(kFile);
    }

    // ---- 6. ★ 界内的值一个都不许被改 ----
    // 这是 v1.5.4 那次事故的核心：一条「纠偏」逻辑把用户存的 150ms 改成 250ms。
    std::printf("\n6. ★ 界内的值不许被改（v1.5.4 回归）\n");
    {
        const int vols[] = {0, 5, 30, 50, 70, 100};
        for (int v : vols) {
            writeRaw(kFile, "{\"volume\": " + std::to_string(v) +
                            ", \"latencyMs\": 200}");
            BridgeConfig out;
            loadConfig(kFile, out);
            checkInt(out.volume, v, "界内 volume=" + std::to_string(v));
        }
        const int lats[] = {100, 130, 150, 200, 250, 300, 1000};
        for (int l : lats) {
            writeRaw(kFile, "{\"volume\": 5, \"latencyMs\": " + std::to_string(l) + "}");
            BridgeConfig out;
            loadConfig(kFile, out);
            checkInt(out.latencyMs, l, "界内 latencyMs=" + std::to_string(l));
        }
        removeFile(kFile);
    }

    // ---- 7. 界外的值被拉回范围内 ----
    std::printf("\n7. 界外的值被拉回\n");
    {
        writeRaw(kFile, "{\"volume\": 150, \"latencyMs\": 50}");
        BridgeConfig out;
        loadConfig(kFile, out);
        checkInt(out.volume, 100, "volume 150 -> 100");
        checkInt(out.latencyMs, 100, "latencyMs 50 -> 100");

        writeRaw(kFile, "{\"volume\": -5, \"latencyMs\": 5000}");
        BridgeConfig o2;
        loadConfig(kFile, o2);
        checkInt(o2.volume, 0, "volume -5 -> 0");
        checkInt(o2.latencyMs, 1000, "latencyMs 5000 -> 1000");
        removeFile(kFile);
    }

    // ---- 8. 未知键被忽略（向前兼容：新版本写的文件被老版本读）----
    std::printf("\n8. 未知键被忽略\n");
    {
        writeRaw(kFile,
                 "{\"volume\": 5, \"latencyMs\": 200, "
                 "\"futureField\": 12345, \"anotherNew\": \"x\"}");
        BridgeConfig out;
        out.volume    = 5;
        out.latencyMs = 200;
        check(loadConfig(kFile, out), "含未知键仍应加载成功");
        checkInt(out.volume, 5, "未知键不影响 volume");
        checkInt(out.latencyMs, 200, "未知键不影响 latencyMs");
        removeFile(kFile);
    }

    // ---- 9. 老配置迁移：quality(0-3) -> qualityLevel(0-100) ----
    // 不换算的话，老的「3=最好」会被当成「3/100=很差」。
    std::printf("\n9. 老字段迁移 quality -> qualityLevel\n");
    {
        const int  legacy[] = {0, 1, 2, 3};
        const int  expect[] = {0, 30, 60, 100};
        for (int i = 0; i < 4; ++i) {
            writeRaw(kFile, "{\"volume\": 5, \"latencyMs\": 200, \"quality\": " +
                            std::to_string(legacy[i]) + "}");
            BridgeConfig out;
            loadConfig(kFile, out);
            checkInt(out.qualityLevel, expect[i],
                     "quality " + std::to_string(legacy[i]) + " -> qualityLevel");
        }
        removeFile(kFile);
    }

    // ---- 10. 单个键缺失：该字段保持传入值，其它正常加载 ----
    std::printf("\n10. 单个键缺失\n");
    {
        writeRaw(kFile, "{\"volume\": 70}");
        BridgeConfig out;
        out.latencyMs = 313;
        out.language  = "zh";
        check(loadConfig(kFile, out), "应加载成功");
        checkInt(out.volume, 70, "有值的键正常加载");
        checkInt(out.latencyMs, 313, "缺的键保持传入值");
        checkStr(out.language, "zh", "缺的键保持传入值（language）");

        // 另一半契约：键在、但值为空 —— 这时才该回退 "en"
        // （空语言会让界面一条词条都匹配不上）。
        writeRaw(kFile, "{\"volume\": 70, \"language\": \"\"}");
        BridgeConfig o3;
        o3.language = "zh";
        loadConfig(kFile, o3);
        checkStr(o3.language, "en", "键在但为空 -> 回退 en");
        removeFile(kFile);
    }

    // ---- 11. 写出来的确实是文件（不是只改了内存）----
    std::printf("\n11. 落盘内容可读\n");
    {
        BridgeConfig a;
        a.volume = 5;
        saveConfig(kFile, a);
        const std::string raw = readRaw(kFile);
        check(!raw.empty(), "文件非空");
        check(raw.find("\"volume\"") != std::string::npos, "含 volume 键");
        removeFile(kFile);
    }

    std::printf("\n");
    // ---- 12. ★ 写入是原子的：不留 .tmp、正式文件完整 ----
    // 触发场景：写在半路被杀（用户强杀 / 断电）→ 半份 JSON →
    // loadConfig 原来只看「非空」就当成有效配置（P1-2，外部审查）。
    {
        std::printf("\n12. 写入原子性\n");
        removeFile(kFile);
        removeFile(kFile + ".tmp");

        BridgeConfig a;
        a.volume = 37;
        a.latencyMs = 188;
        a.lastDeviceName = "测试设备";
        check(saveConfig(kFile, a), "saveConfig 成功");

        // ① 临时文件不该留下
        std::FILE* t = std::fopen((kFile + ".tmp").c_str(), "rb");
        check(t == nullptr, "写完不留 .tmp");
        if (t) std::fclose(t);

        // ② 正式文件完整（首尾花括号）
        std::string raw = readRaw(kFile);
        // 末尾有换行 —— loadConfig 里也是先 trim 再判首尾，这里跟着做。
        while (!raw.empty() && (raw.back() == 10 || raw.back() == 13 ||
                                raw.back() == 32 || raw.back() == 9))
            raw.pop_back();
        check(!raw.empty() && raw.front() == char(123) && raw.back() == char(125),
              "正式文件首尾是花括号（完整）");

        // ③ 能读回来
        BridgeConfig b;
        check(loadConfig(kFile, b), "能读回");
        checkInt(b.volume, 37, "值正确");
        removeFile(kFile);
    }

    // ---- 13. ★ 截断 / 损坏的文件被拒绝，并留一份 .bak ----
    {
        std::printf("\n13. 截断 / 损坏保护\n");
        removeFile(kFile);
        removeFile(kFile + ".bak");

        // 模拟「写到一半被杀」：开头有 {，结尾没 }
        writeRaw(kFile, "{\"volume\": 37, \"latencyMs\": 18");
        BridgeConfig out;
        out.volume = 55;
        check(!loadConfig(kFile, out), "截断文件应被拒绝（返回 false）");
        checkInt(out.volume, 55, "拒绝时不动 out");

        // 留证现在写到 .corrupt（不覆盖 .bak —— 那是恢复用的好副本）。
        // 见 loadConfig 里的说明（2026-10-24 复查：恢复不能自消耗）。
        std::FILE* b = std::fopen((kFile + ".corrupt").c_str(), "rb");
        check(b != nullptr, "留了一份 .corrupt 供事后查证");
        if (b) std::fclose(b);

        // 完全不是 JSON 也一样（备份清掉 → 只能走失败路径）
        removeFile(kFile + ".bak");
        removeFile(kFile + ".corrupt");
        writeRaw(kFile, "这不是 JSON 乱码");
        BridgeConfig o2;
        o2.volume = 66;
        check(!loadConfig(kFile, o2), "非 JSON 应被拒绝");
        checkInt(o2.volume, 66, "拒绝时不动 out");

        removeFile(kFile);
        removeFile(kFile + ".bak");
        removeFile(kFile + ".corrupt");
    }

    // ---- 14. ★ loadConfig 幂等：同一对象读两次结果不变 ----
    // P2-5：ignoredDeviceIds 原来只 push_back 不清空 ——
    // 对同一对象 load 两次会把设备列表累积成两倍。
    {
        std::printf("\n14. loadConfig 幂等\n");
        BridgeConfig a;
        a.ignoredDeviceIds = {"AA:BB:CC:DD:EE:01", "AA:BB:CC:DD:EE:02"};
        a.volume = 23;
        saveConfig(kFile, a);

        BridgeConfig out;
        loadConfig(kFile, out);
        const size_t n1 = out.ignoredDeviceIds.size();
        loadConfig(kFile, out);
        const size_t n2 = out.ignoredDeviceIds.size();
        checkInt(int(n1), 2, "第一次加载 2 条");
        checkInt(int(n2), 2, "第二次加载仍是 2 条（不累积）");
        checkInt(out.volume, 23, "音量不因二次加载而变");
        removeFile(kFile);
    }

    // ---- 15. ★ 非 ASCII 路径（2026-10-18 回归的守卫）----
    //
    // 背景：配置路径来自 appDataDir()，而它用 GetEnvironmentVariableA，
    // 返回的是**系统本地编码(ACP)**字节。原子换名时若按 CP_UTF8 解码它，
    // 非法字节会变成 U+FFFD → 宽路径错误 → MoveFileExW 失败 →
    // 配置永不落盘。中文用户名下必然触发。
    //
    // 实测（同一段 ACP 路径）：CP_UTF8 → MoveFileExW 失败 err=3；CP_ACP → 成功。
    //
    // 这里用 GBK 字节（而不是源码里的中文字面量）构造文件名 ——
    // 窄 API 走的是 ACP，源码里写中文经 /utf-8 编译后是 UTF-8 字节，
    // 反而不忠实。GBK 字节才等价于 appDataDir() 的真实产物。
    //
    // "测试" 的 GBK 字节是 B2 E2 CA D4。
    {
        std::printf("\n15. 非 ASCII 路径（ACP 字节）\n");
        const std::string base = std::string("cfg_test_") +
                                 "\xB2\xE2\xCA\xD4" + ".json";
        removeFile(base);
        removeFile(base + ".tmp");

        BridgeConfig a;
        a.volume = 33;
        a.lastDeviceName = "non-ascii path";
        check(saveConfig(base, a), "非 ASCII 路径：保存成功");

        std::FILE* t = std::fopen((base + ".tmp").c_str(), "rb");
        check(t == nullptr, "非 ASCII 路径：不留 .tmp");
        if (t) std::fclose(t);

        BridgeConfig b;
        check(loadConfig(base, b), "非 ASCII 路径：能读回");
        checkInt(b.volume, 33, "非 ASCII 路径：值正确");

        removeFile(base);
        removeFile(base + ".tmp");
    }

    if (g_fail) {
        std::printf("   %d/%d 项失败 —— 配置读写有回归\n", g_fail, g_checks);
        return 1;
    }
    std::printf("   全部通过（%d 项）\n", g_checks);
    return 0;
}
