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
#include "resampler.h"
#include "tray.h"
#include "darkmode.h"
#include "sysvolume.h"
#include "audiodevices.h"
#include "latency_dialog.h"

#include "raop_loop.h"
#include "raop_sender.h"
#include "raop_auth.h"
#include "ring_buffer.h"

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
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

// 延迟档位。
//
// 实测能跑通的区间很窄：100ms 上下就够，600ms 那种档位纯属占地方。
// 所以档位收在 120-150ms 这一段 —— 需要更大值的话用下面的"自定义"滑块。
// 用户自己试出"不爆音的最低档"就是最优解。
struct LatencyLevel { int ms; const char* label; };
const LatencyLevel kLatencyLevels[] = {
    { 120,  "120ms" },
    { 130,  "130ms" },
    { 140,  "140ms" },
    { 150,  "150ms（推荐）" },
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

// 音质是 0-100 连续可调的（底层是 sinc 抽头数，4..32）。
// 这几个是快捷预设 —— 实测曲线上的几个拐点，不是为了凑数：
//   5% 约 20dB / 20% 约 30dB / 45% 约 38dB / 70% 到顶 39.5dB
struct QualityLevel { int value; const char* label; };
const QualityLevel kQualityLevels[] = {
    {  0, "快速" },
    { 20, "标准" },
    { 45, "高" },
    { 70, "极高" },
    { 90, "至臻" },
};
constexpr int kQualityLevelCount = sizeof(kQualityLevels) / sizeof(kQualityLevels[0]);
constexpr int kQualityMenuBase = 1300;   // 音质档位菜单 id = base + index
constexpr int kDeviceMenuBase  = 1400;   // 设备列表菜单 id = base + index
constexpr int kLatencyMenuBase = 1100;   // 档位菜单 id = base + index
constexpr int kStandbyMenuBase = 1500;   // 静音待机档位菜单 id = base + index

// 静音自动待机的档位（秒）。0 是特例：不等秒数，只要出现一个静音缓冲
// 就待机（见 Session::silentNow）—— 适合"放了就灭屏"的场景。
struct StandbyLevel { int secs; const char* label; };
const StandbyLevel kStandbyLevels[] = {
    {  0, "0 秒" },
    {  1, "1 秒（推荐）" },
    {  3, "3 秒" },
    {  5, "5 秒" },
    {  8, "8 秒" },
    { 10, "10 秒" },
};
constexpr int kStandbyLevelCount = sizeof(kStandbyLevels) / sizeof(kStandbyLevels[0]);

// 托盘菜单 ID
enum : int {
    MENU_STATUS    = 100,   // 状态（禁用项，只显示）
    MENU_TOGGLE    = 101,   // 连接 / 断开
    MENU_RESCAN    = 102,   // 重新扫描并选择设备
    MENU_DEVICES   = 112,   // 选择设备（子菜单）
    MENU_RESCAN_SUB = 113,  // 子菜单里的"重新扫描"
    MENU_AUTOSTART = 103,   // 开机自启开关
    MENU_LATENCY   = 105,   // 延迟档位（子菜单）
    MENU_LATENCY_CUSTOM = 106,   // 自定义延迟（无极滑块）
    MENU_VOLUME   = 107,   // 音量（子菜单）
    MENU_VOLUME_CUSTOM = 108,   // 自定义音量（滑块）
    MENU_QUALITY   = 110,   // 音质（子菜单）
    MENU_QUALITY_CUSTOM = 111,   // 自定义音质（滑块）
    MENU_OPEN_LOG  = 109,   // 打开日志文件夹
    MENU_STANDBY   = 114,   // 静音自动待机（子菜单）
    MENU_QUIT      = 104,   // 退出
};

// 静音判定的峰值阈值（int16 满量程 32767）。
// 300 约等于 -40dBFS：底噪、鼠标点击声这种级别的残留不算"有声音"，
// 定得太低会让待机永远进不去（环回采集总有微小非零样本）。
constexpr int kSilencePeak = 300;

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
    // 日志清理，两条规则：
    //   1) 超过 30 天没动过 -> 删掉重开（老日志没参考价值了）
    //   2) 超过 512KB -> 也删掉重开（长时间挂机每 2 秒一条心跳，
    //      不限制会涨到几十 MB）
    {
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attr)) {
            const ULONGLONG size = (ULONGLONG(attr.nFileSizeHigh) << 32) |
                                   attr.nFileSizeLow;
            // FILETIME -> 100ns 单位。30 天 = 30*24*3600*10^7
            constexpr ULONGLONG k30Days = 30ULL * 24 * 3600 * 10000000ULL;
            ULONGLONG lastWrite = (ULONGLONG(attr.ftLastWriteTime.dwHighDateTime) << 32) |
                                  attr.ftLastWriteTime.dwLowDateTime;
            FILETIME nowFt{};
            GetSystemTimeAsFileTime(&nowFt);
            const ULONGLONG now = (ULONGLONG(nowFt.dwHighDateTime) << 32) |
                                  nowFt.dwLowDateTime;
            const bool tooOld = (now > lastWrite) && (now - lastWrite > k30Days);
            const bool tooBig = size > 512 * 1024;
            if (tooOld || tooBig) std::remove(path.c_str());
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

// 菜单里显示的设备名。
//
// 只要名字就够了。原来还会拼上 "[HomePod AudioAccessory5,1] AP2"
// 这种尾巴，既重复又难懂：
//   名字里本来就带 HomePod，模型号是内部标识，AP2 是协议版本。
//   这些属于诊断信息，该出现在日志和 --list 里，不该塞进菜单。
std::string deviceLabel(const AirPlayDevice& d) {
    if (!d.name.empty()) return d.name;
    return d.host;   // 拿不到名字就退回 IP，至少能区分
}

// 菜单里显示的设备项：名字 + 局域网 IP。
//
// 带上 IP 是为了能区分同名设备 —— 家里有两台同型号 HomePod 时，
// 光看名字分不出哪个是哪个。
std::string deviceMenuLabel(const AirPlayDevice& d) {
    const std::string name = deviceLabel(d);
    if (d.host.empty()) return name;
    return name + "  [" + d.host + "]";
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
// 本机在接收端眼里的设备标识（形如 02:AB:CD:EF:01:23）。
//
// 必须每台机器唯一。此前写死成一个固定 MAC，于是装了本程序的每台电脑
// 在接收端看来都是同一台设备 —— 实测两台同时连会互相顶掉。
//
// 首字节取 02（本地管理地址），不会和真实网卡冲突；随机部分也不暴露本机 MAC。
static std::string g_deviceId;

static std::string makeDeviceId() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> byte(0, 255);
    const char* fmt = "02:%02X:%02X:%02X:%02X:%02X";
    char buf[18];
    std::snprintf(buf, sizeof(buf), fmt,
                  byte(gen), byte(gen), byte(gen), byte(gen), byte(gen));
    return buf;
}

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
    // 采集侧重采样：把设备采样率转到 44100，
    // 让引擎走 pass-through（绕开它那个线性插值）
    Resampler resampler;
    std::vector<int16_t> resampleBuf;
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

    // ---- 静音自动待机 ----
    // 静音满配置的秒数就把整个会话拆掉（接收端屏幕随之熄灭）。
    // 拆掉后不能什么都不做 —— 那样就再也听不到声音、永远不会醒。
    // 所以另开一个轻量采集 standbyEar 只测峰值，听到声音就唤醒。
    // 待机期间不做任何周期性重连试探：那会让屏幕反复亮起，正是要避免的。
    std::atomic<bool> standbyActive{false};   // 已进入待机
    std::atomic<bool> standbyWake{false};     // 待机中听到声音了
    // 最近一次"有声音"的 steady_clock 毫秒时间戳。
    // 初值 0 是"从没响过"，主循环会立刻判成静音 —— 所以连接成功时
    // 必须重置为当前时间（见 ev.launched）。
    std::atomic<long long> lastLoudMs{0};
    // 最近一次采集回调看到的音量是不是"静音缓冲"。
    // 只给 0 秒档位用：那一档的语义是"不等秒数，出现一个静音缓冲就待机"，
    // 光靠 lastLoudMs 算差值做不到 —— now - lastLoudMs 恒 >= 0，
    // 直接把 0 代进秒数判据会刚连上就立刻待机。
    // 初值必须是 false：刚连上还没收到任何回调时，不能算作"已经静音"。
    std::atomic<bool> silentNow{false};
    // 待机期间的轻量采集：只用来唤醒，不推数据。
    std::unique_ptr<WasapiCapture> standbyEar;
    std::thread                    standbyThread;

    // 事件回调（由外部设置）
    std::function<void(bool)>  onConnected;      // true = 已连接
    // 首次连上时调一次（之后置空）—— 用来弹一次性的欢迎提示
    std::function<void(const std::string&)> onFirstConnect;
    std::function<void(const std::string&)> onStatus;

    // skipResolve = true 时由调用方跳过设备扫描（resolveDevice 约 2.5 秒），
    // 复用已知的 host / deviceId / deviceName 直接建会话。
    // 唤醒走的就是这条路：等扫描完开头几秒的声音已经丢了。
    // 清掉待机状态：标志位 + 放掉那路「听声音」的采集。
    //
    // 只要开始建会话就该调它 —— 不管是静音唤醒回来的，还是用户手动点的连接。
    // 留着一个 true 的 standbyActive 会把「异常断线自动重连」永久挡死：
    // 主循环那条重连分支的判据里就带着 !standbyActive。
    void clearStandbyState() {
        standbyActive.store(false);
        standbyWake.store(false);
        if (standbyEar) standbyEar->stop();
        if (standbyThread.joinable()) standbyThread.join();
        standbyThread = std::thread();
        standbyEar.reset();
    }

    bool start(std::string& err, bool skipResolve = false) {
        (void)skipResolve;   // 扫描发生在调用方（tryConnect），这里只记录意图
        // 开始建会话就意味着离开待机 —— 不清的话 standbyActive 会一直挂着，
        // 之后真掉线时自动重连那条路就是死的。
        clearStandbyState();
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
                        } else {
                            // 切不过去（没装虚拟声卡、或名字不叫 CABLE Input）。
                            // 这时是"监听复制"模式：电脑音箱和 HomePod 会同时出声。
                            // 必须让用户知道 —— 否则他以为在接管模式，会奇怪为什么
                            // 电脑自己也在响。
                            logLine("未找到虚拟声卡，退回监听复制模式（电脑与 HomePod 将同时出声）");
                            if (onStatus) onStatus("已连接（无虚拟声卡，两边同时出声）");
                        }
                    }
                }
            }
            connected.store(ok);
            // 刚连上：把"最近有声"的时间戳重置成现在。
            // 不重置的话 lastLoudMs 还是 0，主循环一看"距今一亿秒没声音"
            // 就立刻把刚建立的会话拆了 —— 表现为连上即待机。
            if (ok) {
                lastLoudMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
                // silentNow 也要一起清掉：0 秒档位直接看它。
                // 不清的话，上一轮会话最后一个静音缓冲留下的 true 会让
                // 刚连上的这一秒就待机 —— 正是要避免的"连上即待机"。
                silentNow.store(false);
            }
            if (onConnected) onConnected(ok);
            if (onStatus) onStatus(ok ? "已连接" : ("连接失败: " + e));
            // 首次连上时弹一次气泡。
            // 只在首次 —— 自动重连时不弹，否则隔一会儿蹦一个很烦。
            if (ok && onFirstConnect) {
                onFirstConnect(deviceName);
                onFirstConnect = nullptr;
            }
            if (!ok) {
                // 连接失败：running 必须清掉。
                // start() 是在 sender->start() 之后才置 running=true 的，
                // 而这个回调是 sender->start() 内部同步触发的 ——
                // 如果这里不显式清掉，主循环会把一个失败的会话当成"在跑"，
                // 并且把重连退避重置回 5 秒，形成密集重试。
                running.store(false);
                wantReconnect.store(true);
            }
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

        // 采集侧重采样：把设备采样率转到 44100。
        // 这样引擎走 pass-through，不用它自带的线性插值 ——
        // 那个在高频段失真很重（实测 15kHz 信噪比只有 14dB）。
        const uint32_t kAirPlayRate = 44100;
        bool needResample = (inputRate != kAirPlayRate);
        if (needResample) {
            // 按配置的百分比配重采样器（0 = 关，交给引擎自带的线性插值）
            const int q = cfg ? cfg->qualityLevel : 100;
            resampler.configure(inputRate, kAirPlayRate, q);
            // 品质为 0 时不做重采样，让引擎自己处理
            if (q <= kQualityOff) {
                needResample = false;
            }
        }
        // 告诉引擎送进去的是什么采样率：
        // 我们自己转过 -> 44100（引擎走直通）；没转 -> 原采样率（引擎自己转）
        uint32_t engineInputRate = needResample ? kAirPlayRate : inputRate;

        sender = std::make_unique<RaopSender>(*loop, std::move(ev), std::move(log));
        sender->attachRing(ring.get());
        sender->setInputFormat(engineInputRate);
        sender->setIdentity({"AirPlay Bridge", g_deviceId, "MacBookPro18,3"});
        sender->setAuth(RaopDeviceInfo::Auth::HapTransient, true, "", "", "");
        sender->setVolume(volume);
        if (cfg) sender->setLatencyMs(cfg->latencyMs);   // 延迟必须 start() 前设

        producer = std::thread([this, needResample]() {
            capture->run([this, needResample](const int16_t* d, size_t n) {
                // 先测峰值，再决定要不要推数据。
                // 推之前测是必须的 —— 送进 ring 的是立体声交错样本，
                // 帧数 = 元素数 / 2，两个通道都要看，不然单边声道会漏。
                int peak = 0;
                const size_t samples = n * 2;
                for (size_t i = 0; i < samples; ++i) {
                    const int v = d[i] < 0 ? -static_cast<int>(d[i]) : d[i];
                    if (v > peak) peak = v;
                }
                if (peak >= kSilencePeak) {
                    lastLoudMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
                    silentNow.store(false);
                } else {
                    // 这一块缓冲是静音的。0 秒档位靠这个标志立刻待机。
                    silentNow.store(true);
                }
                if (!needResample) {
                    // 设备本来就是 44.1k，直接送
                    ring->tryPush(std::span<const int16_t>(d, n * 2));
                    return;
                }
                // 转到 44.1k 再送。resampleBuf 复用，避免每回调都分配。
                resampleBuf.clear();
                resampler.process(d, n, resampleBuf);
                if (!resampleBuf.empty())
                    ring->tryPush(std::span<const int16_t>(resampleBuf.data(),
                                                           resampleBuf.size()));
            });
        });

        sender->start(host, 7000, "AirPlay Bridge");

        // 注意：sender->start() 是异步的 —— 它发完握手请求就返回，
        // 真正的结果要等后续 pump() 里才出来（launched 回调）。
        // 所以这里不能按 connected 判断成败，只能先置 running，
        // 让主循环跑起来；失败由 launched(false) 回调负责清掉。
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

    // 进入待机：静音够久了，把整个会话拆掉让接收端屏幕熄灭。
    //
    // 三点必须注意：
    //   1. 不设 userDisconnected —— 那是给"用户手动断开"用的，
    //      设了就再也自动回不来了，而待机必须能自己醒。
    //   2. 不设 wantReconnect —— 那是给掉线重连用的，设了退避循环
    //      会插进来抢，表现为待机期间周期性重连，屏幕反复亮。
    //      而这里由主循环的 standbyActive 分支单独负责唤醒。
    //   3. teardown() 内部会把 wantReconnect 清掉，正是我们要的。
    void enterStandby() {
        if (standbyActive.load()) return;
        logLine("静音超时，进入待机（断开接收端，屏幕应熄灭）");
        teardown();
        standbyActive.store(true);
        standbyWake.store(false);
        running.store(false);

        // 另外开一路轻量采集当"耳朵"。
        // 只测峰值，不推数据 —— 拆会话是为了让屏幕熄灭，
        // 但必须保留"听得到声音"的能力，否则永远醒不来。
        std::string err;
        standbyEar = std::make_unique<WasapiCapture>(WasapiCaptureConfig{});
        if (!standbyEar->open(&err)) {
            // 打不开就没有耳朵，只能靠下次主循环的自动重连兜底 ——
            // 那会周期性试探，屏幕会闪。至少让用户知道。
            logLine("待机采集打开失败: " + err + "（唤醒将退化为周期性重连）");
            standbyEar.reset();
            standbyActive.store(false);   // 退回普通重连路径
            wantReconnect.store(true);
            return;
        }
        standbyThread = std::thread([this]() {
            standbyEar->run([this](const int16_t* d, size_t n) {
                int peak = 0;
                const size_t samples = n * 2;
                for (size_t i = 0; i < samples; ++i) {
                    const int v = d[i] < 0 ? -static_cast<int>(d[i]) : d[i];
                    if (v > peak) peak = v;
                }
                if (peak >= kSilencePeak) {
                    standbyWake.store(true);
                    lastLoudMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
                }
            });
        });
    }

    // 退出待机：听到声音了，立刻把会话建回来。
    //
    // 复用已记住的 host / deviceId / deviceName，不重新扫描设备 ——
    // resolveDevice 要扫约 2.5 秒，等它扫完开头几秒的声音就丢了。
    void leaveStandby() {
        if (!standbyActive.load()) return;
        logLine("检测到声音，退出待机并重连");
        clearStandbyState();
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

// WIN32 子系统的入口。
//
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

// 声明 DPI 感知。
//
// 不声明的话，系统缩放不是 100% 时 Windows 会把整个进程的界面按比例
// 位图放大（DPI 虚拟化）—— 托盘菜单、托盘图标、滑块窗口全部发虚。
// 本机 1920x1080 @125% 实测菜单被放大 1.25 倍，字是糊的。
//
// 必须在创建任何窗口之前调用，所以放在 WinMain 第一行。
static void enableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;

    // Win10 1703+ 的 per-monitor v2，托盘图标能按当前显示器 DPI 取到正确尺寸
    using SetContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    if (auto fn = reinterpret_cast<SetContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
        if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }

    // 更老的系统退回 system-aware
    using SetAwareFn = BOOL(WINAPI*)();
    if (auto fn = reinterpret_cast<SetAwareFn>(
            GetProcAddress(user32, "SetProcessDPIAware"))) {
        fn();
    }
}

