// SPDX-License-Identifier: Apache-2.0
//
// airplay_bridge -- 把 Windows 系统声音实时串流到 AirPlay 2 接收端（HomePod）。
//
// 用法:
//   airplay_bridge                    托盘模式：常驻右下角，右键选设备
//   airplay_bridge --console          命令行模式（带设备列表）
//   airplay_bridge --list             只列出设备后退出
//   airplay_bridge <ip>               直连指定 IP（命令行）
//   airplay_bridge --last             直接重连上次的设备（开机自启用）
//   airplay_bridge --install          注册开机自启
//   airplay_bridge --uninstall        取消开机自启
//
// 架构:
//   WASAPI 环回采集 -> RingBuffer -> RaopSender -> 加密 ALAC -> HomePod
//   托盘/命令行共享同一套引擎逻辑；托盘模式下主循环同时 pump 托盘消息和网络。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "discovery.h"
#include "config_store.h"
#include "wasapi_capture.h"
#include "tray.h"
#include "sysvolume.h"
#include "latency_dialog.h"

#include "raop_loop.h"
#include "raop_sender.h"
#include "raop_auth.h"
#include "ring_buffer.h"

#include <windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fxchain;

int main(int argc, char** argv);   // 前置声明（WinMain 要调它）

namespace {

std::atomic<bool> g_stop{false};

// 延迟档位：从激进到保守。不同网络/设备能承受的最小值不同，
// 用户自己试出"不爆音的最低档"就是最优解。
struct LatencyLevel { int ms; const char* label; };
const LatencyLevel kLatencyLevels[] = {
    { 60,   "极低 60ms（可能爆音）" },
    { 100,  "低 100ms" },
    { 150,  "中 150ms（推荐）" },
    { 300,  "较高 300ms（稳）" },
    { 600,  "高 600ms（最稳）" },
};
constexpr int kLatencyLevelCount = sizeof(kLatencyLevels) / sizeof(kLatencyLevels[0]);

// 音量档位（百分比）。音量可以运行时改，不用重连。
struct VolumeLevel { int pct; const char* label; };
const VolumeLevel kVolumeLevels[] = {
    {  0,   "静音" },
    { 20,  "20%" },
    { 40,  "40%" },
    { 60,  "60%" },
    { 80,  "80%" },
    { 100, "100%" },
};
constexpr int kVolumeLevelCount = sizeof(kVolumeLevels) / sizeof(kVolumeLevels[0]);
constexpr int kVolumeMenuBase = 1200;   // 音量档位菜单 id = base + index
constexpr int kLatencyMenuBase = 1100;   // 档位菜单 id = base + index

// 托盘菜单 ID
enum : int {
    MENU_STATUS    = 100,   // 状态（禁用项，只显示）
    MENU_TOGGLE    = 101,   // 连接 / 断开
    MENU_RESCAN    = 102,   // 重新扫描并选择设备
    MENU_AUTOSTART = 103,   // 开机自启开关
    MENU_LATENCY   = 105,   // 延迟档位（子菜单）
    MENU_LATENCY_CUSTOM = 106,   // 自定义延迟（无极滑块）
    MENU_VOLUME   = 107,   // 音量（子菜单）
    MENU_VOLUME_CUSTOM = 108,   // 自定义音量（滑块）
    MENU_QUIT      = 104,   // 退出
};

BOOL WINAPI onCtrl(DWORD sig) {
    if (sig == CTRL_C_EVENT || sig == CTRL_BREAK_EVENT || sig == CTRL_CLOSE_EVENT) {
        g_stop.store(true);
        return TRUE;
    }
    return FALSE;
}

std::string exePath();   // 前置声明

// 托盘模式没有控制台，把关键事件写到 exe 同目录的日志文件
void logLine(const std::string& msg) {
    static std::string path = []() {
        // 日志放用户目录，不在 exe 旁边留文件（exe 保持零外挂）
        char buf[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, MAX_PATH);
        std::string base = (n > 0 && n < MAX_PATH) ? std::string(buf, n)
                                                   : std::string(".");
        std::string dir = base + "\\AirPlayBridge\\";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "bridge.log";
    }();
    // 简单轮转：超过 512KB 就丢掉旧内容重开。
    // 否则长时间挂机（每 2 秒一条心跳）会把日志撑到几十 MB。
    {
        FILE* chk = std::fopen(path.c_str(), "rb");
        if (chk) {
            std::fseek(chk, 0, SEEK_END);
            long sz = std::ftell(chk);
            std::fclose(chk);
            if (sz > 512 * 1024) std::remove(path.c_str());
        }
    }
    FILE* f = std::fopen(path.c_str(), "a");
    if (!f) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(f, "%02d:%02d:%02d  %s\n", st.wHour, st.wMinute, st.wSecond, msg.c_str());
    std::fclose(f);
}

std::string exePath() {
    char buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::string(buf, n);
}

bool setAutoStart(bool enable) {
    HKEY key = nullptr;
    const char* runKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, runKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;

    const char* valueName = "AirPlayBridge";
    LONG rc;
    if (enable) {
        std::string cmd = "\"" + exePath() + "\" --last";
        rc = RegSetValueExA(key, valueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>(cmd.size() + 1));
    } else {
        rc = RegDeleteValueA(key, valueName);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

bool isAutoStartEnabled() {
    HKEY key = nullptr;
    const char* runKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, runKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    char buf[1024] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    LONG rc = RegQueryValueExA(key, "AirPlayBridge", nullptr, &type,
                               reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

// ---- 设备过滤与显示 ----
std::vector<AirPlayDevice> filterDevices(const std::vector<AirPlayDevice>& in,
                                         const BridgeConfig& cfg) {
    std::vector<AirPlayDevice> out;
    for (auto& d : in) {
        if (cfg.hideAppleTV && d.isAppleTV) continue;
        bool ignored = false;
        for (auto& id : cfg.ignoredDeviceIds)
            if (!id.empty() && id == d.deviceId) { ignored = true; break; }
        if (ignored) continue;
        out.push_back(d);
    }
    return out;
}

std::string deviceLabel(const AirPlayDevice& d) {
    std::string kind = d.isHomePod ? "HomePod" : (d.isAppleTV ? "Apple TV" : "AirPlay");
    std::string s = d.name;
    if (!s.empty()) s += " ";
    s += "[" + kind;
    if (!d.model.empty()) s += " " + d.model;
    s += "]";
    if (d.isAirPlay2) s += " AP2";
    return s;
}

// ---- 命令行模式的选择 ----
const AirPlayDevice* pickDeviceConsole(const std::vector<AirPlayDevice>& devs) {
    if (devs.empty()) return nullptr;
    if (devs.size() == 1) return &devs[0];

    std::printf("\n发现设备:\n");
    for (size_t i = 0; i < devs.size(); ++i)
        std::printf("  [%zu] %s  %s\n", i + 1, deviceLabel(devs[i]).c_str(),
                    devs[i].host.c_str());
    std::printf("\n选择 [1-%zu, 回车=1]: ", devs.size());
    std::fflush(stdout);

    char line[64] = {};
    if (!std::fgets(line, sizeof(line), stdin)) return &devs[0];
    int n = std::atoi(line);
    if (n < 1 || static_cast<size_t>(n) > devs.size()) n = 1;
    return &devs[static_cast<size_t>(n) - 1];
}

// ============================================================
// 流会话：把采集 + 推流封装起来，托盘和命令行都用它
// ============================================================
struct Session {
    BridgeConfig* cfg = nullptr;
    std::string   host;
    std::string   deviceId;
    std::string   deviceName;
    int           volume = 45;

    std::unique_ptr<RingBuffer<int16_t>> ring;
    std::unique_ptr<RaopLoop>            loop;
    std::unique_ptr<RaopSender>          sender;
    std::unique_ptr<WasapiCapture>       capture;
    std::thread                          producer;

    std::atomic<bool> connected{false};
    // 串流期间把默认播放设备切到虚拟声卡，这样电脑自己不出声，
    // 声音只从 HomePod 出。断开时切回原来的设备。
    // 空字符串表示还没保存过。
    std::string savedRenderDevice;
    // 用于把"原设备"落盘（崩溃保护）。由外部在启动时注入。
    std::string cfgPath;
    std::atomic<bool> wantReconnect{false};
    std::atomic<bool> running{false};

    // 事件回调（由外部设置）
    std::function<void(bool)>  onConnected;      // true = 已连接
    std::function<void(const std::string&)> onStatus;

    bool start(std::string& err) {
        // 先清干净：重复调用或上次失败残留时，producer 可能是个
        // joinable 的空线程，留着会在成员析构时 std::terminate。
        if (producer.joinable()) producer.join();

        // 音频采集。默认采集"所有活动的输出设备"并混音 ——
        // 这样即使某个应用（比如浏览器）被单独指到了别的声卡，
        // 它的声音也能抓到，用户不用去音量合成器里改输出。
        capture = std::make_unique<WasapiCapture>(WasapiCaptureConfig{});
        if (!capture->open(&err)) {
            capture.reset();   // 失败路径不留半成品
            return false;
        }

        {
            auto names = capture->deviceNames();
            std::string joined;
            for (size_t i = 0; i < names.size(); ++i) {
                if (i) joined += ", ";
                joined += names[i];
            }
            logLine("采集 " + std::to_string(names.size()) + " 台输出设备: " + joined);
        }

        uint32_t inputRate = capture->deviceSampleRate();

        // ring 深度 = 元素个数（int16_t）；引擎按立体声交错消费，
        // 帧数 = 元素数 / 2。1<<15 = 32768 元素 = 16384 帧 ≈ 371ms。
        // 注意：本地 ring 深度与"延迟"不是一回事——延迟是告诉接收端
        // 往后排多久再播，本地缓冲够喂就行，不必跟着延迟一起放大。
        ring = std::make_unique<RingBuffer<int16_t>>(1u << 15);
        loop = std::make_unique<RaopLoop>();

        RaopEvents ev;
        ev.launched = [this](bool ok, const std::string& e) {
            // 连上了：记住当前默认设备，然后切到虚拟声卡。
            // 这样系统声音不再走向物理音箱，而是进了虚拟声卡；
            // 我们从虚拟声卡采集推给 HomePod —— 电脑自己静悄悄。
            if (ok && savedRenderDevice.empty()) {
                const std::string cur = getDefaultRenderDeviceName();
                if (!cur.empty()) {
                    // 只在自己不是虚拟声卡时切换，避免来回切死锁
                    if (cur.find("CABLE") == std::string::npos) {
                        if (setDefaultRenderDevice("CABLE Input")) {
                            savedRenderDevice = cur;
                            // 落盘记录，这样即使我们崩了，下次启动也能恢复
                            if (!cfgPath.empty()) {
                                BridgeConfig tmp;
                                loadConfig(cfgPath, tmp);
                                tmp.preCaptureDevice = cur;
                                saveConfig(cfgPath, tmp);
                            }
                        }
                    }
                }
            }
            connected.store(ok);
            if (onConnected) onConnected(ok);
            if (onStatus) onStatus(ok ? "已连接" : ("连接失败: " + e));
            if (!ok) wantReconnect.store(true);
        };
        ev.closed = [this]() {
            connected.store(false);
            if (onConnected) onConnected(false);
            if (onStatus) onStatus("连接断开");
            wantReconnect.store(true);
        };
        ev.pinRequired = [](const std::string&) {};
        ev.credentialsObtained = [](const std::string&, const std::string&) {};

        RaopLogSink log = [](RaopLogLevel lvl, const std::string& m) {
            std::string tag = (lvl == RaopLogLevel::Warn) ? "[W] " : "[i] ";
            logLine(tag + m);
        };

        sender = std::make_unique<RaopSender>(*loop, std::move(ev), std::move(log));
        sender->attachRing(ring.get());
        sender->setInputFormat(inputRate);
        sender->setIdentity({"AirPlay Bridge", "AA:BB:CC:DD:EE:FF", "MacBookPro18,3"});
        sender->setAuth(RaopDeviceInfo::Auth::HapTransient, true, "", "", "");
        sender->setVolume(volume);
        if (cfg) sender->setLatencyMs(cfg->latencyMs);   // 延迟必须 start() 前设

        producer = std::thread([this]() {
            capture->run([this](const int16_t* d, size_t n) {
                ring->tryPush(std::span<const int16_t>(d, n * 2));
            });
        });

        sender->start(host, 7000, "AirPlay Bridge");
        running.store(true);
        return true;
    }

    // 主循环的一步；返回 false 表示需要重连
    void pump() {
        if (!sender) return;
        loop->pump(*sender);
    }

    void stop() {
        running.store(false);
        // 切回原来的默认设备
        if (!savedRenderDevice.empty()) {
            setDefaultRenderDevice(savedRenderDevice);
            savedRenderDevice.clear();
            // 正常恢复了，清掉落盘标记
            if (!cfgPath.empty()) {
                BridgeConfig tmp;
                loadConfig(cfgPath, tmp);
                tmp.preCaptureDevice.clear();
                saveConfig(cfgPath, tmp);
            }
        }
        if (capture) capture->stop();
        if (sender)  sender->stop();
        if (producer.joinable()) producer.join();
        connected.store(false);
    }

    void teardown() {
        stop();
        // 主动拆除时必须清掉重连标志：否则用户手动断开/重连后，
        // 下一个检查点会把这个健康的会话当成"待重连"再拆一次，
        // 表现为约 5 秒闪断一次。
        wantReconnect.store(false);
        producer = std::thread();   // 清掉已 join 的线程句柄
        sender.reset();
        loop.reset();
        ring.reset();
        capture.reset();
    }
};

// 扫描 + 选中一个设备（托盘模式：自动选 HomePod；多个则用弹出菜单）
bool resolveDevice(Session& sess, const BridgeConfig& cfg,
                   std::vector<AirPlayDevice>& discovered) {
    discovered = filterDevices(discoverAirPlayDevices(2500), cfg);
    if (discovered.empty()) return false;

    // 优先上次的设备
    for (auto& d : discovered) {
        if (!cfg.lastDeviceId.empty() && d.deviceId == cfg.lastDeviceId) {
            sess.host = d.host;
            sess.deviceId = d.deviceId;
            sess.deviceName = d.name;
            return true;
        }
    }
    // 只有一个就直接用
    if (discovered.size() == 1) {
        sess.host = discovered[0].host;
        sess.deviceId = discovered[0].deviceId;
        sess.deviceName = discovered[0].name;
        return true;
    }
    // 多个且没有历史：选第一个 HomePod
    for (auto& d : discovered) {
        if (d.isHomePod) {
            sess.host = d.host;
            sess.deviceId = d.deviceId;
            sess.deviceName = d.name;
            return true;
        }
    }
    sess.host = discovered[0].host;
    sess.deviceId = discovered[0].deviceId;
    sess.deviceName = discovered[0].name;
    return true;
}

} // namespace

// WIN32 子系统需要 WinMain 作为入口（而不是 main）
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}

int main(int argc, char** argv) {
    // WIN32 子系统默认没有控制台；命令行模式下把父进程的控制台接过来
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }

    // 判断模式与解析参数
    bool wantList = false, wantInstall = false, wantUninstall = false;
    bool wantConsole = false, wantLast = false, wantTray = true;
    std::string hostArg, wavArg;
    int volume = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list")           wantList = true;
        else if (a == "--install")   wantInstall = true;
        else if (a == "--uninstall") wantUninstall = true;
        else if (a == "--console")   { wantConsole = true; wantTray = false; }
        else if (a == "--last")      wantLast = true;
        else if (a == "--wav" && i + 1 < argc) wavArg = argv[++i];
        else if (a == "--volume" && i + 1 < argc) volume = std::atoi(argv[++i]);
        else if (!a.empty() && a[0] != '-') { hostArg = a; wantTray = false; }
    }
    if (wantList || wantInstall || wantUninstall) wantTray = false;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 运行时数据在 %LOCALAPPDATA%/AirPlayBridge/
    std::string cfgPath = defaultConfigPath();
    BridgeConfig cfg;
    loadConfig(cfgPath, cfg);
    if (volume < 0) volume = cfg.volume;

    // ---- 一次性命令 ----
    if (wantInstall) {
        bool ok = setAutoStart(true);
        std::printf(ok ? "已注册开机自启。\n" : "注册失败。\n");
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (wantUninstall) {
        bool ok = setAutoStart(false);
        std::printf(ok ? "已取消开机自启。\n" : "取消失败。\n");
        CoUninitialize();
        return ok ? 0 : 1;
    }
    if (wantList) {
        std::printf("扫描中...\n");
        auto all = discoverAirPlayDevices(3000);
        auto devs = filterDevices(all, cfg);
        if (devs.empty()) std::printf("没有发现设备。\n");
        for (auto& d : devs)
            std::printf("  %s  %s\n", deviceLabel(d).c_str(), d.host.c_str());
        CoUninitialize();
        return 0;
    }

    // ---- 命令行模式 ----
    if (wantConsole || !hostArg.empty()) {
        SetConsoleCtrlHandler(onCtrl, TRUE);

        std::vector<AirPlayDevice> devs;
        std::string host = hostArg;

        if (host.empty()) {
            std::printf("扫描局域网 AirPlay 设备...\n");
            std::fflush(stdout);
            devs = filterDevices(discoverAirPlayDevices(2500), cfg);
            const AirPlayDevice* d = pickDeviceConsole(devs);
            if (!d) {
                std::printf("没有可用设备。\n");
                CoUninitialize();
                return 1;
            }
            host = d->host;
            cfg.lastHost = d->host;
            if (!d->deviceId.empty()) cfg.lastDeviceId = d->deviceId;
            if (!d->name.empty())     cfg.lastDeviceName = d->name;
            saveConfig(cfgPath, cfg);
            std::printf("目标: %s (%s)\n", d->name.c_str(), host.c_str());
        }

        Session sess;
        sess.cfg = &cfg;
        sess.host = host;
        sess.volume = volume;
        sess.onStatus = [](const std::string& s) {
            std::printf(">> %s\n", s.c_str());
            std::fflush(stdout);
        };

        std::string err;
        if (!sess.start(err)) {
            std::printf("启动失败: %s\n", err.c_str());
            CoUninitialize();
            return 1;
        }

        std::printf(">> 串流中，Ctrl+C 停止\n");
        std::fflush(stdout);
        while (!g_stop.load()) sess.pump();
        sess.teardown();

        CoUninitialize();
        return 0;
    }

    // ---- 托盘模式 ----
    TrayIcon tray;
    if (!tray.create("AirPlay Bridge - 未连接")) {
        std::printf("托盘图标创建失败。\n");
        CoUninitialize();
        return 1;
    }

    Session sess;
    sess.cfg = &cfg;
    sess.volume = volume;
    sess.cfgPath = cfgPath;   // 崩溃保护要用
    sess.onStatus = [&tray](const std::string& s) {
        tray.setTooltip("AirPlay Bridge - " + s);
        logLine(s);
    };
    sess.onConnected = [&tray](bool ok) { tray.setConnected(ok); };

    std::vector<AirPlayDevice> discovered;

    // 尝试连接（托盘模式：优先历史设备；没有历史则自动选 HomePod）
    auto tryConnect = [&]() {
        if (sess.running.load()) return;
        if (resolveDevice(sess, cfg, discovered)) {
            std::string err;
            if (!sess.start(err)) {
                tray.setTooltip("AirPlay Bridge - 启动失败");
                tray.notify("AirPlay Bridge", "启动失败: " + err);
            } else {
                cfg.lastHost = sess.host;
                if (!sess.deviceId.empty()) cfg.lastDeviceId = sess.deviceId;
                if (!sess.deviceName.empty()) cfg.lastDeviceName = sess.deviceName;
                saveConfig(cfgPath, cfg);
            }
        } else {
            tray.setTooltip("AirPlay Bridge - 未找到设备");
        }
    };

    // ---- 崩溃保护：自检上次是否异常退出 ----
    // 如果配置里记着"接管前的设备"，说明上次没走正常退出流程。
    // 而当前默认设备还是虚拟声卡的话，就把它切回去 —— 否则用户
    // 会以为电脑坏了（完全没声音，且不知道发生了什么）。
    if (!cfg.preCaptureDevice.empty()) {
        const std::string nowDev = getDefaultRenderDeviceName();
        if (nowDev.find("CABLE") != std::string::npos) {
            logLine("检测到上次异常退出，恢复默认音频设备: " + cfg.preCaptureDevice);
            setDefaultRenderDevice(cfg.preCaptureDevice);
        }
        cfg.preCaptureDevice.clear();
        saveConfig(cfgPath, cfg);
    }

    // 首次扫描
    if (wantLast || !cfg.lastHost.empty()) {
        tryConnect();
    }

    // 主循环：同时 pump 托盘消息和网络
    auto lastCheck = std::chrono::steady_clock::now();
    // 重连退避：连续失败时逐次翻倍，避免每 5 秒猛扫一次
    int retryBackoffSec = 5;
    // 后台扫描（避免阻塞托盘消息循环）
    std::future<std::vector<AirPlayDevice>> scanFuture;
    std::vector<AirPlayDevice> scanResult;

    while (!g_stop.load()) {
        tray.pump([&](TrayEvent e) {
            if (e != TrayEvent::RightClick) return;

            std::vector<TrayMenuItem> menu;
            // 状态区：拆成两行，避免设备名把菜单撑得过宽。
            // 第一行只有状态本身，第二行是设备名 + IP。
            {
                std::string line1, line2, line3;
                if (sess.connected.load()) {
                    line1 = "已连接";
                    // 设备名单独一行；过长则截断，避免把菜单撑宽
                    std::string who = sess.deviceName.empty() ? sess.host : sess.deviceName;
                    const size_t kMaxName = 22;
                    if (who.size() > kMaxName) who = who.substr(0, kMaxName) + "...";
                    line2 = who;
                    // IP 再单独一行（短，不会撑宽菜单）
                    line3 = sess.host;
                } else if (sess.running.load()) {
                    line1 = "连接中...";
                } else {
                    line1 = "未连接";
                }
                // 状态行左侧用彩色圆点（绿=已连接，灰=未连接）。
                // 原生菜单不能给文字上色，圆点位图是唯一能做彩色的方式。
                // 1 = 绿点（已连接），2 = 空心点（未连接）。
                // 位图区是菜单自带的（系统在那儿画对号），SetMenuItemBitmaps
                // 可以把对号换成我们自己的图形。
                int dot = sess.connected.load() ? 1 : 2;
                // enabled 必须是 true：菜单对 enabled=false 的项一律画成灰色，
                // 那样文字和位图都会被系统灰色化，怎么调都出不来颜色。
                // 这里设成可点，但 MENU_STATUS 在回调里不做任何事。
                // 已连接时打勾（checked=true）：系统会用蓝底画那个勾，
                // 跟"开机自启"选中时一模一样 —— 这是原生菜单唯一
                // 能做出彩色高亮的地方。未连接则不打勾。
                // 只用系统自己的勾（蓝底那个），不要再挂自定义位图 ——
                // 位图会把勾的位置占掉，反而看不到勾。
                // 设备名和 IP 设成不可点（enabled=false），视觉上清楚它们是说明文字。
                bool markConnected = sess.connected.load();
                menu.push_back({line1, MENU_STATUS, true, false, markConnected, 0});
                if (!line2.empty()) menu.push_back({line2, MENU_STATUS, false, false, false});
                if (!line3.empty()) menu.push_back({line3, MENU_STATUS, false, false, false});
            }
            menu.push_back({"", 0, true, true, false});   // 分隔线
            menu.push_back({sess.running.load() ? "断开" : "连接", MENU_TOGGLE});
            menu.push_back({"重新扫描设备", MENU_RESCAN});
            // 延迟合并成一个子菜单，当前值显示在右侧
            {
                std::string latLabel = "延迟  " + std::to_string(cfg.latencyMs) + "ms";
                menu.push_back({latLabel, MENU_LATENCY});
            }
            {
                std::string volLabel = "音量  " + std::to_string(cfg.volume) + "%";
                menu.push_back({volLabel, MENU_VOLUME});
            }
            menu.push_back({"", 0, true, true, false});
            menu.push_back({"开机自启", MENU_AUTOSTART, true, false, isAutoStartEnabled()});
            menu.push_back({"", 0, true, true, false});
            menu.push_back({"退出", MENU_QUIT});

            tray.showMenu(menu, [&](int id) {
                switch (id) {
                    case MENU_TOGGLE:
                        if (sess.running.load()) {
                            sess.teardown();
                            tray.setConnected(false);
                            tray.setTooltip("AirPlay Bridge - 已断开");
                        } else {
                            tryConnect();
                        }
                        break;
                    case MENU_RESCAN:
                        sess.teardown();
                        cfg.lastDeviceId.clear();   // 强制重新选择
                        tray.setConnected(false);
                        tray.setTooltip("AirPlay Bridge - 扫描中");
                        discovered = filterDevices(discoverAirPlayDevices(2500), cfg);
                        if (discovered.size() > 1) {
                            std::vector<TrayMenuItem> sub;
                            for (size_t i = 0; i < discovered.size(); ++i)
                                sub.push_back({deviceLabel(discovered[i]),
                                               static_cast<int>(1000 + i)});
                            tray.showMenu(sub, [&](int sid) {
                                size_t idx = static_cast<size_t>(sid - 1000);
                                if (idx >= discovered.size()) return;
                                sess.host = discovered[idx].host;
                                sess.deviceId = discovered[idx].deviceId;
                                sess.deviceName = discovered[idx].name;
                                std::string err;
                                if (!sess.start(err)) {
                                    tray.notify("AirPlay Bridge", "启动失败: " + err);
                                } else {
                                    cfg.lastHost = sess.host;
                                    if (!sess.deviceId.empty()) cfg.lastDeviceId = sess.deviceId;
                                    if (!sess.deviceName.empty()) cfg.lastDeviceName = sess.deviceName;
                                    saveConfig(cfgPath, cfg);
                                }
                            });
                        } else if (discovered.size() == 1) {
                            sess.host = discovered[0].host;
                            sess.deviceId = discovered[0].deviceId;
                            sess.deviceName = discovered[0].name;
                            std::string err;
                            sess.start(err);
                        }
                        break;
                    case MENU_LATENCY: {
                        // 一个窗口里放：固定档（当前档打勾）+ 自定义滑块
                        std::vector<TrayMenuItem> sub;
                        for (int i = 0; i < kLatencyLevelCount; ++i) {
                            sub.push_back({kLatencyLevels[i].label,
                                           kLatencyMenuBase + i,
                                           true, false,
                                           cfg.latencyMs == kLatencyLevels[i].ms});
                        }
                        sub.push_back({"", 0, true, true, false});   // 分隔线
                        sub.push_back({"自定义（滑块）...",
                                       MENU_LATENCY_CUSTOM, true, false, false});
                        tray.showMenu(sub, [&](int sid) {
                            if (sid == MENU_LATENCY_CUSTOM) {
                                int picked = showLatencyDialog(cfg.latencyMs);
                                if (picked > 0) {
                                    cfg.latencyMs = picked;
                                    saveConfig(cfgPath, cfg);
                                    logLine("延迟改为 " + std::to_string(picked) + "ms，重连生效");
                                    if (sess.running.load()) {
                                        sess.teardown();
                                        tryConnect();
                                    }
                                }
                                return;
                            }
                            int idx = sid - kLatencyMenuBase;
                            if (idx < 0 || idx >= kLatencyLevelCount) return;
                            cfg.latencyMs = kLatencyLevels[idx].ms;
                            saveConfig(cfgPath, cfg);
                            logLine("延迟切换为 " + std::to_string(cfg.latencyMs) + "ms，重连生效");
                            // 延迟要在 start() 前设，所以必须重连一次
                            if (sess.running.load()) {
                                sess.teardown();
                                tryConnect();
                            }
                        });
                        break;
                    }
                    case MENU_VOLUME: {
                        std::vector<TrayMenuItem> vsub;
                        for (int i = 0; i < kVolumeLevelCount; ++i) {
                            vsub.push_back({kVolumeLevels[i].label,
                                            kVolumeMenuBase + i,
                                            true, false,
                                            cfg.volume == kVolumeLevels[i].pct});
                        }
                        vsub.push_back({"", 0, true, true, false});
                        vsub.push_back({"自定义（滑块）...", MENU_VOLUME_CUSTOM, true, false, false});
                        tray.showMenu(vsub, [&](int sid) {
                            int newVol = -1;
                            if (sid == MENU_VOLUME_CUSTOM) {
                                newVol = showVolumeDialog(cfg.volume);
                            } else {
                                int idx = sid - kVolumeMenuBase;
                                if (idx >= 0 && idx < kVolumeLevelCount)
                                    newVol = kVolumeLevels[idx].pct;
                            }
                            if (newVol < 0) return;
                            cfg.volume = newVol;
                            saveConfig(cfgPath, cfg);
                            // 音量是运行时参数，改了立刻生效，不用重连
                            if (sess.sender) sess.sender->setVolume(newVol);
                            logLine("音量改为 " + std::to_string(newVol) + "%");
                        });
                        break;
                    }
                    case MENU_AUTOSTART:
                        setAutoStart(!isAutoStartEnabled());
                        break;
                    case MENU_QUIT:
                        g_stop.store(true);
                        break;
                    default:
                        break;
                }
            });
        });

        if (sess.running.load()) sess.pump();

        // 断线重连的检查。
        // 注意：设备扫描会阻塞约 0.7 秒（端口扫描），绝不能放在主线程——
        // 那样每 5 秒托盘菜单就卡一下。所以放到后台 future 里跑。
        auto now = std::chrono::steady_clock::now();
        bool scanDone = false;
        if (scanFuture.valid() &&
            scanFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            scanResult = scanFuture.get();
            scanDone = true;
        }

        if (scanDone) {
            // 扫描回来了：如果连上了就重置退避，否则加大退避
            if (sess.running.load()) {
                retryBackoffSec = 5;
            }
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCheck).count() >= retryBackoffSec) {
            lastCheck = now;
            if (sess.running.load() && sess.wantReconnect.load()) {
                sess.wantReconnect.store(false);
                sess.teardown();
                tryConnect();
            } else if (!sess.running.load() && cfg.autoReconnect) {
                // 启动时没连上（设备还没通电）。
                // 扫描放到后台，避免冻结 UI；连续失败则退避。
                if (!scanFuture.valid()) {
                    scanFuture = std::async(std::launch::async, [&cfg]() {
                        std::vector<AirPlayDevice> out;
                        out = filterDevices(discoverAirPlayDevices(2500), cfg);
                        return out;
                    });
                }
                tryConnect();
                // 退避翻倍，上限 60 秒
                retryBackoffSec = (retryBackoffSec < 60) ? retryBackoffSec * 2 : 60;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    sess.teardown();
    tray.destroy();
    CoUninitialize();
    return 0;
}