// 注意：这里不能用 __argc/__argv —— 在 WIN32 子系统下 CRT 不保证填好它们，
// 实测拿不到参数（--audio-devices 之类会被静默忽略）。
// 改成从 GetCommandLineW 自己切分。
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    enableDpiAwareness();

    // 托盘菜单跟随系统深浅色主题
    darkmode::init();

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return main(0, nullptr);

    // 宽字符 -> UTF-8，凑一份 argv 给 main
    std::vector<std::string> storage;
    std::vector<char*> argv;
    storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0,
                                       nullptr, nullptr);
        std::string u(need > 1 ? static_cast<size_t>(need - 1) : 0, 0);
        if (need > 1)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, u.data(), need,
                                nullptr, nullptr);
        storage.push_back(std::move(u));
    }
    LocalFree(wargv);

    for (auto& x : storage) argv.push_back(x.data());
    int rc = main(static_cast<int>(argv.size()), argv.data());

    // 命令行模式下确保输出刷干净再退出
    std::fflush(stdout);
    std::fflush(stderr);
    return rc;
}


int main(int argc, char** argv) {


    // WIN32 子系统默认没有控制台。命令行模式下要把输出送到终端，
    // 得先 AttachConsole 接上父进程的控制台，再重绑 stdout/stderr。
    // （直接 printf 是没有地方去的 —— 所以这里是必须的。）
    // 命令行参数存在时说明用户是在终端里用的，把输出接出去。
    // 先试 AttachConsole（复用当前终端），失败再 AllocConsole（新开一个窗口）。
    bool hasArgs = argc > 1;
    bool hasConsole = false;
    if (hasArgs) {
        hasConsole = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
        if (!hasConsole) hasConsole = AllocConsole() != 0;
    }
    if (hasConsole) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        // 行缓冲，保证日志能实时看到
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
    }
    // 退出前统一 flush（见文件末尾的 atexit）

    // 判断模式与解析参数
    bool wantList = false, wantInstall = false, wantUninstall = false;
    bool wantConsole = false, wantLast = false, wantTray = true;
    bool wantAudioDevices = false;      // --audio-devices
    std::string setDefaultArg;          // --set-default <设备名子串>
    std::string hostArg;
    int volume = -1;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list")           wantList = true;
        else if (a == "--install")   wantInstall = true;
        else if (a == "--uninstall") wantUninstall = true;
        else if (a == "--console")   { wantConsole = true; wantTray = false; }
        else if (a == "--last")      wantLast = true;
        else if (a == "--audio-devices") { wantAudioDevices = true; wantTray = false; }
        else if (a == "--set-default" && i + 1 < argc) {
            setDefaultArg = argv[++i];
            wantTray = false;
        }
        else if (a == "--volume" && i + 1 < argc) volume = std::atoi(argv[++i]);
        else if (!a.empty() && a[0] != '-') { hostArg = a; wantTray = false; }
    }
    if (wantList || wantInstall || wantUninstall) wantTray = false;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    // 运行时数据在 %LOCALAPPDATA%/AirPlayBridge/
    std::string cfgPath = defaultConfigPath();
    BridgeConfig cfg;
    loadConfig(cfgPath, cfg);

    // 设备标识：第一次运行生成一个，之后一直复用
    if (cfg.deviceId.empty()) {
        cfg.deviceId = makeDeviceId();
        saveConfig(cfgPath, cfg);
    }
    g_deviceId = cfg.deviceId;

    // 老配置里的待机秒数可能不在档位上（比如 20），那样菜单里一个勾都不会显示。
    // 就近吸附到最近的档位。
    if (cfg.standbyEnabled) {
        int best = kStandbyLevels[0].secs;
        int bestD = cfg.standbySecs > best ? cfg.standbySecs - best : best - cfg.standbySecs;
        for (int i = 1; i < kStandbyLevelCount; ++i) {
            const int d = cfg.standbySecs > kStandbyLevels[i].secs
                              ? cfg.standbySecs - kStandbyLevels[i].secs
                              : kStandbyLevels[i].secs - cfg.standbySecs;
            if (d < bestD) { bestD = d; best = kStandbyLevels[i].secs; }
        }
        cfg.standbySecs = best;
    }
    if (volume < 0) volume = cfg.volume;

    // 把加载到的设置写进日志 —— 用户能确认"上次调的值记住了"。
    logLine("已加载设置: 音量 " + std::to_string(cfg.volume) + "%" +
            ", 延迟 " + std::to_string(cfg.latencyMs) + "ms" +
            ", 音质 " + qualityLevelName(cfg.qualityLevel) +
            (cfg.lastDeviceName.empty() ? "" : (", 设备 " + cfg.lastDeviceName)));

    // ---- 一次性命令 ----
    if (wantAudioDevices || !setDefaultArg.empty()) {
        printAudioDevices();
        if (!setDefaultArg.empty()) {
            audioOut("\n");
            if (setDefaultRenderDevice(setDefaultArg)) {
                Sleep(200);
                audioOut("已切换到: %s\n",
                         getDefaultRenderDeviceName().c_str());
            } else {
                audioOut("切换失败: 没有匹配 '%s' 的设备\n",
                         setDefaultArg.c_str());
                CoUninitialize();
                return 1;
            }
        }
        CoUninitialize();
        return 0;
    }

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
    sess.onFirstConnect = [&tray](const std::string& dev) {
        // 首次连上弹一次。托盘图标只有 16px，颜色变化不明显，
        // 新用户需要一个明确的"连上了"信号。
        tray.notify("AirPlay Bridge",
                    dev.empty() ? "已连接" : ("已连接: " + dev));
    };
    sess.onStatus = [&tray](const std::string& s) {
        tray.setTooltip("AirPlay Bridge - " + s);
        logLine(s);
    };
    sess.onConnected = [&tray](bool ok) { tray.setConnected(ok); };

    // 菜单打开期间继续推音频 IO。
    // TrackPopupMenu 自带模态消息循环，主循环在里面完全停摆 ——
    // 不在这儿接上这口气，点一下托盘声音就会断。
    tray.setMenuPump([&]() {
        if (sess.running.load()) sess.pump();
    });

    std::vector<AirPlayDevice> discovered;

    // 尝试连接（托盘模式：优先历史设备；没有历史则自动选 HomePod）
    //
    // skipResolve = true 时不扫描网段，直接用 sess 里已经记住的
    // host / deviceId / deviceName 建会话。待机唤醒走这条路 ——
    // 扫描要 2.5 秒，等它扫完开头的声音已经错过了。
    auto tryConnect = [&](bool skipResolve = false) {
        if (sess.running.load()) return;
        if (skipResolve) {
            if (sess.host.empty()) return;   // 没有可用目标，交给常规重连
            std::string err;
            if (!sess.start(err, true)) {
                tray.setTooltip("AirPlay Bridge - 启动失败");
                tray.notify("AirPlay Bridge", "启动失败: " + err);
            }
        } else if (resolveDevice(sess, cfg, discovered)) {
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
        if (nowDev.find("CABLE") == std::string::npos) {
            // 当前不是虚拟声卡 —— 说明问题已经不存在了（用户自己切回去了，
            // 或者上次其实正常退出只是标记没来得及清）。这条记录已经过期。
            cfg.preCaptureDevice.clear();
            saveConfig(cfgPath, cfg);
        } else {
            // 设备还卡在虚拟声卡上，需要恢复。
            // 注意：只有恢复成功才清标记 —— 恢复失败（比如原来的声卡被拔了）
            // 时必须保留，否则下次启动就不会再试，用户永久没声音。
            logLine("检测到上次异常退出，尝试恢复默认音频设备: " + cfg.preCaptureDevice);
            if (setDefaultRenderDevice(cfg.preCaptureDevice)) {
                logLine("已恢复到: " + cfg.preCaptureDevice);
                cfg.preCaptureDevice.clear();
            } else {
                logLine("恢复失败（设备可能已移除），保留记录以便下次重试");
            }
            saveConfig(cfgPath, cfg);
        }
    }

    // 首次扫描
    if (wantLast || !cfg.lastHost.empty()) {
        tryConnect();
    }

    // 主循环：同时 pump 托盘消息和网络
    auto lastCheck = std::chrono::steady_clock::now();
    // 重连退避：连续失败时逐次翻倍，避免每 5 秒猛扫一次
    int retryBackoffSec = 5;
    // 用户主动断开标记。
    // true = 用户自己点的"断开"，不要自动重连；
    // false = 正常状态（网络断线仍然自动重连）。
    // 没有这个标志的话，用户点完断开 5 秒后它又自己连上了。
    bool userDisconnected = false;
    // 后台扫描（避免阻塞托盘消息循环）
    std::future<std::vector<AirPlayDevice>> scanFuture;
    std::vector<AirPlayDevice> scanResult;

    while (!g_stop.load()) {
        tray.pump([&](TrayEvent e) {
            // 左键和右键都弹菜单。
            // 只认右键的话，习惯左键的人会以为程序坏了 ——
            // 托盘工具没有别的入口，点不开等于失联。
            // 托盘图标的约定本来就是"点它看菜单"，左右键不该有区别。
            if (e != TrayEvent::RightClick && e != TrayEvent::LeftClick) return;

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

            // "选择设备"是个真子菜单（向右展开），内容来自缓存的设备列表。
            // 之所以要缓存：Windows 的菜单树必须在弹出前一次建好，
            // 没法在展开的那一刻去扫网段。
            {
                TrayMenuItem devItem;
                devItem.id = MENU_DEVICES;
                devItem.label = "选择设备";

                if (discovered.empty()) {
                    // 缓存是空的（还没扫过，或者上次啥也没找到）
                    devItem.children.push_back({"尚未扫描到设备", 0, false, false, false});
                } else {
                    for (size_t i = 0; i < discovered.size(); ++i) {
                        TrayMenuItem d;
                        d.label = deviceMenuLabel(discovered[i]);
                        d.id = static_cast<int>(kDeviceMenuBase + i);
                        // 当前连着的打勾
                        d.checked = sess.connected.load() &&
                                    !discovered[i].host.empty() &&
                                    discovered[i].host == sess.host;
                        devItem.children.push_back(std::move(d));
                    }
                }
                devItem.children.push_back({"", 0, true, true, false});
                devItem.children.push_back({"重新扫描", MENU_RESCAN_SUB, true, false, false});
                menu.push_back(std::move(devItem));
            }

            // 延迟 / 音量 / 音质 —— 三个都是音频参数，都做成真子菜单。
            //
            // 内容都是固定的预设档位，不需要扫描，所以可以直接建成
            // 带 children 的子菜单 —— 这样系统会自动加个 › 箭头，
            // 用户一眼就知道能展开。

            // 延迟
            {
                TrayMenuItem it;
                it.id = MENU_LATENCY;   // 父项本身不可点，但保留 id 便于识别
                it.label = "延迟  " + std::to_string(cfg.latencyMs) + "ms";
                for (int i = 0; i < kLatencyLevelCount; ++i) {
                    it.children.push_back({kLatencyLevels[i].label,
                                           kLatencyMenuBase + i,
                                           true, false,
                                           cfg.latencyMs == kLatencyLevels[i].ms});
                }
                it.children.push_back({"", 0, true, true, false});
                // 当前值不等于任何预设档位时，勾打在「自定义」上 ——
                // 光比档位会让滑块调出来的值一个勾都没有。
                bool latencyCustom = true;
                for (int i = 0; i < kLatencyLevelCount; ++i)
                    if (cfg.latencyMs == kLatencyLevels[i].ms) latencyCustom = false;
                it.children.push_back({"自定义（滑块）...", MENU_LATENCY_CUSTOM,
                                       true, false, latencyCustom});
                menu.push_back(std::move(it));
            }

            // 音量
            {
                TrayMenuItem it;
                it.id = MENU_VOLUME;
                it.label = "音量  " + std::to_string(cfg.volume) + "%";
                for (int i = 0; i < kVolumeLevelCount; ++i) {
                    it.children.push_back({kVolumeLevels[i].label,
                                           kVolumeMenuBase + i,
                                           true, false,
                                           cfg.volume == kVolumeLevels[i].pct});
                }
                it.children.push_back({"", 0, true, true, false});
                // 当前值不等于任何预设档位时，勾打在「自定义」上 ——
                // 光比档位会让滑块调出来的值一个勾都没有。
                bool volumeCustom = true;
                for (int i = 0; i < kVolumeLevelCount; ++i)
                    if (cfg.volume == kVolumeLevels[i].pct) volumeCustom = false;
                it.children.push_back({"自定义（滑块）...", MENU_VOLUME_CUSTOM,
                                       true, false, volumeCustom});
                menu.push_back(std::move(it));
            }

            // 音质
            {
                TrayMenuItem it;
                it.id = MENU_QUALITY;
                it.label = std::string("音质  ") + qualityLevelName(cfg.qualityLevel);
                for (int i = 0; i < kQualityLevelCount; ++i) {
                    it.children.push_back({kQualityLevels[i].label,
                                           kQualityMenuBase + i,
                                           true, false,
                                           cfg.qualityLevel == kQualityLevels[i].value});
                }
                it.children.push_back({"", 0, true, true, false});
                // 当前值不等于任何预设档位时，勾打在「自定义」上 ——
                // 光比档位会让滑块调出来的值一个勾都没有。
                bool qualityCustom = true;
                for (int i = 0; i < kQualityLevelCount; ++i)
                    if (cfg.qualityLevel == kQualityLevels[i].value) qualityCustom = false;
                it.children.push_back({"自定义（滑块）...", MENU_QUALITY_CUSTOM,
                                       true, false, qualityCustom});
                menu.push_back(std::move(it));
            }
            menu.push_back({"", 0, true, true, false});
            // 静音自动待机 —— 和延迟 / 音量 / 音质一样做成带 › 箭头的子菜单。
            // 标签里带上当前值，用户不用展开就知道现在是多少。
            {
                TrayMenuItem it;
                it.id = MENU_STANDBY;   // 父项本身不可点，但保留 id 便于识别
                it.label = cfg.standbyEnabled
                               ? ("静音待机  " + std::to_string(cfg.standbySecs) + " 秒")
                               : std::string("静音待机  关闭");
                for (int i = 0; i < kStandbyLevelCount; ++i) {
                    it.children.push_back({kStandbyLevels[i].label,
                                           kStandbyMenuBase + i,
                                           true, false,
                                           cfg.standbyEnabled &&
                                               cfg.standbySecs == kStandbyLevels[i].secs});
                }
                it.children.push_back({"", 0, true, true, false});
                it.children.push_back({"关闭（不自动待机）", MENU_STANDBY + 1000,
                                       true, false, !cfg.standbyEnabled});
                menu.push_back(std::move(it));
            }
            menu.push_back({"开机自启", MENU_AUTOSTART, true, false, isAutoStartEnabled()});
            menu.push_back({"打开日志文件夹", MENU_OPEN_LOG});
            menu.push_back({"", 0, true, true, false});
            menu.push_back({"退出", MENU_QUIT});

            tray.showMenu(menu, [&](int id) {
                // 设备列表里点了一项：直接连它。
                // 这些 id 在子菜单里（kDeviceMenuBase + 序号）。
                if (id >= kDeviceMenuBase &&
                    id < kDeviceMenuBase + static_cast<int>(discovered.size())) {
                    const size_t idx = static_cast<size_t>(id - kDeviceMenuBase);
                    sess.teardown();
                    sess.host = discovered[idx].host;
                    sess.deviceId = discovered[idx].deviceId;
                    sess.deviceName = discovered[idx].name;
                    std::string err;
                    if (!sess.start(err)) {
                        logLine("启动失败: " + err);
                        tray.setTooltip("AirPlay Bridge - 启动失败");
                        tray.notify("AirPlay Bridge", "启动失败: " + err);
                    } else {
                        cfg.lastHost = sess.host;
                        if (!sess.deviceId.empty()) cfg.lastDeviceId = sess.deviceId;
                        if (!sess.deviceName.empty()) cfg.lastDeviceName = sess.deviceName;
                        saveConfig(cfgPath, cfg);
                    }
                    return;
                }

                // 延迟档位
                if (id >= kLatencyMenuBase && id < kLatencyMenuBase + kLatencyLevelCount) {
                    cfg.latencyMs = kLatencyLevels[id - kLatencyMenuBase].ms;
                    saveConfig(cfgPath, cfg);
                    logLine("延迟切换为 " + std::to_string(cfg.latencyMs) + "ms，重连生效");
                    tray.notify("AirPlay Bridge",
                                "延迟已设为 " + std::to_string(cfg.latencyMs) +
                                "ms，正在重连…");
                    // 延迟写在握手参数里，必须重连才生效
                    if (sess.running.load()) {
                        sess.teardown();
                        tryConnect();
                    }
                    return;
                }
                if (id == MENU_LATENCY_CUSTOM) {
                    const int picked = showLatencyDialog(cfg.latencyMs);
                    if (picked > 0) {
                        cfg.latencyMs = picked;
                        saveConfig(cfgPath, cfg);
                        logLine("延迟改为 " + std::to_string(picked) + "ms，重连生效");
                        tray.notify("AirPlay Bridge",
                                    "延迟已设为 " + std::to_string(picked) +
                                    "ms，正在重连…");
                        if (sess.running.load()) {
                            sess.teardown();
                            tryConnect();
                        }
                    }
                    return;
                }

                // 音量档位
                if (id >= kVolumeMenuBase && id < kVolumeMenuBase + kVolumeLevelCount) {
                    const int v = kVolumeLevels[id - kVolumeMenuBase].pct;
                    cfg.volume = v;
                    saveConfig(cfgPath, cfg);
                    // 音量是运行时参数，改了立刻生效，不用重连
                    if (sess.sender) sess.sender->setVolume(v);
                    logLine("音量改为 " + std::to_string(v) + "%");
                    return;
                }
                if (id == MENU_VOLUME_CUSTOM) {
                    const int v = showVolumeDialog(cfg.volume);
                    if (v >= 0) {
                        cfg.volume = v;
                        saveConfig(cfgPath, cfg);
                        if (sess.sender) sess.sender->setVolume(v);
                        logLine("音量改为 " + std::to_string(v) + "%");
                    }
                    return;
                }

                // 音质档位
                if (id >= kQualityMenuBase && id < kQualityMenuBase + kQualityLevelCount) {
                    cfg.qualityLevel = kQualityLevels[id - kQualityMenuBase].value;
                    saveConfig(cfgPath, cfg);
                    logLine("音质档位改为 " + std::to_string(cfg.qualityLevel) + "，重连生效");
                    tray.notify("AirPlay Bridge", "音质已调整，正在重连…");
                    // 重采样器在会话建立时初始化，要重连才生效
                    if (sess.running.load()) {
                        sess.teardown();
                        tryConnect();
                    }
                    return;
                }
                if (id == MENU_QUALITY_CUSTOM) {
                    const int q = showQualityDialog(cfg.qualityLevel);
                    if (q >= 0) {
                        cfg.qualityLevel = q;
                        saveConfig(cfgPath, cfg);
                        logLine("音质档位改为 " + std::to_string(q) + "，重连生效");
                        tray.notify("AirPlay Bridge", "音质已调整，正在重连…");
                        if (sess.running.load()) {
                            sess.teardown();
                            tryConnect();
                        }
                    }
                    return;
                }

                // 静音待机档位
                if (id >= kStandbyMenuBase && id < kStandbyMenuBase + kStandbyLevelCount) {
                    cfg.standbyEnabled = true;
                    cfg.standbySecs = kStandbyLevels[id - kStandbyMenuBase].secs;
                    saveConfig(cfgPath, cfg);
                    logLine("静音待机改为 " + std::to_string(cfg.standbySecs) + " 秒");
                    // 阈值是主循环每次检查现读的，改了立刻生效，不用重连
                    return;
                }
                if (id == MENU_STANDBY + 1000) {   // "关闭（不自动待机）"
                    cfg.standbyEnabled = false;
                    saveConfig(cfgPath, cfg);
                    logLine("静音待机已关闭");
                    // 关掉时如果正处在待机，要立刻醒回来 ——
                    // 否则会卡在"用户以为没开待机，实际连接是断的"。
                    if (sess.standbyActive.load()) {
                        sess.leaveStandby();
                        tryConnect(true);
                    }
                    return;
                }
                if (id == MENU_STANDBY) return;   // 父项，展开用，点它本身不做事

                switch (id) {
                    case MENU_TOGGLE:
                        if (sess.running.load()) {
                            // 用户主动断开：标记一下，别再自动连回来
                            userDisconnected = true;
                            sess.teardown();
                            tray.setConnected(false);
                            tray.setTooltip("AirPlay Bridge - 已断开（手动）");
                            logLine("用户手动断开，暂停自动重连");
                        } else {
                            // 用户手动连接：清掉标记，恢复正常
                            userDisconnected = false;
                            tryConnect();
                        }
                        break;
                    case MENU_RESCAN_SUB:   // 子菜单里的"重新扫描"走同一条路
                    case MENU_RESCAN: {
                        sess.teardown();
                        cfg.lastDeviceId.clear();   // 强制重新选择
                        tray.setConnected(false);
                        tray.setTooltip("AirPlay Bridge - 扫描中");

                        // 先给个反馈再扫。扫描要一两秒，期间主循环是卡住的，
                        // 没有这句话用户会以为点击没生效。
                        tray.notify("AirPlay Bridge", "正在扫描局域网设备…");
                        discovered = filterDevices(discoverAirPlayDevices(2000), cfg);
                        {
                            // 型号、协议版本这类诊断信息写日志，不进菜单
                            std::string detail;
                            for (auto& d : discovered) {
                                if (!detail.empty()) detail += "; ";
                                detail += d.name + " / " + d.model;
                                if (d.isAirPlay2) detail += " / AP2";
                            }
                            logLine("扫描完成，找到 " +
                                    std::to_string(discovered.size()) + " 台设备" +
                                    (detail.empty() ? std::string() : (": " + detail)));
                        }

                        // 不论扫到几台，都弹设备列表。
                        // 原来只有 2 台以上才弹：1 台直接连、0 台毫无动静，
                        // 用户点完菜单就消失，完全不知道发生了什么。
                        std::vector<TrayMenuItem> sub;

                        if (discovered.empty()) {
                            // 扫不到也给个明确交代，并提示该查什么
                            sub.push_back({"未找到设备", 0, false, false, false});
                            sub.push_back({"", 0, true, true, false});
                            sub.push_back({"请确认 HomePod 已通电", 0, false, false, false});
                            sub.push_back({"且与电脑在同一 WiFi", 0, false, false, false});
                            tray.setTooltip("AirPlay Bridge - 未找到设备");
                        } else {
                            for (size_t i = 0; i < discovered.size(); ++i) {
                                sub.push_back({deviceMenuLabel(discovered[i]),
                                               static_cast<int>(1000 + i),
                                               true, false, false});
                            }
                            tray.setTooltip("AirPlay Bridge - 找到 " +
                                            std::to_string(discovered.size()) + " 台设备");
                        }

                        tray.showMenu(sub, [&](int sid) {
                            const size_t idx = static_cast<size_t>(sid - 1000);
                            if (idx >= discovered.size()) return;   // 包括"未找到设备"

                            sess.host = discovered[idx].host;
                            sess.deviceId = discovered[idx].deviceId;
                            sess.deviceName = discovered[idx].name;
                            std::string err;
                            if (!sess.start(err)) {
                                logLine("启动失败: " + err);
                                tray.setTooltip("AirPlay Bridge - 启动失败");
                                tray.notify("AirPlay Bridge", "启动失败: " + err);
                            } else {
                                cfg.lastHost = sess.host;
                                if (!sess.deviceId.empty()) cfg.lastDeviceId = sess.deviceId;
                                if (!sess.deviceName.empty()) cfg.lastDeviceName = sess.deviceName;
                                saveConfig(cfgPath, cfg);
                            }
                        });
                        break;
                    }

                    case MENU_AUTOSTART:
                        setAutoStart(!isAutoStartEnabled());
                        break;
                    case MENU_OPEN_LOG:
                        // 用资源管理器打开数据目录。
                        // 出错时用户要去看日志，那个路径带空格又是隐藏的，
                        // 手打太痛苦 —— 给个入口。
                        ShellExecuteA(nullptr, "open", appDataDir().c_str(),
                                      nullptr, nullptr, SW_SHOWNORMAL);
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

        // ---- 静音自动待机 ----
        // 必须在下面的重连逻辑之前处理：
        // 待机是主动断开，不是掉线，不能被重连退避循环接管。
        {
            const long long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now.time_since_epoch()).count();
            if (sess.standbyActive.load() && sess.standbyWake.load()) {
                // 听到声音了：立刻醒回来。此时 running 是 false，
                // 能过 tryConnect 的 guard。
                sess.leaveStandby();
                tryConnect(true);   // skipResolve：不等那 2.5 秒扫描
                lastCheck = now;
            } else {
                // 0 秒是特例：秒数判据对 0 没意义（now - lastLoudMs 恒 >= 0，
                // 代进去就是"刚连上立刻待机"），改成看 silentNow ——
                // 只要采集侧报告过一块静音缓冲就待机。
                // 刚连上、还没收到任何回调时 silentNow 是 false，
                // 所以不会秒待机（见 ev.launched 里的重置）。
                const bool wantStandby =
                    cfg.standbyEnabled && sess.running.load() && sess.connected.load() &&
                    (cfg.standbySecs <= 0
                         ? sess.silentNow.load()
                         : nowMs - sess.lastLoudMs.load() >=
                               static_cast<long long>(cfg.standbySecs) * 1000);
                if (wantStandby) sess.enterStandby();
            }
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCheck).count() >= retryBackoffSec) {
            lastCheck = now;
            if (sess.running.load() && sess.wantReconnect.load()) {
                sess.wantReconnect.store(false);
                sess.teardown();
                tryConnect();
                // 待机也要排除掉：待机是主动断开，running 会是 false，
                // 不排除的话 5 秒后的下一次重连检查会立刻把它连回来 ——
                // 表现就是「一待机就秒醒」，屏幕反复亮。
            } else if (!sess.running.load() && !sess.standbyActive.load() &&
                       cfg.autoReconnect && !userDisconnected) {
                // 启动时没连上（设备还没通电），或中途掉线。
                // userDisconnected 挡住的只是"用户自己点的断开" ——
                // 那种情况重连是骚扰，不是帮忙。
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
