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
//   airplay_bridge --fix-firewall     添加防火墙入站规则后退出（内部用，由托盘菜单提权拉起）
//   airplay_bridge --clean-firewall-rules  清理指向旧文件的同名放行规则后退出（内部用）
//
// 架构:
//   WASAPI 环回采集 -> RingBuffer -> RaopSender -> 加密 ALAC -> HomePod
//   托盘/命令行共享同一套引擎逻辑；托盘模式下主循环同时 pump 托盘消息和网络。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "discovery.h"
#include "config_store.h"
#include "i18n.h"
#include "version.h"
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
#include <comdef.h>
#include <netfw.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>
#include <cstdlib>
#include <cstring>
#include <timeapi.h>   // timeBeginPeriod（播放期间提高时钟精度）
#include <functional>
#include <future>
#include <memory>
#include <algorithm>
#include <string>
#include <thread>
#include <mutex>
#include <winhttp.h>   // 检查更新
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
// ⚠️ 不要往 250ms 以下加档位当默认值。
//
// 端到端延迟决定接收端预先垫多少音频；垫得越少，网络和调度抖动越容易
// 把缓冲掏空（underrun），听感就是爆音。上游源码里写得很明确：
//「调小可以降低延迟，但太小会导致接收端 underrun（爆音）」
// 并注明 0.25s 才是 AP2 的现实下限（shairport-sync 作者给出的约 250ms）。
//
// 此前默认 150ms、还提供 120ms 档 —— 都在下限以下。实测 120ms 爆音明显、
// 150ms 有间歇性爆音，调到 250ms 以上才干净。
// 档位按实测的可靠程度排（维护者耳朵验的）：
//   105ms  零星爆音      —— 实测能接受的底线
//   110ms  基本不爆      —— 低延迟可用区间的起点
//   150ms  完全流畅
//   200ms  很难听到爆音
//   250ms  AP2 的现实下限（shairport-sync 作者给出的约 250ms）—— 推荐值
//
// 当前档位 130/150/200/250/300 是维护者定的：从 130 起步（低于实测 110 的
// 安全区仍有距离，标「可能爆音」），到 300 更稳。滑块允许 100-300 无极，
// 给愿意试更低值的用户留口子，但档位表不再主动推荐 110 以下。
const LatencyLevel kLatencyLevels[] = {
    { 130,  "130ms（最低）" },
    { 150,  "150ms（低延迟）" },
    { 200,  "200ms（平衡）" },
    { 250,  "250ms（推荐）" },
    { 300,  "300ms（更稳）" },
};
constexpr int kLatencyLevelCount = sizeof(kLatencyLevels) / sizeof(kLatencyLevels[0]);

// 音量档位（百分比）。音量可以运行时改，不用重连。
struct VolumeLevel { int pct; const char* label; };
const VolumeLevel kVolumeLevels[] = {
    {  0,   "静音" },
    { 30,  "30%" },
    { 50,  "50%" },
    { 70,  "70%" },
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
// 隐藏设备 / 采集端点的子项 id 基址（和设备数量无关，各留 100 个位置）
// 「连接时静音电脑扬声器」用到的状态。
//
// g_mutedByUs     —— 是我们把端点静音了吗（只有自己静音的才负责恢复）
// g_wasMutedBefore —— 静音之前用户本来的状态（他本来就静音的话，断开时别给打开）
// 最近一次扫描的**未过滤**设备列表。
//
// 「选择设备」列表用的是过滤后的结果（被隐藏的不出现）；
// 但「隐藏设备」子菜单必须能看到**全部**，否则被隐藏的设备
// 就再也没有入口可以取消隐藏了 —— 用户会以为它消失了。
// 可选采集端点（设备 id + 友好名）。启动时枚举一次 ——
// 菜单树必须在弹出前一次建好，不能等到展开子菜单时再去枚举 COM。
// 当前生效的采集端点 id（UTF-8）。空 = 跟随系统默认输出设备。
//
// 为什么用全局而不是 Session 成员：Session::start() 每次重连都会被调用，
// 而它的调用点分散在 main 的多个 lambda 里，逐个传参会牵动一大片。
// 配置本身也是全局唯一的，放这里语义上并不别扭。
// 采集端点（UTF-8，空 = 跟随系统默认输出设备）。
//
// ★ 用锁保护：main 线程在「菜单切换采集设备」时写它，而 Session::start()
// （主循环线程）在别处读 c_str() 转宽字符。std::string 赋值会释放旧缓冲，
// 读到的指针就悬空了 —— 而「切设备」和「重连」完全可能同时发生。
static std::mutex g_captureDeviceIdMutex;
std::string g_captureDeviceId;

std::string getCaptureDeviceId() {
    std::lock_guard<std::mutex> lk(g_captureDeviceIdMutex);
    return g_captureDeviceId;
}
void setCaptureDeviceId(const std::string& v) {
    std::lock_guard<std::mutex> lk(g_captureDeviceIdMutex);
    g_captureDeviceId = v;
}

// 当前是否处于「虚拟声卡接管」状态。
//
// 接管时程序会把默认输出设备切到虚拟声卡，系统声音全部流向它 ——
// 这时必须采默认设备（也就是虚拟声卡）才能拿到声音。
// 用户指定的采集端点在这个模式下没有意义，采它会「HomePod 没声音、
// 电脑那边还在出声」（隔离也失效）。所以 start() 里要按这个标志跳过。
std::atomic<bool> g_takeoverActive{false};

std::vector<std::pair<std::string, std::string>> g_captureDevices;

std::vector<AirPlayDevice> g_lastScanAll;
std::mutex                 g_lastScanAllMutex;

std::atomic<bool> g_mutedByUs{false};
std::atomic<bool> g_wasMutedBefore{false};

constexpr int kHideDeviceMenuBase    = 1600;
constexpr int kCaptureDeviceMenuBase = 1700;

// 无声音时待机的固定档位（内部单位是十分之一秒）。
// 更短的档位（0/1/3 秒）实测体验很差 —— 一停就断，换歌和安静段落都会误触，
// 所以整档砍掉，只保留 5/6/8/10 秒这四档；中间值走「自定义（滑块）」。
struct StandbyLevel { int tenths; const char* label; };
const StandbyLevel kStandbyLevels[] = {
    {  50, "5 秒" },
    { 100, "10 秒" },
    { 150, "15 秒" },
    { 200, "20 秒" },
};
constexpr int kStandbyLevelCount = sizeof(kStandbyLevels) / sizeof(kStandbyLevels[0]);

// 十分之一秒 -> 显示文本（50 -> "5 秒"，75 -> "7.5 秒"）。
// 整秒不带小数点，避免菜单里出现"5.0 秒"这种多余的尾巴。
std::string tenthsToSecsText(int tenths) {
    const int whole = tenths / 10;
    const int frac  = tenths % 10;
    if (frac == 0) return std::to_string(whole) + i18n::tr(" 秒");
    return std::to_string(whole) + "." + std::to_string(frac) + i18n::tr(" 秒");
}

// 托盘菜单 ID
enum : int {
    MENU_STATUS    = 100,   // 状态（禁用项，只显示）
    MENU_TOGGLE    = 101,   // 连接 / 断开
    MENU_RESCAN    = 102,   // 重新扫描并选择设备
    MENU_RESCAN_SUB = 113,  // 子菜单里的"重新扫描"
    MENU_AUTOSTART = 103,   // 开机自启开关
    MENU_LATENCY   = 105,   // 延迟档位（子菜单）
    MENU_LATENCY_CUSTOM = 106,   // 自定义延迟（无极滑块）
    MENU_VOLUME   = 107,   // 音量（子菜单）
    MENU_VOLUME_CUSTOM = 108,   // 自定义音量（滑块）
    MENU_QUALITY   = 110,   // 音质（子菜单）
    MENU_QUALITY_CUSTOM = 111,   // 自定义音质（滑块）
    MENU_OPEN_LOG  = 109,   // 日志（打开日志文件夹）
    MENU_STANDBY   = 114,   // 无声音时待机（子菜单）
    MENU_STANDBY_CUSTOM = 115,   // 自定义静音待机（无极滑块）
    MENU_FIX_FIREWALL = 116,   // 连接诊断（一键加入站放行规则）
    MENU_CHECK_UPDATE = 118,   // 检查更新（无独立菜单项；由「关于」落下触发）
    MENU_MUTE_LOCAL = 119,      // 连接时静音电脑扬声器（开关）
    MENU_HIDE_DEVICE = 120,     // 隐藏设备（子菜单）
    MENU_CAPTURE_DEVICE = 121,  // 采集哪一个音频端点（子菜单）
    MENU_DEVICE_GROUP = 122,    // 设备（一级子菜单，含 选择/隐藏/采集 三项）
    MENU_ADVANCED = 123,        // 更多设置（一级子菜单，含诊断/待机/静音/自启/日志）
    MENU_LANGUAGE = 124,        // 语言（子菜单）
    MENU_LANGUAGE_EN = 125,
    MENU_LANGUAGE_ZH = 126,
    MENU_ABOUT     = 117,   // 关于（版本 + 项目主页）
    MENU_QUIT      = 104,   // 退出
};

// 版本号由 CMake 在编译期注入（见 CMakeLists.txt 的
// target_compile_definitions APP_VERSION），值来自 project(... VERSION x.y.z)。
//
// 唯一的版本来源就是 CMakeLists.txt 那一行 —— 源码里不再手写版本号，
// 所以不会再出现"发版忘了改这里"的情况（v1.2.33 就是这样漏掉的）。
// 兜底 0.0.0 只对"脱离 CMake 直接编译"的情况生效。
#ifndef APP_VERSION
#define APP_VERSION "0.0.0"
#endif
constexpr const char* kAppVersion = APP_VERSION;

// 项目主页。「关于」里点一下跳这里。
constexpr const char* kProjectUrl = "https://github.com/RiverFlowsInUUU/AirPlay-Bridge";

// ============================================================
// 检查更新（托盘菜单）
// ============================================================
//
// 为什么不用更省事的办法：项目要单文件、不装运行库，所以不能引 libcurl
// 之类的东西。WinHTTP 是系统自带的，链接 winhttp.lib 就能用，
// 而且原生支持 TLS，正好。
//
// 注意 GitHub API 对没有 User-Agent 的请求会返回 403，必须带上。
constexpr const char* kLatestReleaseApi =
    "https://api.github.com/repos/RiverFlowsInUUU/AirPlay-Bridge/releases/latest";

// 版本比较已抽到 version.h（纯函数，抽出去才能单测）。
// 这里用 using namespace fxchain，所以调用点不用改。

// 检查更新的结果。网络线程填、主线程读 —— 用 mutex + atomic 传递。
enum class UpdateState { Failed, UpToDate, HasNew };
struct UpdateResult {
    UpdateState state = UpdateState::Failed;
    std::string tag;
    std::string url;
};
UpdateResult           g_updateResult;
std::mutex             g_updateMutex;
std::atomic<bool>      g_updateReady{false};
// 防连点：检查更新进行中就不再起第二个线程。
// 必须放在文件作用域 —— 跑它的 lambda 是无捕获的（见 MENU_CHECK_UPDATE）。
std::atomic<bool>      g_updateRunning{false};

// 从 GitHub 的 JSON 里抠出某个字符串字段（够用就行，不引 JSON 库）。
std::string jsonField(const std::string& body, const std::string& key) {
    const std::string pat = std::string(1, '"') + key + std::string(1, '"');
    size_t pos = body.find(pat);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos + pat.size());
    if (pos == std::string::npos) return "";
    pos = body.find('"', pos);
    if (pos == std::string::npos) return "";
    const size_t end = body.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return body.substr(pos + 1, end - pos - 1);
}

// 拉一次最新 release。成功返回 true，结果填进 tag / url。
bool fetchLatestRelease(std::string& tag, std::string& url) {
    HINTERNET hS = WinHttpOpen(L"AirPlayBridge/" L"" APP_VERSION,
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return false;

    // 5 秒超时：检查更新是个可选动作，不该让用户等太久。
    WinHttpSetTimeouts(hS, 5000, 5000, 5000, 5000);

    bool ok = false;
    std::string body;
    HINTERNET hC = WinHttpConnect(hS, L"api.github.com",
                                  INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (hC) {
        HINTERNET hR = WinHttpOpenRequest(hC, L"GET",
                                          L"/repos/RiverFlowsInUUU/AirPlay-Bridge/releases/latest",
                                          nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          WINHTTP_FLAG_SECURE);
        if (hR) {
            // User-Agent 必须带 —— 不带的话 GitHub API 直接 403。
            WinHttpAddRequestHeaders(hR,
                L"User-Agent: AirPlayBridge" L"", (DWORD)-1L,
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            WinHttpAddRequestHeaders(hR,
                L"Accept: application/vnd.github+json", (DWORD)-1L,
                WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

            if (WinHttpSendRequest(hR, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hR, nullptr)) {
                DWORD code = 0, len = sizeof(code);
                WinHttpQueryHeaders(hR,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &len,
                    WINHTTP_NO_HEADER_INDEX);
                if (code == 200) {
                    char buf[4096];
                    DWORD got = 0;
                    while (WinHttpReadData(hR, buf, sizeof(buf), &got) && got > 0) {
                        body.append(buf, got);
                        if (body.size() > 512 * 1024) break;   // 防御：别被撑爆
                    }
                    ok = true;
                }
            }
            WinHttpCloseHandle(hR);
        }
        WinHttpCloseHandle(hC);
    }
    WinHttpCloseHandle(hS);
    if (!ok) return false;

    tag = jsonField(body, "tag_name");
    url = jsonField(body, "html_url");
    return !tag.empty();
}

// 静音判定的峰值阈值（int16 满量程 32767）。
//
// 20 约等于 -64dBFS。原先是 300（-40dBFS），太高了 —— 实测：
//
//   | 场景                        | 采集到的峰值 |
//   |-----------------------------|--------------|
//   | 真安静（数字静音）           | 0            |
//   | 球赛解说安静时的现场环境声   | 30 ~ 158     |
//   | 钢琴每拍衰减到尾部           | 334          |
//   | 正常内容                     | 1700 ~ 5000  |
//
// 球赛那种"小但确实有声"的内容**一次都超不过 300**，于是被判成全程静音，
// 表现为频繁待机（老板实际遇到的问题）。
//
// 20 的取值：远高于实测的数字静音（0），又低于最轻的真实内容（30）。
// 之所以往低取 —— 阈值偏低只会"该待机时不待机"（用户可把待机时长调长缓解），
// 偏高则会"有声音也待机"（用户没有别的办法，只能关掉待机）。
constexpr int kSilencePeak = 20;

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
    // 日志会被多个线程写（主循环、看门狗、网络 pump、更新检查）。
    // fopen/fprintf/fclose 是逐次调用，不加锁两个线程会交错写同一行，
    // 排查时看到的日志就是断的。锁的范围就包住这一次写入。
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lk(logMutex);
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
    // 这里拿到的路径要写进防火墙规则、还要跟规则里存的路径做等值比较。
    // 不要试图用 GetModuleFileNameA(nullptr, nullptr, 0) 先探长度 ——
    // 实测这个 API 传空指针直接返回 0 / ERROR_INSUFFICIENT_BUFFER，
    // 它不支持两段式调用。照着写会得到永远为空的路径，
    // 而空路径拿去和防火墙规则里的路径比较永远不相等 ——
    // 表现为「明明已放行，程序却说没放行」。
    //
    // 用足够大的缓冲：216 是 Windows 上不含 \\?\ 前缀的最大路径长度，
    // 留到 1024 足够覆盖长路径场景；真被截断也不会报错，
    // 只是那条防火墙规则会对不上（下次启动会提示重建）。
    std::string buf(1024, static_cast<char>(0));
    const DWORD got = GetModuleFileNameA(nullptr, buf.data(),
                                         static_cast<DWORD>(buf.size()));
    if (got == 0 || got >= buf.size()) return std::string();
    buf.resize(got);   // resize 到实际长度，末尾不残留空字符
    return buf;
}

// 弹窗正文统一走这里：源码是 UTF-8，直接喂给 MessageBoxA 的话，
// 系统会按当前代码页（中文机器上是 936/GBK）解码，中文会变成乱码。
// 显式转成 UTF-16 再走 MessageBoxW。
//
// （--fix-firewall 那一份里也有一份同样的转换，只是它是 lambda；
//   这里是给启动检查用的，不碰它，避免动到 v1.2.29-37 的那块逻辑。）
std::wstring screenText(const std::string& s) {
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    // need 含结尾 NUL 且函数会写入 need 个字符 —— 必须按 need 分配，
    // 否则每次调用都越界写 2 字节（v1.3.1 修过同类 4 处，这里漏了）。
    if (need <= 1) return std::wstring();
    std::wstring w(static_cast<size_t>(need), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), need);
    w.resize(static_cast<size_t>(need - 1));   // 去掉结尾 NUL
    return w;
}

// 本地编码（A 版 API 的字符串）-> UTF-16。
//
// ★ 约定（2026-10-18 统一，P2-6）：**防火墙那一条链上的 std::string 一律是
//   系统本地编码（ACP）**，不是 UTF-8。原因是源头：
//     · exePath() / netshExePath() 用 GetModuleFileNameA / GetSystemDirectoryA
//     · exePath() 还被 ShellExecuteA（启动失败时打开日志、打开安装目录）消费
//   所以「改成 UTF-8」会连带弄坏那些调用方；统一到 ACP 才是改动最小、
//   且对 A 版 API 完全自洽的选择（ACP 字节正是 A 版 API 自己能表示的那批）。
//
//   原来这个文件里有三套写法并存：firewallRuleExists 逐字节扩宽、
//   findOtherCopies/findStale 用 CP_ACP、runCommandLine 也是逐字节扩宽
//   而 deleteFirewallRuleByNameEx 为了配合它把规则名编成 UTF-8 ——
//   于是一条命令里 exe 路径是 ACP、规则名是 UTF-8，谁也不对谁。
//   非 ASCII 路径下 firewallRuleExists 会「明明已放行却说没放行」。
std::wstring acpToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    // need 含结尾 NUL，所以按 need 分配、再 resize(need-1)。
    const int need = MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(need), 0);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, out.data(), need);
    out.resize(static_cast<size_t>(need - 1));
    return out;
}

// ============================================================
// 单实例检测（命名互斥体）
// ============================================================
// 为什么必须有这道闸：AirPlay 接收端只服务一路。同一台机器开两个实例、
// 或两台机器各开一个去连同一个 HomePod，都会互相把对方踢掉
// （连上 -> 被踢 -> 自动重连 -> 再踢，无限循环，声音反复中断）。
// 已实际踩到过：日志特征为「会话还在跑的时候又发起新连接」
// 加 RAOP connection closed by the device。
//
// 名字固定、不带版本号 —— 新旧版本之间也要能互相检测到。
//
// 为什么用 Global\ 前缀：不加的话互斥体落在调用者自己的会话命名空间里，
// 一个跑在 RDP 会话、一个跑在控制台的进程看到的不是同一个对象，检测不到。
// 加 Global\ 才会落到全局（跨会话）命名空间。
//
// 为什么要用互斥体而不是 FindWindow / EnumWindows 找窗口：
// 托盘程序本来就没有（也不需要）可见窗口，窗口查找根本看不见它；
// 而且进程名、窗口类名一旦变（发版改名）就会漏判。互斥体是内核对象，
// 与窗口、与版本都无关。
//
// 返回的句柄必须活到进程退出（放静态里）—— 句柄一关，互斥体就被回收，
// 后面再启动的进程就会以为"没有实例"。进程退出时由系统自动释放。
HANDLE g_singleInstanceMutex = nullptr;

// 已经有实例在跑时弹提示。文案用 wstring + static_cast<wchar_t>(10) 拼换行。
// 正在弹窗等用户回答。
//
// MessageBox 是**模态**的 —— 主线程在等用户作答，这是预期行为，不是卡住。
// 看门狗看到主循环不转就会每分钟写一条"主线程无响应"，
// 首次运行连着问两个框就能把日志刷满，真出问题时反而被淹掉。
std::atomic<bool> g_inUserPrompt{false};


// 统一的弹窗入口：期间给看门狗打豁免。
// 所有 MessageBox 都要走这里，别直接调 —— 漏一个就漏一个误报源。
int promptUser(const wchar_t* text, const wchar_t* title, UINT flags) {
    g_inUserPrompt.store(true);
    // 这里必须调真正的 MessageBoxW —— 注意别被"把所有弹窗换成 promptUser"
    // 的批量替换误伤成自己（那样会无限递归，而且**编译照样通过**）。
    const int r = MessageBoxW(nullptr, text, title, flags);
    g_inUserPrompt.store(false);
    return r;
}

// 安装程序调用的一次性工具参数 —— 必须豁免单实例检测。
//
// 为什么：安装的最后一步（以及卸载清理）会拉起本程序跑这些参数，
// 而那时用户机器上**很可能正开着程序**（开机自启），于是被单实例检测
// 当成「重复启动」静默退出 —— 结果是放行规则没配上。
//
// 用户看到的就是「装了新版还是连不上，而且每次打开都提示未被防火墙放行」。
// （实测复现：装完版本正确，但放行规则 0 条。）
//
// 这些参数只做一次性动作，不起托盘、不进主循环、不碰接收端，
// 与「同时跑两个实例会互相抢接收端」是两回事。
// ⚠️ 不要往里加 --last 或任何正常启动参数。
bool isOneShotToolArg() {
    const wchar_t* cmd = GetCommandLineW();
    if (!cmd) return false;
    static const wchar_t* kTools[] = {
        L"--fix-firewall",
        L"--clean-firewall-rules",
        L"--uninstall-cleanup",
        L"--audio-devices",
        // ⚠️ 不要加 --console。
        //
        // 它不是一次性工具参数 —— 它会起引擎、去连接收端（只是不显示托盘）。
        // 放进来就等于允许两个实例同时抢同一个接收端，表现为互相踢、
        // 声音反复中断。这跟本函数注释里那条「不碰接收端」直接冲突。
    };
    for (const wchar_t* t : kTools) {
        if (wcsstr(cmd, t)) {
            logLine("单实例检测: 一次性工具调用，豁免检测");
            return true;
        }
    }
    return false;
}

bool anotherInstanceIsRunning() {
    if (isOneShotToolArg()) return false;

    constexpr const wchar_t* kMutexName = L"Global\\AirPlayBridge_SingleInstance";
    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (!g_singleInstanceMutex) {
        // 句柄都拿不到（极罕见）：不把用户挡在门外，放行。
        logLine("单实例检测: CreateMutexW 失败（" +
                std::to_string(GetLastError()) + "），跳过检测");
        return false;
    }
    // 注意：ERROR_ALREADY_EXISTS 不是"失败"，只是告诉我们别人已经建过了。
    // 句柄照样要留着（见上），所以这里不做任何清理。
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;

    logLine("单实例检测: 已有 AirPlay Bridge 在运行，本进程退出");


    std::wstring title = L"AirPlay Bridge";
    std::wstring msg;
    msg += i18n::trw("已有 AirPlay Bridge 在运行。");
    msg += static_cast<wchar_t>(10);
    msg += static_cast<wchar_t>(10);
    msg += i18n::trw("同时运行两个会导致声音反复中断，请先关闭另一个再启动本程序。");

    // MB_SETFOREGROUND：托盘程序是后台进程，不加的话弹窗可能被压在
    // 别的窗口后面，用户只看到进程一闪而过，完全不知道发生了什么。
    promptUser(msg.c_str(), title.c_str(),
                MB_OK | MB_SETFOREGROUND);
    return true;
}

// ============================================================
// 防火墙入站规则的修复（托盘菜单「更多设置 › 连接诊断」）
// ============================================================
// 背景见下面「握手超时的防火墙诊断」。结论是：缺的就是一条入站 UDP
// 放行规则，加规则必须管理员权限 —— 所以这条路径是：托盘菜单提权拉起
// 自己（带 --fix-firewall），被拉起的那一份加规则、弹提示、退出。
//
// 规则名固定。加之前先删同名旧规则：这样重复点菜单不会堆出一串规则，
// 而且用户把 exe 挪了位置之后，规则里的 program 路径也会更新到当前 exe
// （同名规则只留一条，就是刚加的那条）。
const char* const kFirewallRuleName = "AirPlay Bridge inbound UDP";

// 前置声明：deleteFirewallRuleByName() 要用这两个，
// 而它们的定义在下面（findStaleFirewallRules 依赖上面的常量与 exePath）。
std::vector<std::wstring> findStaleFirewallRules();
// 所有前缀匹配的规则名（不只「多余的」）—— 加规则前清场用。
std::vector<std::wstring> findAllOurFirewallRules();
bool deleteFirewallRuleByNameEx(const std::wstring& name);

// netsh 的**完整路径** —— 不能只写 "netsh"。
//
// 实测：本程序从托盘启动时继承的是 Explorer 的环境，PATH 里当然有
// C:\Windows\System32；但用户也可能从 Git Bash / 各种便携终端的快捷方式
// 启动，那些环境会把 System32 从 PATH 里挤掉（实测这类环境下
// cmd /c netsh 直接报「不是内部或外部命令」，退出码 9009）。
// 写死 "netsh" 会在那种环境下一声不响地失败 —— 用户点了修复却什么都没发生。
//
// 用 %SystemRoot% 拼绝对路径，不写反斜杠转义（本项目工具链会写坏它）。
std::string netshExePath() {
    const char* root = std::getenv("SystemRoot");
    // 兜底值用两个字符拼：反斜杠不能写成字面转义 ——
    // 写成 "C:" 时，反斜杠会把结尾的引号转义掉，字符串变成 C: + 一个引号，
    // 于是路径成了 C:"/System32/netsh.exe（cmd 报「不是内部或外部命令」）。
    std::string base = (root && *root) ? std::string(root)
                      : std::string("C") + std::string(1, static_cast<char>(92));
    return base + "/System32/netsh.exe";
}

// 跑一条命令行，返回退出码。
//
// 不用 std::system()：它会把命令交给 cmd /c，而 cmd 对「以引号开头的命令」
// 有一套自己的引号剥离规则，会把 "C:/.../netsh.exe" advfirewall ... 里的
// program="带 空格 的路径" 咬坏 —— 实测报「不是内部或外部命令」。
// 直接把命令行交给 CreateProcessW，不做任何 shell 解析，最稳。
int runCommandLine(const std::string& cmd) {
    if (cmd.empty()) return -1;
    // ★ 按本地编码显式转换，不逐字节扩宽 —— 见 acpToWide 的约定说明。
    // CreateProcessW 需要一个可写的缓冲区
    std::wstring w = acpToWide(cmd);
    std::vector<wchar_t> buf(w.begin(), w.end());
    buf.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr,
                                   FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si, &pi);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

// 日志 / 提示里的程序路径要动态取 —— 引号用 char 常量拼，
// 不写反斜杠转义（本项目工具链会写坏它）。
// 这里加的是**本程序自己**的路径：规则覆盖的是本进程，不是别的东西。
//
// 注意：给用户看的那条命令里仍然写 "netsh"（用户在命令行里敲，
// 他们的 PATH 没问题，短名字更好复制）。真正执行的走 netshExePath()。
std::string netshAddRuleArgs(bool useFullNetshPath) {
    const char QC = '"';
    std::string cmd = useFullNetshPath ? (QC + netshExePath() + QC) : std::string("netsh");
    cmd += " advfirewall firewall add rule name=";
    cmd += QC; cmd += kFirewallRuleName;              cmd += QC;
    cmd += " dir=in action=allow protocol=UDP program=";
    cmd += QC; cmd += exePath();                      cmd += QC;
    cmd += " remoteip=localsubnet profile=any";
    return cmd;
}

// 删掉**所有前缀匹配**的旧规则（不只是精确同名的那条）。
// 返回 false 表示 netsh 执行失败（多数是没管理员权限）。
//
// 为什么必须连带序号的一起删：netsh 遇到重名会自动加序号，于是同一台
// 机器上会攒出 "... UDP"、"... UDP 1"、"... UDP 2"……。只按精确名字删
// 的话，带序号的一条都删不掉 —— 这正是序号堆积的根源。
// 这里是「加之前先清干净」的语义：清完之后系统里只会剩下面新加的那一条。
//
// 用 netsh 而不是 INetFwPolicy2：写规则走 COM 需要管理员权限时
// 一样会失败，而 netsh 的命令行跟上面日志里给用户看的那条完全一致，
// 出问题时日志和用户手册能对得上。
//
// 删的是"我们前缀匹配"的那些 —— 别的 AirPlay 规则（"AirPlay Bridge UDP
// Range" 等）名字对不上前缀，一条都不会被碰到。用 findStaleFirewallRules()
// 而不是自己拼 netsh 通配：它走 INetFwPolicy2 枚举 + 前缀比较，判据与
// 启动检查完全同一套，不会出现"检查时算一条、删的时候删另一批"的错位。
// ⚠️ 这个函数必须删掉**所有**前缀匹配的规则，不能只删「多余的」。
//
// 原因（实测踩到，导致放行规则归零）：
//   1. netsh 的 `delete rule name="X"` 会把**同名的所有规则一起删**，
//      没法只删其中一条 —— 所以「精确地只删多余那条」本来就做不到。
//   2. 更糟的是加规则前若不把同名规则清干净，netsh add 会**再产生一条同名**。
//      于是先删一条（结果两条都没了）、或者保留一条再新增（变成两条同名），
//      两种情况最后都是一场空。
//
// 真实表现：覆盖安装时旧规则还在 → 加规则前没删掉它 → 变成两条同名 →
// 随后「清理多余规则」按名字删 → **两条一起消失** → 放行规则 0 条 →
// 接收端 UDP 回连被丢 → 连不上 + 每次启动都提示未被防火墙放行。
//
// 所以这里的语义就是「加规则前先把同名规则全部清掉」，然后干净地加一条。
bool deleteFirewallRuleByName() {
    // ⚠️ 必须用「全部」而不是「多余的」。理由见上方那段长注释：
    //   - netsh 按名字删会把同名的**一起删**，做不到"只删多余那条"
    //   - 加规则前若不清干净同名规则，netsh add 会再产生一条同名 →
    //     随后任何按名字的删除都会把它们一起删光 → 放行规则归零
    const std::vector<std::wstring> ours = findAllOurFirewallRules();
    if (ours.empty()) return true;   // 本来就没有，等价于删成功
    bool ok = true;
    for (const std::wstring& name : ours) {
        if (!deleteFirewallRuleByNameEx(name)) ok = false;
    }
    return ok;
}

// 遍历所有防火墙规则，对每条调用 fn(rule)。
// fn 返回值：true = 继续遍历，false = 提前停止。
//
// 这里是**唯一**一处 COM 枚举骨架：
//   CoInitializeEx/CoUninitialize 的配对（含「调用方已经初始化过 COM
//   （RPC_E_CHANGED_MODE 等）就不重复初始化」的判断）、
//   NetFwPolicy2 -> get_Rules -> get__NewEnum -> IEnumVARIANT ->
//   QueryInterface(INetFwRule) 的每一步，以及所有 COM 对象和 BSTR/VARIANT
//   的释放，全部收敛在这里。调用方只写业务判据。
//
// 调用方因此不需要（也不允许）自己碰 policy / rules / enumerator —— 
// 提前 return、fn 返回 false 停遍历、枚举中途失败，走的都是同一条收尾路径，
// 不可能漏释放。
//
// 注：规则组是全局的、与 profile 无关，所以这里不按 profile 循环。
void forEachFirewallRule(const std::function<bool(INetFwRule*)>& fn) {
    // 已初始化过 COM 时不重复初始化（RPC_E_CHANGED_MODE 说明本线程
    // 已是别的 apartment，此时 CoInitializeEx 返回该值但 COM 是可用的）。
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(hrInit) || hrInit == RPC_E_CHANGED_MODE;

    INetFwPolicy2* policy = nullptr;
    INetFwRules* rules = nullptr;
    // 提前声明：后面收尾统一释放
    if (SUCCEEDED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr,
                                   CLSCTX_INPROC_SERVER,
                                   __uuidof(INetFwPolicy2),
                                   reinterpret_cast<void**>(&policy))) && policy &&
        SUCCEEDED(policy->get_Rules(&rules)) && rules) {
        IEnumVARIANT* enumerator = nullptr;
        if (SUCCEEDED(rules->get__NewEnum(reinterpret_cast<IUnknown**>(&enumerator))) &&
            enumerator) {
            VARIANT item;
            VariantInit(&item);
            while (enumerator->Next(1, &item, nullptr) == S_OK) {
                bool keepGoing = true;
                if (item.vt == VT_DISPATCH && item.pdispVal) {
                    INetFwRule* rule = nullptr;
                    if (SUCCEEDED(item.pdispVal->QueryInterface(
                            __uuidof(INetFwRule),
                            reinterpret_cast<void**>(&rule))) &&
                        rule) {
                        keepGoing = fn(rule);
                        rule->Release();
                    }
                }
                VariantClear(&item);
                VariantInit(&item);
                if (!keepGoing) break;
            }
            VariantClear(&item);
            enumerator->Release();
        }
    }
    if (rules)  rules->Release();
    if (policy) policy->Release();
    if (needUninit) CoUninitialize();
}

// 已经加过规则了吗？（菜单项「只在需要时才出现」的判据）
//
// 判断「本程序当前这个 exe」是否已经放行。
//
// 必须同时比对**规则名**和**规则指向的 exe 路径** —— 只看名字会出错：
// 规则绑定的是 exe 路径，用户换版本/换目录/改文件名之后旧规则还在，
// 但它指向的是已经不存在的文件。只看名字就会误判成「已配好」，
// 于是拒绝执行修复，用户点了菜单还是连不上。
bool firewallRuleExists() {
    bool found = false;
    forEachFirewallRule([&](INetFwRule* rule) -> bool {
        BSTR name = nullptr;
        if (SUCCEEDED(rule->get_Name(&name)) && name) {
            std::wstring wide(name);
            // 名字对了还不够 —— 还要看这条规则到底放行的是不是
            // 当前这个 exe。
            //
            // 规则是绑定 exe **路径**的。用户下载了新版本、换了目录
            // 或改了文件名之后，旧规则仍然在（名字还叫这个），但它
            // 指向的是那个已经不存在的老文件。只看名字的话程序会
            // 以为「已经配好了」而拒绝干活 —— 用户看到的就是
            // 「点了修复却说已搞过，但还是连不上」。
            if (wide == L"AirPlay Bridge inbound UDP") {
                BSTR appPath = nullptr;
                if (SUCCEEDED(rule->get_ApplicationName(&appPath)) &&
                    appPath && *appPath) {
                    const std::string want = exePath();
                    // ★ 逐字节扩宽会把 ACP 路径里 >0x7F 的字节当成单个码点，
                    //   于是和注册表里的值对不上 ——「明明已放行却提示未放行」。
                    const std::wstring cur = acpToWide(want);
                    if (_wcsicmp(appPath, cur.c_str()) == 0) {
                        found = true;
                    }
                }
                if (appPath) SysFreeString(appPath);
            }
        }
        if (name) SysFreeString(name);
        return !found;   // 找到了就停
    });
    return found;
}

// 找**多余的**放行规则：本程序加过的那些，按「指向哪个 exe」分组，
// 每个 exe 路径只保留一条。
//
// 怎么攒出来的：netsh 遇到重名会自动加序号（"... UDP 1"、"... UDP 2"……），
// 而规则真正生效的是 program=（exe 完整路径）。于是同一台机器上会出现：
//
//   AirPlay Bridge inbound UDP      <- 指向当前 exe（正在起作用）
//   AirPlay Bridge inbound UDP 1    <- 也指向当前 exe（纯冗余）
//   AirPlay Bridge inbound UDP 2    <- 也指向当前 exe（纯冗余）
//   AirPlay Bridge inbound UDP 5    <- 指向已经不存在的旧 exe（陈旧）
//
// ⚠️ 只认**精确前缀** "AirPlay Bridge inbound UDP"。
// 本机还可能有别的 AirPlay 规则（"AirPlay Bridge UDP Range"、
// "AirPlay Bridge TCP Range"、"AirPlay Sender EXE"……），
// 那些**不是本程序加的**，一条都不能碰 —— 前缀多加一个字符就能把它们
// 排除干净，所以这里坚持用前缀 + 长度比较，而不是 find()。
//
// 判据（按规则指向的 exe 路径分组，每组只留一条）：
//   - 分组 A：ApplicationName == 当前 exePath() —— 正在起作用，**保留一条**，
//             其余同组的（带序号的冗余）收进待删列表。
//             ⚠️ 老板明确要求：正在起作用的规则绝不能删。
//   - 分组 B：ApplicationName != 当前 exePath() —— 旧版本 / 已删除 /
//             已换目录 / 已改名，那条规则已经不起作用，**全部收进待删列表**。
//
// 分组 A 里保留哪一条：优先留下名字恰好等于 kFirewallRuleName（不带序号）
// 的那条；没有的话（例如第一条被改名了）就留下枚举到的第一条。
//
// 与 firewallRuleExists() 用同一套接口（INetFwPolicy2 / INetFwRules /
// INetFwRule），同一个枚举写法。
std::vector<std::wstring> findStaleFirewallRules() {
    std::vector<std::wstring> stale;

    const std::wstring prefix(
        kFirewallRuleName, kFirewallRuleName + std::strlen(kFirewallRuleName));
    // 不带序号的那个名字：分组 A 里优先保留它。
    const std::wstring exact(kFirewallRuleName,
                             kFirewallRuleName + std::strlen(kFirewallRuleName));
    // 当前 exe 的宽字符形式：规则里存的是宽字符路径，比较要同类型。
    // exePath() 返回的是 A 版 API 拿到的**系统本地编码**字符串，
    // 用 CP_ACP 转回宽字符才能和注册表里的值逐字符对上 ——
    // 这就是这条链统一用 ACP 的依据（见 acpToWide 的说明）。
    std::wstring curExe;
    {
        const std::string p = exePath();
        const int need = MultiByteToWideChar(CP_ACP, 0, p.c_str(), -1, nullptr, 0);
        if (need > 1) {
            curExe.resize(static_cast<size_t>(need));
            MultiByteToWideChar(CP_ACP, 0, p.c_str(), -1, curExe.data(), need);
        }
    }

    // 两趟：先枚举收集，再在内存里分组做决定。
    // 不能边枚举边删 —— 分组 A 要「留下哪一条」得先看全同组的所有名字。
    std::vector<std::wstring> ours;      // 名字（前缀匹配的全部）
    std::vector<bool>        isCurrent;  // 与 ours 一一对应：是否指向当前 exe
    std::vector<bool>        aliveOther; // 指向的是**另一个确实存在**的文件

    // 规则组是全局的、与 profile 无关，一趟枚举收齐。
    forEachFirewallRule([&](INetFwRule* rule) -> bool {
        BSTR name = nullptr;
        if (SUCCEEDED(rule->get_Name(&name)) && name) {
            const std::wstring wide(name);
            // 前缀匹配（含不带序号的那条本身）。
            // 别的 AirPlay 规则前缀对不上，天然被排除。
            if (wide.size() >= prefix.size() &&
                wide.compare(0, prefix.size(), prefix) == 0) {
                BSTR appPath = nullptr;
                bool sameAsCurrent = false;
                bool otherFileExists = false;
                if (SUCCEEDED(rule->get_ApplicationName(&appPath)) &&
                    appPath) {
                    sameAsCurrent =
                        (_wcsicmp(appPath, curExe.c_str()) == 0);
                    if (!sameAsCurrent) {
                        otherFileExists =
                            (GetFileAttributesW(appPath) !=
                             INVALID_FILE_ATTRIBUTES);
                    }
                }
                if (appPath) SysFreeString(appPath);
                ours.push_back(wide);
                isCurrent.push_back(sameAsCurrent);
                aliveOther.push_back(otherFileExists);
            }
        }
        if (name) SysFreeString(name);
        return true;   // 收齐全部，不提前停
    });

    // keep[i] = true 表示第 i 条要保留，其余收进待删列表。
    std::vector<bool> keep(ours.size(), false);

    // 分组 B：指向**别的** exe 的规则 —— 只有那个文件**已经不在了**才删。
    //
    // ⚠️ 这里曾经是「只要不指向当前 exe 就删」，后果很严重：
    // 机器上同时留着两份副本时（比如换了安装目录但旧的没卸干净），
    // 放行规则只能指向其中一份 —— 另一份每次启动都报「未被防火墙放行」，
    // 而且**真的连不上**；用户点多少次「连接诊断」都没用，
    // 因为修复只会把规则抢到自己这边、下次打开另一份又重复。
    //
    // 现在按「文件还在不在」分：活着的另一份是条**合法的**放行，留着；
    // 死路径才清。既不堆积垃圾，也不会把有效副本顶掉。
    for (size_t i = 0; i < ours.size(); ++i) {
        if (isCurrent[i]) continue;          // 指向当前 exe 的走分组 A
        keep[i] = aliveOther[i];
    }

    // 分组 A：只留一条 —— 优先"名字恰好是 kFirewallRuleName"的那条，
    // 否则留枚举到的第一条（同一份 exe 的重复规则，留一条就够）。
    size_t keepCurrentIdx = static_cast<size_t>(-1);
    for (size_t i = 0; i < ours.size(); ++i) {
        if (!isCurrent[i]) continue;
        if (keepCurrentIdx == static_cast<size_t>(-1)) keepCurrentIdx = i;
        if (ours[i] == exact) { keepCurrentIdx = i; break; }
    }
    if (keepCurrentIdx != static_cast<size_t>(-1)) keep[keepCurrentIdx] = true;

    for (size_t i = 0; i < ours.size(); ++i) {
        if (!keep[i]) stale.push_back(ours[i]);
    }
    return stale;
}

// 找「指向**另一份确实还存在**的副本」的放行规则，返回那些副本的完整路径。
//
// 为什么需要它：放行规则按 exe 完整路径记录，一台机器上只能有一份生效。
// 用户同时留着安装版和免安装版时，两份会互相顶掉对方的设置 —— 谁点一次
// 「修复」就把另一份顶掉，于是两份轮流提示、永远修不好，而且其中一份真的连不上。
//
// 光说「未被放行」会让人一头雾水（「我明明配过」）。所以要把另一份的**路径**
// 直接摆出来，用户才能动手删。
//
// ⚠️ 只在**文件确实还在**时才算 —— 指向已删除文件的规则是纯粹的陈旧垃圾，
//    由 findStaleFirewallRules() 那条路清理，不该在这里冒充「另一份副本」。
std::vector<std::wstring> findOtherCopies() {
    std::vector<std::wstring> out;

    std::wstring curExe;
    {
        const std::string p8 = exePath();
        const int need = MultiByteToWideChar(CP_ACP, 0, p8.c_str(), -1, nullptr, 0);
        if (need > 1) {
            curExe.resize(static_cast<size_t>(need));
            MultiByteToWideChar(CP_ACP, 0, p8.c_str(), -1, curExe.data(), need);
        }
    }
    if (curExe.empty()) return out;

    const std::wstring prefix(
        kFirewallRuleName, kFirewallRuleName + std::strlen(kFirewallRuleName));

    forEachFirewallRule([&](INetFwRule* rule) -> bool {
        BSTR name = nullptr;
        if (SUCCEEDED(rule->get_Name(&name)) && name) {
            const std::wstring wide(name);
            if (wide.size() >= prefix.size() &&
                wide.compare(0, prefix.size(), prefix) == 0) {
                BSTR appPath = nullptr;
                if (SUCCEEDED(rule->get_ApplicationName(&appPath)) && appPath) {
                    const std::wstring other(appPath);
                    const bool isOther =
                        (_wcsicmp(other.c_str(), curExe.c_str()) != 0);
                    const bool exists =
                        (GetFileAttributesW(other.c_str()) != INVALID_FILE_ATTRIBUTES);
                    if (isOther && exists &&
                        std::find(out.begin(), out.end(), other) == out.end()) {
                        out.push_back(other);
                    }
                }
                if (appPath) SysFreeString(appPath);
            }
        }
        if (name) SysFreeString(name);
        return true;
    });
    return out;
}

// 列出**所有**前缀匹配的放行规则名（不只「多余的」）。
//
// 加规则前的清场要用它 —— 理由见 deleteFirewallRuleByName 上方那段。
// 前缀比较的写法与 findStaleFirewallRules 完全一致，避免两处判据漂移。
std::vector<std::wstring> findAllOurFirewallRules() {
    std::vector<std::wstring> out;
    const std::wstring prefix(
        kFirewallRuleName, kFirewallRuleName + std::strlen(kFirewallRuleName));
    forEachFirewallRule([&](INetFwRule* rule) -> bool {
        BSTR name = nullptr;
        if (SUCCEEDED(rule->get_Name(&name)) && name) {
            const std::wstring w(name);
            if (w.size() >= prefix.size() &&
                w.compare(0, prefix.size(), prefix) == 0)
                out.push_back(w);
        }
        if (name) SysFreeString(name);
        return true;   // 收齐全部
    });
    return out;
}

// 删掉一条指定名字的放行规则（**只能删我们自己的名字** ——
// 传进来的名字必须先确认过前缀，调用方负责）。
//
// 用 netsh 而不是 INetFwPolicy2::Remove：命令行跟日志/用户手册里
// 给用户看的那条完全一致，出问题时两边对得上。
bool deleteFirewallRuleByNameEx(const std::wstring& name) {
    if (name.empty()) return false;
    const char QC = '"';
    // 规则名里带空格，必须整体加引号；netsh 走的是 CreateProcess 直接
    // 传参（不经 cmd），不会被引号规则咬到。
    //
    // 名字从 BSTR 转成窄字符：走 WideCharToMultiByte 而不是迭代器构造。
    // 迭代器构造会把每个 wchar_t 按低字节截断（编译期 C4244），
    // 名字里一旦有超出 ASCII 的字符就会拼出一条不存在的规则名 ——
    // 删不掉，还一声不响。
    //
    // 用 CP_ACP：这条链统一走本地编码（见 acpToWide 的约定说明）。
    // 原来为了让「逐字节扩宽」碰巧还原正确，这里特意编成 UTF-8 ——
    // 结果是同一条命令里 exe 路径是 ACP、规则名是 UTF-8，谁也不对谁。
    // 现在两端都统一成 ACP，不再需要那种配合。
    // 规则名是我们自己生成的 ASCII 常量，ACP 一定表示得下。
    std::string narrow;
    {
        const int need = WideCharToMultiByte(CP_ACP, 0, name.c_str(), -1,
                                             nullptr, 0, nullptr, nullptr);
        if (need > 1) {
            narrow.resize(static_cast<size_t>(need));
            WideCharToMultiByte(CP_ACP, 0, name.c_str(), -1, narrow.data(),
                                need, nullptr, nullptr);
        }
    }
    if (narrow.empty()) return false;
    std::string cmd = QC + netshExePath() + QC;
    cmd += " advfirewall firewall delete rule name=";
    cmd += QC; cmd += narrow; cmd += QC;
    const int rc = runCommandLine(cmd);
    // netsh 在"规则不存在"时也返回 0，这里只看负值（进程起不来）。
    return rc >= 0;
}

// 删掉所有多余规则（重复的 / 指向旧文件的），返回删掉的条数。
//
// 只删 findStaleFirewallRules() 收进来的那些 —— 其中**不含**指向当前 exe
// 且被判定"要保留"的那一条。正在起作用的规则绝不在这里被删掉。
//
// ⚠️ 日志里只写条数，**不写规则名也不写路径** —— 用户目录的完整路径
// 属于隐私，日志可能被贴到 issue 里。
int cleanStaleFirewallRules() {
    const std::vector<std::wstring> stale = findStaleFirewallRules();
    int removed = 0;
    for (const std::wstring& name : stale) {
        if (deleteFirewallRuleByNameEx(name)) ++removed;
    }
    if (removed > 0) {
        logLine("已清理 " + std::to_string(removed) + " 条多余防火墙规则");
    } else if (!stale.empty()) {
        logLine("清理多余防火墙规则失败（可能没有管理员权限），共 " +
                std::to_string(stale.size()) + " 条");
    }
    return removed;
}

// 启动时检查放行规则是否指向**当前这个 exe**，不匹配就提醒用户去修。
//
// 为什么必须放在启动时做：
// 规则绑定的是 exe 路径。用户下载了新版本、换目录、改文件名之后，
// 旧规则还在（名字没变），但指向的是那个已经删掉的老文件 ——
// 新 exe 实际上没有被放行。等到用户连不上、自己去点菜单才发现，
// 中间他已经困惑半天了。启动时一句话就能省掉这个过程。
//
// 只在**确实有问题**时才打扰用户：规则指向当前 exe 就完全静默。
// 也不在这里直接修 —— 加规则要管理员权限，会弹 UAC，
// 让用户在还没搞清楚状况的时候先吃一个 UAC 弹窗是不礼貌的。
// 前置声明：两个提权入口定义在下面，而启动检查要用它们。
bool elevateAndRunFixFirewall();
bool elevateAndRunCleanFirewallRules();
bool setAutoStart(bool enable);   // 前置声明：卸载清理要用

// 播放期间把系统时钟精度提到 1ms。
//
// 为什么需要：Windows 默认时钟粒度是 15.6ms，会让 std::this_thread::sleep_for
// 实际睡成两倍长 —— 主循环从设计的 20ms 变成 ~30ms，而音频 pacer 的发送节拍
// 也跟着从 8ms 拉长到 ~30ms（发包变成一批一批的，对 150ms 这种浅缓冲不友好）。
// 实测：开启后主循环单轮耗时 37ms → 21ms，回到设计值。
//
// 功耗考量（查过微软文档与社区实测）：
//   - Win10 2004 起，这个调用**只影响本进程**，不再拖累整个系统；
//   - Win11 下本进程窗口被遮挡/最小化时，系统会自动收回高精度；
//   - 但它确实会阻止 CPU 进入省电态 —— 所以**只在播放期间开、停止立刻关**，
//     不播放时零成本。这才是关键：待机/空闲时这个程序不占任何时钟开销。
//
// 必须成对调用；用 static 保证幂等，避免 begin/end 配不上。
void setHighResTimer(bool on) {
    static bool active = false;
    if (on == active) return;
    if (on) timeBeginPeriod(1);
    else    timeEndPeriod(1);
    active = on;
    logLine(on ? "时钟精度已提到 1ms（播放期间）"
               : "时钟精度已恢复默认（停止播放）");
}

void checkFirewallRuleAtStartup() {
    // 一次启动最多弹**一个**窗口 —— 两个提示框叠着弹只会让用户烦。
    // 所以两件事（"还没放行" / "有陈旧规则"）在这里先各自算出结论，
    // 再合成一次 MessageBox。
    const bool ruleOk = firewallRuleExists();
    const int staleCount =
        static_cast<int>(findStaleFirewallRules().size());


    // ★ 还要单独查「本机有没有另一份副本」——不能挂在 staleCount 上。
    //
    // findStaleFirewallRules() 会把「指向另一份**仍存在**副本」的规则判为
    // keep=true（那是刻意的：活着的另一份是条合法放行），所以两份并存时
    // staleCount 恰好是 0。原来这里直接 return，于是「另一份在哪」的提示
    // 永远不会出现 —— 而那正是它唯一该出现的场景。
    //
    // findOtherCopies() 会做 COM 枚举，比 firewallRuleExists() 贵；
    // 但一次启动只调一次，且它本来就在同一个函数里被调用，成本可接受。
    const std::vector<std::wstring> others = findOtherCopies();

    // 三件都没问题才静默。绝大多数启动走这条路 —— 不能每次开机都弹框烦人。
    if (ruleOk && staleCount == 0 && others.empty()) return;

    const wchar_t kNLw = static_cast<wchar_t>(10);
    std::wstring tip;
    bool wantFixOnYes = false;     // 点「是」= 修复放行（要提权）
    bool wantCleanOnYes = false;   // 点「是」= 清理陈旧规则

    if (!ruleOk) {
        logLine("启动检查: 本程序尚未被防火墙放行，可能影响连接");
        tip += i18n::trw("AirPlay Bridge 还没有被防火墙放行。");
        tip += kNLw; tip += kNLw;
        tip += i18n::trw("这会让声音连不上或频繁中断。");

        // 如果本机存在指向**另一份副本**的放行规则，说清楚 ——
        // 否则用户会疑惑「我明明配好了，怎么还提示」。
        // 放行规则按 exe 完整路径记，两份副本会互相顶掉对方的设置：
        // 谁点一次修复就把另一份的规则覆盖掉，于是两份轮流提示。
        if (!others.empty()) {
            // 把「另一份到底在哪」直接摆出来 —— 只说「建议只保留一份」，
            // 用户根本不知道该删哪个文件。
            logLine("启动检查: 检测到另一份副本占用了放行设置（两份会互相顶）");
            tip += kNLw; tip += kNLw;
            // 外层已经判过 !others.empty()（见上面），这里不必再判一次 ——
            // 原来这里嵌了一层同条件的 if，内层恒真、else 分支不可达。
            tip += i18n::trw("本机还有另一份 AirPlay Bridge，占着网络放行设置：");
            tip += kNLw;
            for (size_t i = 0; i < others.size() && i < 2; ++i) {
                tip += L"    ";
                tip += others[i];
                tip += kNLw;
            }
            tip += kNLw;
            tip += i18n::trw("两个文件不能同时用（放行设置只能指向一个），");
            tip += kNLw;
            tip += i18n::trw("而且会互相顶掉，所以每次打开都提示。");
            tip += kNLw;
            tip += i18n::trw("请删掉上面那个不用的，再用你常用的这个打开。");
        }
        wantFixOnYes = true;
    }

    if (staleCount > 0) {
        logLine("启动检查: 检测到 " + std::to_string(staleCount) +
                " 条多余的防火墙规则（同一条放行被重复添加，或指向旧文件）");
        // 已经有主提示时隔一行再追加，读起来是两段而不是一串。
        if (!tip.empty()) { tip += kNLw; tip += kNLw; }
        tip += i18n::trw("检测到 ") + std::to_wstring(staleCount) +
               i18n::trw(" 条多余的网络放行设置（重复添加，或指向已不存在或已更换位置的文件）。");
        tip += kNLw; tip += kNLw;
        tip += i18n::trw("它们不影响使用，但会让系统里的设置变乱。");
        wantCleanOnYes = true;
    }

    // 点「是」做什么，由上面哪几件事成立决定。
    // 三件事都成立（没放行 + 有陈旧规则）时：提权拉起的那一份跑完
    // --fix-firewall 之后顺带把陈旧规则也清掉，所以这里只提一个请求 ——
    // 用户只看到一次 UAC，再点一下就好。
    if (wantFixOnYes) {
        tip += kNLw;
        tip += i18n::trw("点「是」立即修复（需要管理员权限）。");
        tip += kNLw; tip += kNLw;
        tip += i18n::trw("也可以稍后从托盘菜单点「更多设置 › 连接诊断」。");
    } else {
        tip += kNLw;
        tip += i18n::trw("点「是」清理，点「否」跳过。");
    }

    // MB_SETFOREGROUND：托盘程序没有前台窗口，不加的话弹窗可能被压在别的
    // 窗口后面，用户只看到进程一闪而过。
    const int r = promptUser(tip.c_str(), L"AirPlay Bridge",
                              MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND);
    if (r != IDYES) {
        if (staleCount > 0) logLine("用户选择跳过陈旧防火墙规则清理");
        return;
    }

    if (wantFixOnYes) {
        logLine("用户在启动提示里选择了立即修复，请求提权添加防火墙入站规则");
        // 提权那一份会在加完规则后顺带清理陈旧规则（见 runFixFirewall）。
        elevateAndRunFixFirewall();
    } else if (wantCleanOnYes) {
        logLine("用户在启动提示里选择了清理陈旧防火墙规则，请求提权");
        elevateAndRunCleanFirewallRules();
    }
}


// --fix-firewall 的入口（被提权拉起的那一份跑它）。
//
// 约束：这一份**不起托盘图标、不跑主循环** —— 做完就退。
// 用户点了菜单总得有反馈，所以做完必须弹一个 MessageBox；
// 提权进程一闪而过会让人以为没生效。
// quiet = true 时不弹提示框，只写日志 —— 给安装程序用。
// 用户在等安装完成，不该再冒一个框让他点；失败与否有日志，
// 而且程序启动时的自检还会再提示一次（有兜底）。
int runFixFirewall(bool quiet = false) {
    logLine("--fix-firewall: 开始添加防火墙入站规则");

    // 先删同名旧规则，再加 —— 保证只有一条，且 program 指向当前 exe。
    // 删不掉不致命（可能本来就没有），继续往下走。
    if (!deleteFirewallRuleByName())
        logLine("--fix-firewall: 删除同名旧规则失败（可能没有管理员权限），继续尝试添加");

    // 直接跑 netsh.exe（不走 cmd.exe）：绝对路径 + 参数全部带引号，
    // cmd 的引号剥离规则反而会咬到 program= 里那个带空格的路径
    // （实测 "C:\...\cmd.exe /c "C:\...\netsh.exe" ..." 会报
    //   「不是内部或外部命令」，退出码 1）。
    const std::string cmd = netshAddRuleArgs(true);
    logLine("--fix-firewall: 执行 " + cmd);
    const int rc = runCommandLine(cmd);

    // 顺手把陈旧规则清掉：这一份已经是管理员了，而启动检查里
    // 「还没放行 + 有陈旧规则」那一路就是靠这里一次性收拾干净的
    // —— 用户不会被要求点两次 UAC。
    cleanStaleFirewallRules();

    int exitCode = 0;
    std::string title;
    std::string msg;
    if (rc == 0) {
        logLine("已添加防火墙入站规则（允许接收端回连）");
        title = "AirPlay Bridge";
        msg  = i18n::tr("已修复，程序正在重连。");
        msg += char(10);
        msg += char(10);
        msg += i18n::tr("已添加防火墙入站规则，允许接收端回连本机做时钟同步。");
        msg += char(10);
        // 规则对已经建立的连接不一定立刻生效，所以提醒一句，
        // 而不是让用户对着一动不动的界面猜。
        msg += i18n::tr("若仍连不上，请断开后重新连接一次（或重启程序）。");
    } else {
        logLine("--fix-firewall: 添加防火墙入站规则失败（返回值 " +
                std::to_string(rc) + "）");
        title = "AirPlay Bridge";
        msg  = i18n::tr("修复失败：无法添加防火墙入站规则。");
        msg += char(10);
        msg += char(10);
        msg += i18n::tr("请确认在弹出的用户账户控制窗口中选择了「是」。");
        exitCode = 1;
    }

    // MessageBoxW 需要宽字符，从 UTF-8 转过去（源码含中文，
    // 走 A 版会按系统代码页解码，这里显式转）。
    auto toWide = [](const std::string& s) {
        int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
        if (need <= 1) return std::wstring();
        std::wstring w(static_cast<size_t>(need), 0);   // need 含 NUL，见 screenText
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), need);
        w.resize(static_cast<size_t>(need - 1));
        return w;
    };
    if (!quiet) {
        promptUser(toWide(msg).c_str(), toWide(title).c_str(),
                   MB_OK | MB_SETFOREGROUND);
    } else {
        logLine("--fix-firewall: 静默模式，跳过提示框");
    }
    return exitCode;
}

// --clean-firewall-rules 的入口（被提权拉起的那一份跑它）。
//
// 只做一件事：删掉指向旧文件的同名放行规则，然后弹提示、退出。
// 同样不起托盘、不跑主循环 —— 与 --fix-firewall 一个待遇。
//
// 为什么不就地用 INetFwPolicy2::Remove()：不是接口不好用，是 netsh
// 的命令行跟日志里给用户看的那条能对上，排查时少一层猜测。
// 而且这一条路径本来就在"有问题"时才走，多起一个 netsh 进程无所谓。
int runCleanFirewallRules() {
    logLine("--clean-firewall-rules: 开始清理陈旧防火墙规则");

    const int n = cleanStaleFirewallRules();

    std::string msg;
    if (n > 0) {
        msg  = std::string(i18n::tr("已清理 ")) + std::to_string(n) + i18n::tr(" 条旧的网络放行设置。");
        msg += char(10);
        msg += char(10);
        msg += i18n::tr("它们指向的文件已经不存在了，清理不影响正在使用的放行设置。");
    } else {
        // 走到这里通常有两种情况：读取规则时被挡（GROUP POLICY 禁了
        // INetFwPolicy2），或者删的动作没成功。都如实说。
        msg  = i18n::tr("没有可清理的旧设置，或清理未能完成。");
        msg += char(10);
        msg += char(10);
        msg += i18n::tr("请确认在弹出的用户账户控制窗口中选择了「是」。");
    }

    promptUser(screenText(msg).c_str(), L"AirPlay Bridge",
                MB_OK | MB_SETFOREGROUND);
    // 提示框不是失败信号 —— 与 --fix-firewall 那里不同，这里没有"失败"的
    // 概念（没东西可删也是正常结束）。
    return 0;
}

// 提权拉起自己跑 --fix-firewall。用户会看到一次 UAC 弹窗 ——
// 加防火墙规则必须管理员，这个弹窗省不掉。
// 返回值只表示「有没有把提权请求递出去」，不代表规则加成功了
// （结果由那一份自己弹的 MessageBox 告知用户）。
bool elevateAndRunFixFirewall() {
    const char QC = '"';
    std::string params = "--fix-firewall";

    SHELLEXECUTEINFOA sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    // NULL = 当前显示的窗口，系统自己挑一个父窗口挂 UAC 弹窗
    sei.hwnd   = nullptr;
    sei.lpVerb = "runas";
    sei.lpFile = exePath().c_str();
    sei.lpParameters = params.c_str();
    sei.nShow  = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&sei)) {
        const DWORD err = GetLastError();
        // 用户在 UAC 弹窗里点了「否」—— 这不是错误，是用户的选择。
        // 必须静默处理并写日志：不弹提示就让人干等才是真问题。
        if (err == ERROR_CANCELLED)
            logLine("已取消防火墙修复（UAC 弹窗被拒绝）");
        else
            logLine("拉起防火墙修复失败（错误码 " + std::to_string(err) + "）");
        return false;
    }

    // 等那一份跑完再回收句柄。**不要等太久就 WaitForSingleObject INFINITE**：
    // 它在等用户点 MessageBox，可能停很久 —— 那期间主循环走的是托盘
    // 菜单的模态消息循环，主界面会僵住。这里等一小会儿拿结果，
    // 没结束就放手（子进程自己会退，句柄由系统回收）。
    if (sei.hProcess) {
        const DWORD waited = WaitForSingleObject(sei.hProcess, 20000);
        if (waited == WAIT_OBJECT_0) {
            DWORD code = 1;
            if (GetExitCodeProcess(sei.hProcess, &code) && code == 0)
                logLine("防火墙入站规则已就绪，等待下一轮重连生效");
            else
                logLine("防火墙修复进程返回失败（退出码 " +
                        std::to_string(static_cast<unsigned long>(code)) + "）");
        } else {
            logLine("防火墙修复进程仍在运行（等待 UAC / 提示框），不再等待");
        }
        CloseHandle(sei.hProcess);
    }
    return true;
}

// 提权拉起自己跑 --clean-firewall-rules（清理陈旧规则）。
//
// 与 elevateAndRunFixFirewall 同一套路，只是换参数 —— 这里不复用它
// 是因为两者的日志文案、返回语义不同，硬套会让"谁在清理、谁在添加"
// 在日志里分不清。用户同样会看到一次 UAC 弹窗。
bool elevateAndRunCleanFirewallRules() {
    std::string params = "--clean-firewall-rules";

    SHELLEXECUTEINFOA sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd   = nullptr;
    sei.lpVerb = "runas";
    sei.lpFile = exePath().c_str();
    sei.lpParameters = params.c_str();
    sei.nShow  = SW_SHOWNORMAL;

    if (!ShellExecuteExA(&sei)) {
        const DWORD err = GetLastError();
        // 用户在 UAC 弹窗里点了「否」—— 这是用户的选择，不是错误。
        if (err == ERROR_CANCELLED)
            logLine("已取消清理陈旧防火墙规则（UAC 弹窗被拒绝）");
        else
            logLine("拉起陈旧防火墙规则清理失败（错误码 " +
                    std::to_string(err) + "）");
        return false;
    }

    // 同 --fix-firewall：只等一小会儿。那一份在等用户点提示框，
    // 无限等会把主界面拖死。
    if (sei.hProcess) {
        const DWORD waited = WaitForSingleObject(sei.hProcess, 20000);
        if (waited == WAIT_OBJECT_0) {
            DWORD code = 1;
            if (GetExitCodeProcess(sei.hProcess, &code) && code == 0)
                logLine("陈旧防火墙规则清理进程已结束");
            else
                logLine("陈旧防火墙规则清理进程返回失败（退出码 " +
                        std::to_string(static_cast<unsigned long>(code)) + "）");
        } else {
            logLine("陈旧防火墙规则清理进程仍在运行（等待 UAC / 提示框），不再等待");
        }
        CloseHandle(sei.hProcess);
    }
    return true;
}

// 卸载前清理系统集成：删掉防火墙放行规则和开机自启项。
//
// 由安装程序在卸载时调用（Inno 的 [UninstallRun]）。这两样都是
// **按 exe 路径**写进系统的 —— 程序被删掉之后，它们会变成指向
// 一个不存在文件的死记录，留着只会让人困惑。
//
// 不弹窗、不起托盘、不读配置；配置和日志**保留**（用户重装还能续上设置）。
// 想彻底清干净的话，卸载说明里写了路径。
//
// 注意它和 --fix-firewall / --clean-firewall-rules 一样要**豁免单实例检测** ——
// 卸载时托盘里那个实例可能还开着，不能被自己挡住。
// 删掉本程序的**全部**防火墙规则（卸载时用）。
//
// 别拿 deleteFirewallRuleByName() 顶替 —— 那个的语义是
// 「删掉多余的、**保留**正在起作用的那条」（v1.2.39 为启动自检设计的）。
// 卸载时那条正好也指向要被删掉的 exe，必须一起清，
// 否则系统里就留下一条指向不存在文件的死规则。
//
// 这种「两个函数名字像、语义不同」的地方最容易用错。所以这里单开一个，
// 名字也写清楚是 All。
void removeAllFirewallRules() {
    const std::wstring prefix(
        kFirewallRuleName, kFirewallRuleName + std::strlen(kFirewallRuleName));
    std::vector<std::wstring> names;
    forEachFirewallRule([&](INetFwRule* rule) -> bool {
        BSTR name = nullptr;
        if (SUCCEEDED(rule->get_Name(&name)) && name) {
            const std::wstring w(name);
            if (w.compare(0, prefix.size(), prefix) == 0) names.push_back(w);
        }
        if (name) SysFreeString(name);
        return true;   // 收齐全部
    });
    for (const auto& n : names) deleteFirewallRuleByNameEx(n);
}

bool runUninstallCleanup() {
    logLine("--uninstall-cleanup: 开始清理系统集成（防火墙规则 / 开机自启）");

    removeAllFirewallRules();
    logLine("--uninstall-cleanup: 防火墙放行规则已清空");

    if (setAutoStart(false))
        logLine("--uninstall-cleanup: 开机自启项已移除");
    else
        logLine("--uninstall-cleanup: 移除开机自启项失败（可能本来就没有）");

    logLine("--uninstall-cleanup: 完成");
    return true;
}

// ============================================================
// 握手超时的防火墙诊断
// ============================================================
// 接收端要主动回连做时钟同步，本机必须有一条入站 UDP 规则。
// 没有这条规则的话，「无主入站 UDP」会被系统静默丢掉：
//
//   RECORD 200 OK 之后，接收端向发送端在 session SETUP 里声明的
//   timingPort / controlPort 发 UDP（NTP 式时钟同步），收不到回话
//   就拿不到时间基准 —— 于是它不回应 stream SETUP。我们这边干等到
//   10 秒超时，界面永远停在「连接中」。
//
// 上游源码里就有这条提示（third_party/airplay2-sender-cpp/src/
// raop_sender.cpp:490 附近的注释）。放行入站规则后同一份 exe
// 3/3 成功 —— 所以这是环境问题，不是协议问题。
//
// 提示只在连续失败够多次之后打一次：重连循环每几秒跑一轮，
// 每轮都写一遍会把日志刷成废纸。

// 连续握手失败计数（每次 launched(false) 加一，连上一次清零）。
std::atomic<int> g_handshakeFailStreak{0};
// 已经提示过防火墙了吗？只提示一次，直到下一次连接成功才复位。
std::atomic<bool> g_firewallHintShown{false};
// 托盘菜单里已经出现过「连接诊断」了吗？
// 出现之后就不再关掉：一旦让用户看见它，menu 每弹一次都去枚举一遍
// 防火墙规则太贵，而且用户点完没成功时这项必须还在（还得能再点）。
std::atomic<bool> g_fixFirewallOffered{false};
// 提示的触发阈值。低于它不提示 —— 偶发一次超时很常见（接收端忙、
// 网络抖一下），先别让用户去翻防火墙。
// 这次启动如果没扫到设备，要不要给个气泡提示。
//
// 只在**首次运行**那一次置起：那时用户面对的是灰图标 + 菜单里
//「尚未扫描到设备」，完全不知道发生了什么，需要一句话说明。
// 之后就靠 tooltip 静态提示即可 —— 重连循环里反复弹气泡是骚扰。
// 用 atomic：主循环消费，启动阶段写入。
std::atomic<bool> g_notifyIfNoDevice{false};

// 音频链路的健康计数器 —— 专门用来定位「杂音 / 爆音」。
//
// 为什么需要：`ring->tryPush()` 在环满时返回 false，而调用处一直忽略返回值，
// 于是「丢帧」这条最经典的爆音线索完全看不见。
// 丢帧 = 音频不连续 = 咔哒/爆裂声；削波 = 波形被削平 = 失真。
// 两个都记下来，主循环每秒汇总一次（见 logAudioHealth）。
std::atomic<uint64_t> g_ringDroppedFrames{0};   // 推不进环、被丢掉的**帧**数
// ↑ 单位是帧，不是样本。ring 里存的是交错样本（帧数 x 2），
//   加进这里之前必须先换算 —— 2026-10-18 之前一直加的是样本数，
//   于是日志「累计 N 帧」立体声下虚报一倍。
std::atomic<uint64_t> g_clippedSamples{0};      // 峰值触顶（可能削波）的样本数
// 诊断用：本窗口内采集到的最大峰值。
// 排查「杂音/爆音」时必须知道**采集侧到底有多大电平** ——
// 如果一直是 0 或接近 0，那问题在采集（比如把音频送到一个没接设备的
// 数字输出上，系统几乎不渲染），而不在后续链路。
std::atomic<int> g_peakWindowMax{0};

constexpr int kFirewallHintAfterFails = 3;

// 如果本机已经有一条覆盖本程序的入站放行规则，就说明防火墙大概率
// 本程序当前这个 exe 是否已经放行（判据与启动检查完全一致）。
//
// 这里**不要**再写一套宽松的"名字里含 airplay 就算"的判据 ——
// 那会和启动检查得出相反结论（启动说"尚未放行"，超时提示说"已有规则"），
// 同一个问题两个答案，排查时反而误导。统一走 firewallRuleExists()。
bool hasInboundRuleForThisApp() {
    return firewallRuleExists();
}

// 打那条「握手超时，去放行入站 UDP」的诊断提示。
//
// 由 launched(false) 回调调用。计数不到阈值什么都不做；
// 到了阈值、且本机还没有放行规则时写一条。规则已存在则跳过 ——
// 那说明防火墙不是原因，再提示只会误导。
void maybeLogFirewallHint() {
    const int streak = g_handshakeFailStreak.fetch_add(1) + 1;
    if (streak < kFirewallHintAfterFails) return;
    // 只提示一次。用 exchange 原子置位，避免多线程重入时写两遍。
    bool expected = false;
    if (!g_firewallHintShown.compare_exchange_strong(expected, true)) return;

    if (hasInboundRuleForThisApp()) {
        logLine("握手连续超时 " + std::to_string(streak) +
                " 次，但本机已有本程序的入站放行规则 —— 若仍连不上，"
                "请检查接收端与本机是否在同一网段。");
        return;
    }

    std::string msg;
    msg += "握手超时。常见原因：Windows 防火墙拦了本程序的入站 UDP ——";
    msg += char(10);
    msg += "接收端需要通过 UDP 回连本机做时钟同步，收不到就不会回应 stream SETUP。";
    msg += char(10);
    msg += "放行方法（管理员命令提示符）：";
    msg += char(10);
    // 命令行用短名字 "netsh"（用户在终端里敲，PATH 是好的，短的好复制）；
    // 程序自己执行时走绝对路径，见 netshExePath()。
    // 程序路径必须动态取 —— 用户装在哪都有可能。
    msg += "  ";
    msg += netshAddRuleArgs(false);
    msg += char(10);
    // 上面那条命令是给老用户 / 远程排查用的。真正的用户路径是菜单：
    // 没人会去敲命令 —— 所以必须给出菜单里的入口。
    msg += "也可在托盘菜单点「更多设置 › 连接诊断」一键添加规则。";
    logLine(msg);
}

// ============================================================
// 主线程看门狗
// ============================================================
// 问题现场在别人的机器上，抓不到。所以让程序自己报告"卡在哪"。
//
// 做法：主循环每轮把当前时间戳和"正在做的事"写进两个原子变量，
// 看门狗线程每 500ms 检查一次 —— 超过 2 秒没更新就认为主线程卡住了，
// 把最后阶段和已卡住秒数写进日志。
//
// 卡住期间按固定节奏持续记录（每 1 秒一条），所以日志里能直接看出
// 总共卡了多久；恢复正常时再补一条收尾。
std::atomic<long long> g_mainTick{0};   // 主循环每轮更新（steady_clock 毫秒）

// 主循环"当前正在做的事"。这是可能耗时的操作清单 —— 每个值都对应
// 一处已知会阻塞主线程的调用（扫描网段、开采集设备、握手、弹菜单……）。
enum MainPhase : int {
    PHASE_IDLE = 0,        // 空闲（每轮循环开头重置成这个）
    PHASE_SCAN,            // 扫描设备
    PHASE_CAPTURE_OPEN,    // 打开音频采集
    PHASE_CONNECT,         // 建立连接
    PHASE_MENU_OPEN,       // 菜单打开中
    PHASE_NET_PUMP,        // 驱动网络
    PHASE_CAPTURE_STOP,    // 等采集线程退出（卡在这 = 采集停不下来）
    PHASE_COUNT,
};

std::atomic<int> g_phase{PHASE_IDLE};

// 枚举 -> 中文。用数组映射，不要用反斜杠转义拼接。
const char* const kPhaseNames[PHASE_COUNT] = {
    "空闲",
    "扫描设备",
    "打开音频采集",
    "建立连接",
    "菜单打开中",
    "驱动网络",
    "等采集线程退出",
};

inline long long steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// 打点：当前正在做什么。主循环每轮开头重置为"空闲"。
inline void setPhase(MainPhase p) {
    g_phase.store(static_cast<int>(p));
}

// 毫秒 -> 一位小数的秒文本（3400 -> "3.4"）。看门狗日志用。
std::string msToSecsText(long long ms) {
    const long long whole = ms / 1000;
    const long long tenth = (ms % 1000) / 100;
    return std::to_string(whole) + "." + std::to_string(tenth);
}

// 看门狗线程的主体。
//
// 持续记录的逻辑：
//   - 首次发现超时（>2 秒没心跳）立刻记一条，之后每 1 秒再记一条，
//     每条都带上"已经卡了多少秒"，所以最后一条就是卡住的总时长。
//   - 主线程恢复时补一条收尾，写明总共卡了多久。
//   - 没卡住时什么都不写 —— 空闲时不刷屏。
void watchdogLoop() {
    // 800ms 就报。定 2000 会漏掉最常见的卡顿 —— 同步扫网段大约 1~2.5 秒，
    // 刚好卡在 2 秒阈值下面，等于白装。用户点托盘时的「卡一下」就是它。
    constexpr long long kStallMs  = 800;
    constexpr long long kReportMs = 1000;   // 卡住期间每 1 秒记一条

    bool stalled = false;
    long long lastReported = 0;
    long long stalledSince = 0;

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (g_stop.load()) break;

        // 正在等用户回答弹窗：主循环停着是正常的，不算「卡住」。
        //
        // 但**音频还指着心跳定时器活**：弹窗的模态消息循环会把 WM_TIMER
        // 派发到托盘窗口，心跳里调 pumpNonBlocking() 推 RTP。
        // 心跳一停，用户听到的就是「点一下菜单里的东西，声音没了」——
        // 所以这里不能无条件放过，得单独盯心跳还在不在跳。
        // （已实测：MessageBox 模态期间 50ms 定时器照常触发，2.5 秒跳了 40 次。）
        if (g_inUserPrompt.load()) {
            const long long now2 = steadyNowMs();
            const long long tick2 = g_mainTick.load();
            if (tick2 != 0 && now2 - tick2 > kStallMs) {
                if (now2 - lastReported >= kReportMs || lastReported == 0) {
                    lastReported = now2;
                    logLine("弹窗期间音频心跳停了 " + msToSecsText(now2 - tick2) +
                            " 秒 —— 声音会断，检查菜单心跳定时器是否被提前 KillTimer");
                }
            } else {
                stalled = false;
            }
            continue;
        }

        const long long now = steadyNowMs();
        const long long tick = g_mainTick.load();
        // 主线程还没跑起来（启动阶段）时不做判断
        if (tick == 0) continue;

        const long long lag = now - tick;
        if (lag > kStallMs) {
            if (!stalled) {
                stalled = true;
                stalledSince = tick;
                lastReported = 0;
            }
            // 首次立刻记；之后每 1 秒一条，不刷屏
            if (now - lastReported >= kReportMs || lastReported == 0) {
                lastReported = now;
                const int p = g_phase.load();
                const char* name = (p >= 0 && p < PHASE_COUNT) ? kPhaseNames[p] : "未知";
                logLine("主线程无响应 " + msToSecsText(now - stalledSince) +
                        " 秒，最后阶段: " + name);
            }
        } else if (stalled) {
            // 恢复正常：收尾一条，写明总共卡了多久
            stalled = false;
            const int p = g_phase.load();
            const char* name = (p >= 0 && p < PHASE_COUNT) ? kPhaseNames[p] : "未知";
            logLine("主线程恢复（共卡住 " + msToSecsText(now - stalledSince) +
                    " 秒，最后阶段: " + name + "）");
        }
    }
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

// 读「开机自启」注册表项，返回它里面记的 exe 路径。空 = 没有这一项。
//
// 值的格式是 "<exe 全路径>" --last —— 引号里是路径，后面跟着启动参数。
std::string autoStartExePath() {
    HKEY key = nullptr;
    const char* runKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, runKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return std::string();

    char buf[2048] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    const char* valueName = "AirPlayBridge";
    const LONG rc = RegQueryValueExA(key, valueName, nullptr, &type,
                                     reinterpret_cast<BYTE*>(buf), &sz);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS) return std::string();

    const std::string v(buf);
    if (!v.empty() && v[0] == static_cast<char>(34)) {   // 开头的引号
        const size_t end = v.find(static_cast<char>(34), 1);
        if (end == std::string::npos) return std::string();
        return v.substr(1, end - 1);
    }
    const size_t sp = v.find(static_cast<char>(32));      // 无引号：取到第一个空格
    return (sp == std::string::npos) ? v : v.substr(0, sp);
}

// 自启是否指向**当前这个 exe**。
//
// 只看「注册表里有没有这一项」是不够的 —— 自启项存的是 exe 的完整路径。
// 用户换了目录、改了文件名、或者删掉旧文件之后，键还在，但它指向的是那个
// 已经不存在的老文件：开机时 Windows 找不到它，会**静默跳过**；
// 而菜单里「开机自启」照样打着勾，用户以为开着、实际从来没启动过。
//
// 所以必须同时比对路径：只有指向当前 exe 才算真的开着。
bool isAutoStartEnabled() {
    const std::string registered = autoStartExePath();
    if (registered.empty()) return false;
    const std::string cur = exePath();
    if (cur.empty()) return false;
    return _stricmp(registered.c_str(), cur.c_str()) == 0;
}

// 系统是否在「任务管理器 → 启动应用」里单独禁用过本项。
//
// Windows 把这类用户/系统决策记在 StartupApproved 里，是个二进制值：
// 第一个字节偶数（02/06）表示禁用，奇数（03/07）表示启用；
// 值不存在说明没被单独动过，按 Run 键本身生效。
//
// 这里只在需要时调用（日志诊断），取不到一律当作"没被禁用"——
// 宁可漏报也不误报。
bool autoStartDisabledBySystem() {
    HKEY key = nullptr;
    const char* k = "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
    if (RegOpenKeyExA(HKEY_CURRENT_USER, k, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    BYTE buf[64] = {};
    DWORD sz = sizeof(buf);
    DWORD type = 0;
    const char* valueName = "AirPlayBridge";
    const LONG rc = RegQueryValueExA(key, valueName, nullptr, &type, buf, &sz);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || sz == 0) return false;
    return (buf[0] % 2) == 0;
}

// 把开机自启的实际情况记进日志。
//
// 用户报「开机没有自启」时，光看代码猜不出是哪种 ——
// 可能是从没开过、可能是路径失效、也可能是被系统在「启动应用」里禁用了。
// 启动时记一笔，三种情况一眼可分。
// 首次运行时问一次「要不要开机自启」。
//
// 只在这个时机问：用户刚装好、第一次手动打开。开机自启启动的那一份
// （带 --last）绝不会走到这里 —— 每次开机都被问一遍是骚扰。
//
// 用系统的 MessageBox 而不是自绘窗口：这里只要一个二选一，
// 没必要再写一个窗口，也不会显得像来路不明的弹窗。
// 首次运行时问一次界面语言。只问一次，之后在「更多设置 › 语言」里改。
//
// 这个弹窗**不能用 tr()** —— 问的时候语言还没定。
// 所以正文里中英并列，用户自己挑。系统按钮是「是/否」，
// 正文里写清哪个对应哪个，不依赖按钮文字。
void askLanguageOnFirstRun(BridgeConfig& cfg, const std::string& cfgPath) {
    std::string msg;
    msg += "Choose the interface language.";
    msg += char(10);
    msg += "选择界面语言。";
    msg += char(10); msg += char(10);
    msg += "Yes / 是  ->  中文";
    msg += char(10);
    msg += "No  / 否  ->  English";
    msg += char(10); msg += char(10);
    msg += "You can change it later under \"More settings > Language\".";
    msg += char(10);
    msg += "之后可以在托盘菜单的「更多设置 › 语言」里随时更改。";

    const int need = MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, nullptr, 0);
    std::wstring w(need > 1 ? static_cast<size_t>(need) : 0, 0);
    if (need > 1)
        MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, w.data(), need);

    const int r = promptUser(w.c_str(), L"AirPlay Bridge",
                             MB_YESNO | MB_SETFOREGROUND);
    cfg.language = (r == IDYES) ? "zh" : "en";
    logLine("首次运行：界面语言选择为 " + cfg.language);
    if (!cfgPath.empty()) saveConfig(cfgPath, cfg);
}

void askAutoStartOnFirstRun() {
    const wchar_t kNLw = static_cast<wchar_t>(10);
    std::wstring tip;
    tip += i18n::trw("要不要让 AirPlay Bridge 开机时自动启动？");
    tip += kNLw; tip += kNLw;
    tip += i18n::trw("选「是」之后，开机会自动运行并连上上次用过的音箱。");
    tip += kNLw;
    tip += i18n::trw("以后随时可以在托盘菜单里改。");

    const int r = promptUser(tip.c_str(), L"AirPlay Bridge",
                              MB_YESNO | MB_SETFOREGROUND);
    if (r == IDYES) {
        setAutoStart(true);
        logLine("首次运行：用户选择开机自启");
    } else {
        logLine("首次运行：用户暂不开启开机自启");
    }
}

void logAutoStartStatus() {
    const std::string registered = autoStartExePath();
    if (registered.empty()) {
        logLine("开机自启：未开启");
        return;
    }
    const std::string cur = exePath();
    const bool sameExe = !cur.empty() && _stricmp(registered.c_str(), cur.c_str()) == 0;
    const bool fileExists = GetFileAttributesA(registered.c_str()) != INVALID_FILE_ATTRIBUTES;

    if (!fileExists) {
        logLine("开机自启：指向的文件已不存在（换位置或改名留下的旧记录）");
    } else if (!sameExe) {
        logLine("开机自启：指向的是另一个程序，本程序没有自启");
    } else if (autoStartDisabledBySystem()) {
        logLine("开机自启：已在系统的「启动应用」里被禁用");
    } else {
        logLine("开机自启：已开启");
    }
}

// 启动时修一次已经失效的自启项。
//
// 只在「有这一项 且 它指向的文件已经不存在」时动手 —— 那明确是换位置留下的
// 死路径。用户当初既然开了自启，把路径更新到当前这个 exe 正是他的意图，
// 所以这里直接改，不弹窗打扰（只写一行日志）。
//
// 其它情况一律不碰：
//   - 没有这一项               -> 用户没开过（或主动关了），不该替他打开
//   - 指向的是另一个**存在**的文件 -> 那是另一个版本在用，别去抢
void fixAutoStartIfStale() {
    const std::string registered = autoStartExePath();
    if (registered.empty()) return;                       // 没开过自启

    if (GetFileAttributesA(registered.c_str()) != INVALID_FILE_ATTRIBUTES)
        return;                                           // 那个文件还在，别动

    const std::string cur = exePath();
    if (cur.empty()) return;
    if (_stricmp(registered.c_str(), cur.c_str()) == 0) return;

    logLine("开机自启原本指向一个已不存在的文件，已更新为当前程序");
    setAutoStart(true);
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

    // ---- 无声音时待机 ----
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
    // 待机期间的轻量采集：只用来唤醒，不推数据。
    std::unique_ptr<WasapiCapture> standbyEar;
    std::thread                    standbyThread;
    // sender->start() 是否**同步**失败过（RAOP 的 UDP 绑定失败会同步调 launched(false)）。
    // 见 start() 末尾的说明。
    std::atomic<bool>              launchSyncFailed{false};
    // 采集启动失败是否已经处理过（避免每轮重复触发重连）。
    bool                           captureFailHandled = false;

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

        // 要开始推流了：提高时钟精度，让主循环跑在设计节奏上。
        setHighResTimer(true);

        // wantReconnect 也必须一起清掉。
        //
        // 它是「掉线需要重连」的标志，但 start() 并不经过 teardown()，
        // 所以上一次失败留下的 true 会原样带进这个新会话。
        // 后果：退避循环每 5 秒看到「running && wantReconnect」，
        // 就把这个刚建好的健康会话整个拆掉再重连 —— 永远连不上，
        // 而且表现是「4 秒后会话 stopped、没有任何超时警告」（因为是我们自己拆的）。
        // 重启接收端也没用：毒在我们自己的标志位里。
        wantReconnect.store(false);
        // 先清干净：重复调用或上次失败残留时，producer 可能是个
        // joinable 的空线程，留着会在成员析构时 std::terminate。
        //
        // 但光 join 不够 —— 采集线程只有在 capture->stop() 之后才会退出，
        // 而 capture->stop() 只有 teardown() 会调。掉线那条路不经过 teardown，
        // 于是这里的 join 会等一个永远不会退出的线程，主线程就此死透
        // （表现为托盘菜单没反应 + 状态永远停在"连接中"）。
        // 所以：先停，再 join。
        if (capture) capture->stop();
        if (sender)  sender->stop();
        setPhase(PHASE_CAPTURE_STOP);
        if (producer.joinable()) producer.join();
        producer = std::thread();

        // 音频采集。默认采**系统默认输出设备**这一路 ——
        //（WasapiCaptureConfig 的 mode 是 DefaultOnly；多设备混音的实现
        //  另有未解决的问题，没有启用。）
        // 这样即使某个应用（比如浏览器）被单独指到了别的声卡，
        // 它的声音也能抓到，用户不用去音量合成器里改输出。
        // ★ 接管必须发生在**打开采集之前**。
        //
        // 采集的 deviceId 留空时会去取「此刻的默认输出设备」并绑死在那上面
        // （见 wasapi_capture.cpp 的 open()）。而接管（把默认设备切到 CABLE）
        // 原本是在握手成功后的 onConnected 回调里做的 —— 那时采集早已绑在
        // 原来的扬声器上，不会跟着切。
        //
        // 后果是一条自相矛盾的链：
        //   声音送进 CABLE → 采集还在听扬声器（那儿是静的）→ peak 恒为 0
        //   → 被判静音 → 5 秒待机 → 断开 → 恢复默认设备 → 采集又有声音 → 唤醒
        //   → 每 6 秒循环一次。用户看到的是「卡顿、断开、声音在两边抢」。
        //
        // 所以把接管提到这里：先切默认设备，再打开采集 —— 采集自然落到 CABLE 上。
        // 握手失败时会走 stop()/teardown()，那里本来就有「savedRenderDevice
        // 非空就切回去」的兜底，所以提前接管不会把用户的设备留在 CABLE 上。
        maybeTakeOverRenderDevice();

        {
            WasapiCaptureConfig wcfg;
            // 用户指定了采集端点就用它（空 = 跟随系统默认输出设备）。
            // 配置里存的是 UTF-8，COM 需要宽字符。
            // 接管中一律跟随默认设备（见 g_takeoverActive 的说明）。
            // 先取一份带锁的副本：不能直接 .c_str() —— 主线程可能同时改它。
            const std::string wantDev = getCaptureDeviceId();
            if (!g_takeoverActive.load() && !wantDev.empty()) {
                const int need = MultiByteToWideChar(
                    CP_UTF8, 0, wantDev.c_str(), -1, nullptr, 0);
                if (need > 1) {
                    wcfg.deviceId.resize(static_cast<size_t>(need));
                    MultiByteToWideChar(CP_UTF8, 0, wantDev.c_str(),
                                        -1, wcfg.deviceId.data(), need);
                }
            }
            capture = std::make_unique<WasapiCapture>(wcfg);
        }
        setPhase(PHASE_CAPTURE_OPEN);   // 打点：开采集设备可能耗时
        if (!capture->open(&err)) {
            capture.reset();   // 失败路径不留半成品
            // ★ 必须撤销接管 —— 默认设备已经切到 CABLE 了，不撤销就等于
            //   把用户的声音送进一个没人采集的黑洞（P1-3）。
            rollbackTakeover();
            setHighResTimer(false);   // 同样别留着一个不断电的高精度定时器
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
            // 某些分支想给状态文案加点补充（比如"无虚拟声卡"）。
            // 攒在这里、最后统一发，避免被后面那句通用文案覆盖掉。
            std::string extraStatus;
            // 连上了：记住当前默认设备，然后切到虚拟声卡。
            // 这样系统声音不再走向物理音箱，而是进了虚拟声卡；
            // 我们从虚拟声卡采集推给 HomePod —— 电脑自己静悄悄。
            // 连上了：确认已经接管（把默认设备切到虚拟声卡）。
            // 真正的切换在 start() 里、打开采集之前就做了 —— 见那边的注释，
            // 这里只是兜底调用：如果 start 时没切成（比如当时没装虚拟声卡），
            // 现在再试一次。函数本身幂等，切过就直接返回。
            if (ok) {
                maybeTakeOverRenderDevice();
                if (g_takeoverActive.load()) {
                    // 已接管：声音进了虚拟声卡，由我们采集后推给 HomePod，
                    // 电脑自己不出声。
                } else {
                    // 切不过去（没装虚拟声卡、或名字不叫 CABLE Input）。
                    // 这时是「监听复制」模式：系统声音仍然送往默认输出设备，
                    // 程序只是复制一份推给接收端。
                    //
                    // 文案别写死成「两边同时出声」—— 电脑可能根本没接音箱，
                    // 那样说会让人以为程序描述错了。准确的说法是
                    // 「电脑的输出设备也会播放」：这是信号层面的事实；
                    // 至于用户听不听得见，取决于他接没接音箱。
                    logLine("未找到虚拟声卡：无法隔离系统声音，它仍会送往电脑的输出设备");
                    // 不在这里直接发 onStatus —— 下面还会用「已连接」覆盖一次，
                    // 用户就看不到这条提示了。攒到 extraStatus 里统一发。
                    extraStatus = "已连接（未装虚拟声卡：电脑的输出设备也会播放）";
                }
            }

            connected.store(ok);
            // 握手成功就复位防火墙提示的整套状态：
            // 计数清零、提示解禁 —— 下次真出问题时还能再提示一次。
            if (ok) {
                g_handshakeFailStreak.store(0);
                g_firewallHintShown.store(false);
            } else {
                // 连续失败够多次就写一条可执行的诊断（只写一次）。
                // 见 maybeLogFirewallHint 上方关于入站 UDP 的说明。
                maybeLogFirewallHint();
            }
            // 刚连上：把"最近有声"的时间戳重置成现在。
            // 不重置的话 lastLoudMs 还是 0，主循环一看"距今一亿秒没声音"
            // 就立刻把刚建立的会话拆了 —— 表现为连上即待机。
            if (ok) {
                lastLoudMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
            }
            if (onConnected) onConnected(ok);
            if (onStatus) {
                // ★ 送出去的是**翻译后**的文案（tooltip / 菜单要跟着界面语言走）；
                //   日志仍写中文 —— 那是排查用的，不跟界面语言变（既有设计）。
                if (ok && !extraStatus.empty()) onStatus(i18n::tr(extraStatus.c_str()));
                else onStatus(ok ? i18n::tr("已连接")
                                 : (i18n::tr("连接失败: ") + e));
            }
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
                // ★ 标记「这次失败是 sender->start() 同步触发的」。
                //   launchSyncFailed 在 sender->start() 之前清零、之后检查 ——
                //   否则下面那句无条件 running=true 会把这里刚清掉的状态冲回来，
                //   主循环就把一个 Idle 状态的会话当成在跑（P2-2）。
                launchSyncFailed.store(true);
                wantReconnect.store(true);
            }
        };
        ev.closed = [this]() {
            connected.store(false);
            if (onConnected) onConnected(false);
            if (onStatus) onStatus(i18n::tr("连接断开"));
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
        // 音频链路一句话记清楚 —— 排查「杂音/爆音」时，
        // 第一件要确认的就是「采集多少 Hz、有没有转、引擎收到多少 Hz」。
        logLine("音频链路: 采集 " + std::to_string(inputRate) + " Hz -> 引擎 " +
                std::to_string(engineInputRate) + " Hz" +
                (needResample ? (std::string(i18n::tr("（本机重采样，品质 ")) +
                                  qualityLevelName(cfg ? cfg->qualityLevel : 0) +
                                  i18n::tr("）"))
                              : std::string(i18n::tr("（直通）"))));

        sender->setInputFormat(engineInputRate);
        sender->setIdentity({"AirPlay Bridge", g_deviceId, "MacBookPro18,3"});
        sender->setAuth(RaopDeviceInfo::Auth::HapTransient, true, "", "", "");
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
                // 峰值触顶 = 波形可能已经被削平（系统混音过载）。
                // 这跟静音判定无关，单独计数，主循环汇总。
                // 丢帧只在「已连上、真的在推流」时才算 ——
                // 没连上时采集照跑但没人消费，丢弃是正常的，
                // 算进来会刷出「每 5 秒丢 4 秒音频」这种毫无意义的警报。
                const bool pushing = connected.load();

                if (peak >= 32700) g_clippedSamples.fetch_add(1);
                if (peak > g_peakWindowMax.load()) g_peakWindowMax.store(peak);

                if (peak >= kSilencePeak) {
                    const long long tLoud = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
                    lastLoudMs.store(tLoud);
                }
                if (!needResample) {
                    // 设备本来就是 44.1k，直接送。
                    // ★ 只在已连接时才推 —— 见下方 resampler 分支的说明。
                    if (pushing) {
                        // ★ 计数器是**帧数**，而 ring 里存的是**样本**（n*2）。
                        //   原来这里加的是 n*2（样本数），立体声下日志里
                        //   「音频丢帧：累计 N 帧」恒为真实值的 2 倍 ——
                        //   排查爆音时会被这个虚报的数字带偏（P2-8）。
                        if (!ring->tryPush(std::span<const int16_t>(d, n * 2)))
                            g_ringDroppedFrames.fetch_add(n);
                    }
                    return;
                }
                // 转到 44.1k 再送。resampleBuf 复用，避免每回调都分配。
                resampleBuf.clear();
                resampler.process(d, n, resampleBuf);
                // ★ 只在已连接时才推。
                //
                // 连接建立要几秒（发现 + 握手 + SETUP），期间采集照跑。
                // 原来无论连没连都往 ring 灌 —— 等发送启动时 ring 里已经积压了
                // 近满仓的音频（实测 8 秒内灌到 99%，掉 3528 帧）。
                //
                // 而 pacer 按「自 clockStart_ 以来的墙钟」发，只会补时间轴上的量，
                // 不会去排空已积压的部分 —— 于是那批积压永久留在 ring 里，
                // 水位稳定在 87%（余量仅 48ms）。之后任何一次超过 48ms 的发送停顿
                // （实测尖峰 56~89ms）都会再次溢出，听感就是「偶尔爆音」。
                //
                // 连上之前的音频本来就不该进缓冲：用户没在听，接收端也来不及播。
                // 断开时 teardown() 会 ring.reset()，所以重连一律从空 ring 开始。
                if (pushing && !resampleBuf.empty()) {
                    if (!ring->tryPush(std::span<const int16_t>(resampleBuf.data(),
                                                       resampleBuf.size()))) {
                        // resampleBuf 是交错样本，除以声道数才是帧（同 ①）。
                        g_ringDroppedFrames.fetch_add(resampleBuf.size() / 2);
                    }
                }
            });
        });

        setPhase(PHASE_CONNECT);   // 打点：握手全在这一步里跑
        // ★ 清零必须在 sender->start() **之前** —— 它可能同步触发 launched(false)
        //   并把标志置 true（P2-2）。紧跟其后检查，再决定要不要置 running。
        launchSyncFailed.store(false);
        captureFailHandled = false;   // 新会话重新观察采集状态
        sender->start(host, 7000, "AirPlay Bridge");

        // ★ 音量必须在 start() **之后**设。
        //
        // start() 内部会 `pendingVolumeDb_ = kNoVolume`（-1000，注释写着
        //「never carry volume between devices」）—— 它把会话开始前的音量设置
        // 当成上一台设备的残留清掉。所以 start() 之前调 setVolume 会被无声覆盖，
        // 之后 startStreaming_ 判定「用户没设过音量」→ 推 0 dB = 完全不衰减，
        // 也就是每次启动都是最大音量。
        // 实测：配置里写 35%，发出的却是 volume: 0.000000。
        //
        // 这时调是安全的：会话还没进入 Streaming，setVolume 只把它存进
        // pending，等 startStreaming_ 一并推给接收端。
        // ★ 取 cfg 的实时值，不要用 sess.volume 那个缓存。
        //
        // sess.volume 只在启动时被赋一次值，之后用户在菜单里改音量只更新了
        // cfg.volume（并直接 setVolume 到当前会话），**没有回写 sess.volume**。
        // 于是一旦重连（改延迟 / 掉线 / 待机唤醒 / 手动断开再连），
        // 这里就会用回启动时的旧值 —— 用户感受是「调完延迟，音量被重置了」。
        //
        // cfg 是指向 main 里那个 BridgeConfig 的指针，任何时刻读都是最新值。

        sender->setVolume(cfg->volume);

        // 注意：sender->start() 是异步的 —— 它发完握手请求就返回，
        // 真正的结果要等后续 pump() 里才出来（launched 回调）。
        // 所以这里不能按 connected 判断成败，只能先置 running，
        // 让主循环跑起来；失败由 launched(false) 回调负责清掉。
        if (launchSyncFailed.load()) {
            // sender->start() 同步失败（launched(false) 已经把 running 清了、
            // 把重连标志设了）。这里**不能再置 running=true** ——
            // 那会让主循环把一个 Idle 会话当成在跑，界面显示「已连接」
            // 但实际没有音频（2026-10-18 第四轮外部审查 P2-2）。
            return false;
        }
        running.store(true);
        return true;
    }

    // 主循环的一步
    void pump() {
        if (!sender) return;
        // ★ 采集健康检查：WASAPI 的 Start() 失败时采集线程会直接退出，
        //   而 running 仍为 true —— 界面显示「已连接」、实际一个采样都没收到，
        //   用户只会觉得「连上了但没声音」，也没有任何提示（P2-1）。
        //   这里发现失败就让会话「正常地」断开，交给既有的重连/重枚举去处理。
        //   只做一次（cleared 标志），避免每轮重复触发。
        if (capture && !captureFailHandled && capture->startFailures() > 0) {
            captureFailHandled = true;
            logLine("采集启动失败（" + std::to_string(capture->startFailures()) +
                    " 路设备），断开并等待重连");
            running.store(false);
            wantReconnect.store(true);
            if (onStatus) onStatus(i18n::tr("采集启动失败"));
            return;
        }
        // 等待上限压到 10ms（默认 50ms）。
        //
        // 主循环一轮 = pump + 20ms sleep，而托盘每轮只处理一条消息。
        // 流媒体正常时 sender 的 deadline 约 8ms，pump 很快返回，改不改都一样；
        // 但连接不顺畅时 sender 空转、没有近 deadline，就会撑满 50ms，
        // 一轮变成 70ms —— 点托盘会觉得钝。
        setPhase(PHASE_NET_PUMP);
        loop->pump(*sender, std::chrono::milliseconds(10));
    }

    // 非阻塞版本，给「菜单打开期间的心跳」用。
    //
    // 默认的 pump() 最多会阻塞 50ms（等 socket 或下一个 deadline）。
    // 菜单的模态循环里按 15ms 的节奏调它，等于把这个循环永久占满 ——
    // 菜单再也处理不了点击，TrackPopupMenu 不返回，主线程就卡死了。
    // 流媒体正常时 deadline 很近、pump 秒回，所以平时看不出来；
    // 一旦连接失败（sender 空转没有近 deadline），每次就阻塞满 50ms。
    void pumpNonBlocking() {
        if (!sender) return;
        setPhase(PHASE_NET_PUMP);
        loop->pump(*sender, std::chrono::milliseconds(0));
    }

    // 把系统默认播放设备切到虚拟声卡（接管），让声音进 CABLE、由我们采集。
    //
    // **幂等**：已经切过（savedRenderDevice 非空）就直接返回。
    // 两处调用：Session::start() 里（打开采集**之前**）和 ev.launched 里（兜底）。
    //
    // 为什么必须在打开采集之前切：采集的 deviceId 留空时会取「此刻的默认设备」
    // 并绑死上去，之后系统默认设备再变它也不跟。原来接管发生在握手成功之后，
    // 那时采集已经绑在物理扬声器上 —— 声音进了 CABLE，采集却在听一个安静的口，
    // 结果是「采不到声音 → 判静音 → 待机 → 断开 → 恢复设备 → 又有声音」。
    void maybeTakeOverRenderDevice() {
        if (!savedRenderDevice.empty()) return;          // 已经接管过
        if (g_takeoverActive.load()) return;
        const std::string cur = getDefaultRenderDeviceName();
        if (cur.empty()) return;
        // 只在自己不是虚拟声卡时切换，避免来回切死锁
        if (cur.find("CABLE") != std::string::npos) return;

        if (!setDefaultRenderDevice("CABLE Input")) {
            logLine("未找到虚拟声卡：无法隔离系统声音，它仍会送往电脑的输出设备");
            return;
        }
        savedRenderDevice = cur;
        g_takeoverActive.store(true);   // 见 g_takeoverActive 的说明
        // 落盘记录，这样即使我们崩了，下次启动也能恢复
        if (cfg && !cfgPath.empty()) {
            BridgeConfig tmp;
            loadConfig(cfgPath, tmp);
            tmp.preCaptureDevice = cur;
            saveConfig(cfgPath, tmp);
        }
        logLine("已接管：默认播放设备切到 CABLE Input（原为 " + cur + "）");
    }
    // 只负责「把接管撤销掉」，不碰采集/发送线程。
    //
    // 为什么需要单独一个：start() 里 maybeTakeOverRenderDevice() 已经把默认设备
    // 切到 CABLE 了，之后若采集打不开就直接 return —— 那时 stop() 没被调用，
    // 用户的系统声音会继续送往一个**没人采集**的端点，表现是「没声音了」。
    // （2026-10-18 第四轮外部审查 P1-3）
    //
    // 这里比 stop() 里那段更保守：只有恢复**成功**才清标记 ——
    // 与 DECISIONS 的「恢复失败要保留重试依据」一致（P2-3 同类）。
    void rollbackTakeover() {
        if (savedRenderDevice.empty()) return;
        if (setDefaultRenderDevice(savedRenderDevice)) {
            savedRenderDevice.clear();
            g_takeoverActive.store(false);
            if (!cfgPath.empty()) {
                BridgeConfig tmp;
                loadConfig(cfgPath, tmp);
                tmp.preCaptureDevice.clear();
                saveConfig(cfgPath, tmp);
            }
        } else {
            // 恢复失败：保留 savedRenderDevice 和落盘标记，下次启动还能重试。
            logLine("回滚接管失败：默认设备没能切回原设备，保留恢复标记供下次重试");
        }
    }

    void stop() {
        running.store(false);
        // 不再推流了 —— 立刻恢复系统时钟精度（不播放时不占任何时钟开销）。
        setHighResTimer(false);
        // 切回原来的默认设备
        // ★ 只有**恢复成功**才清标记 —— 这是 D4 的硬约束（docs/DECISIONS.md）：
        //   「失败必须保留标记，否则用户会永久失去声音且程序不再尝试恢复」。
        //   原来这里不看返回值直接清空，恢复失败就等于把重试依据也丢了
        //   （2026-10-18 第四轮外部审查 P2-3）。
        rollbackTakeover();
        if (capture) capture->stop();
        if (sender)  sender->stop();
        if (producer.joinable()) producer.join();

        // 状态改了就必须通知托盘。
        //
        // 图标和 tooltip 是**事件驱动**的（只在被通知时才更新），
        // 而托盘菜单是每次打开时**实时读** connected 的。
        // 这里只 store 不通知，就会出现自相矛盾的状态：
        // 图标/tooltip 说"已连接"、点开菜单却是"未连接"。
        //
        // 走到这里最常见的情况是重连：teardown() -> stop() 收尾之后
        // 才去连下一个，中间这段时间托盘会一直挂着上一次的状态。
        if (connected.load()) {
            connected.store(false);
            if (onConnected) onConnected(false);
        }
    }

    void teardown() {
        stop();
        // ★ 必须清待机状态 —— 否则 standbyThread 仍是 joinable，
        //   Session 析构时 C++ 运行库会**确定性地**调 std::terminate。
        //   触发场景：开启「无声音时待机」→ 静音进入待机 → 从托盘退出 /
        //   注销 / 关机。进程以 0xC0000409 异常终止，日志落盘、崩溃标记
        //   清理都可能中断。（2026-10-18 第四轮外部审查 P1-2，已实测复现）
        //   clearStandbyState() 本身可重复调用（先判 joinable 再 join）。
        clearStandbyState();
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

// 设备缓存的最长复用年龄。
//
// discoverAirPlayDevices(2500) 是同步的 —— 它在主线程上把网段扫满 2.5 秒。
// 每次重连都重扫一遍，托盘就僵 2.5 秒，正是"连接过程中菜单卡死"的来源。
// 局域网的设备列表不会几秒钟就变，30 秒内直接复用完全够用。
constexpr long long kDeviceCacheMaxAgeMs = 30000;

// 扫描 + 选中一个设备（托盘模式：自动选 HomePod；多个则用弹出菜单）
//
// lastScanMs = 上次真扫的时间戳（steady_clock 毫秒，0 = 从没扫过）。
// forceScan = true 时无视缓存强制重扫（用户点"重新扫描"走这条路）。
//
// 缓存判定分三步：
//   1. 缓存里一个设备都没有 -> 必须真扫。
//      不能把"空结果"当有效缓存反复用：那会让扫描失败（设备还没通电、
//      网卡没起来）永远卡在空列表里，之后再也不会重扫。
//   2. forceScan -> 真扫（用户明确要求刷新）。
//   3. 缓存非空、且年龄 < 30 秒 -> 直接复用，返回 true，不碰网络。
//   否则（过期）-> 真扫。
bool resolveDevice(Session& sess, const BridgeConfig& cfg,
                   std::vector<AirPlayDevice>& discovered,
                   long long& lastScanMs, bool forceScan = false) {
    const long long nowMs = steadyNowMs();
    const bool cacheFresh = !discovered.empty() && lastScanMs > 0 &&
                            (nowMs - lastScanMs) < kDeviceCacheMaxAgeMs;
    if (cacheFresh && !forceScan) {
        // 复用缓存：跳过那 2.5 秒的同步扫描
    } else {
        setPhase(PHASE_SCAN);   // 打点：这里是唯一会同步扫网段的地方
        {
            std::vector<AirPlayDevice> all = discoverAirPlayDevices(2500);
            {
                std::lock_guard<std::mutex> lk(g_lastScanAllMutex);
                g_lastScanAll = all;
            }
            discovered = filterDevices(all, cfg);
        }
        lastScanMs = steadyNowMs();
    }
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


    // WIN32 子系统默认没有控制台。命令行模式要把输出送到终端，
    // 得先 AttachConsole 接上父进程的控制台，再重绑 stdout/stderr
    // （直接 printf 没地方去 —— 所以这里是必须的）。
    //
    // ★ AllocConsole 只在 --console 时用。
    //
    // 它给进程**新建**一个控制台窗口。而绝大多数带参数启动的场景是
    // **无人值守**的，没有父终端，AttachConsole 必然失败：
    //
    //   开机自启   AirPlayBridge.exe --last
    //   安装程序   AirPlayBridge.exe --fix-firewall --quiet
    //   卸载清理   AirPlayBridge.exe --uninstall-cleanup
    //
    // 这些场景下 AllocConsole 会**凭空弹出一个黑色控制台窗口**，
    // 而且它不会自己消失 —— 用户把它关掉，等于关掉程序本身
    // （那个窗口就是进程的控制台，关窗即结束进程）。
    //
    // 所以：有终端就复用，没有就算了 —— 输出走日志文件
    // （见 audioOut()，C8 要求诊断类输出必须落盘）。
    bool wantsConsoleWindow = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--console") == 0) { wantsConsoleWindow = true; break; }
    }

    bool hasConsole = false;
    if (argc > 1) {
        hasConsole = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
        if (!hasConsole && wantsConsoleWindow) hasConsole = AllocConsole() != 0;
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
    bool wantFixFirewall = false;       // --fix-firewall
    bool wantCleanFirewall = false;     // --clean-firewall-rules
    bool wantUninstallCleanup = false;  // --uninstall-cleanup
    bool wantQuiet = false;             // --quiet（配合 --fix-firewall 给安装程序用）
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
        else if (a == "--fix-firewall") { wantFixFirewall = true; wantTray = false; }
        else if (a == "--clean-firewall-rules") { wantCleanFirewall = true; wantTray = false; }
        else if (a == "--uninstall-cleanup") { wantUninstallCleanup = true; wantTray = false; }
        else if (a == "--quiet")     wantQuiet = true;

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

    // ---- 防火墙修复（提权拉起的那一份，见托盘菜单「更多设置 › 连接诊断」）----
    // 放在最前面：这一份只做一件事 —— 加规则、弹提示、退出。
    // 不读配置、不起托盘图标、不跑主循环。走 COM 的函数自己管
    // CoInitialize，所以这里先放掉 main 开头那次初始化。
    if (wantFixFirewall) {
        CoUninitialize();
        return runFixFirewall(wantQuiet);
    }

    // ---- 清理陈旧放行规则（提权拉起的那一份）----
    // 与 --fix-firewall 同一待遇：一次性操作，做完即退，
    // 不起托盘、不读配置、不跑主循环。
    if (wantCleanFirewall) {
        CoUninitialize();
        return runCleanFirewallRules();
    }

    // ---- 卸载前清理（安装程序的 [UninstallRun] 调用）----
    // 与上面两个同待遇：一次性操作、豁免单实例检测。
    if (wantUninstallCleanup) {
        CoUninitialize();
        return runUninstallCleanup() ? 0 : 1;
    }

    // ---- 单实例检测（命名互斥体，见 anotherInstanceIsRunning）----
    //
    // 顺序上必须在 --fix-firewall / --clean-firewall-rules 之后，
    // 且只有它们被放行：
    // --fix-firewall 是一次性操作（加完规则就退出，不是"运行本软件"），
    // 而且用户恰恰在"已经有一个实例在跑"时最可能需要它 ——
    // 拿单实例闸去挡它，用户会在程序已经占着的情况下无法修复连接，
    // 只能先杀进程，那就本末倒置了。
    //
    // 其余所有路径（托盘模式、--console、--last、直连 IP 等）都会真正
    // 起引擎、真正去连接收端，所以一律受限制。
    //
    // 也放在读配置之前：被挡住的那一份不该产生任何副作用（不生成
    // deviceId、不写配置、不碰日志以外的状态）。
    // ★ 尽早把界面语言定下来。
    //
    // 下面几处（单实例提示、--fix-firewall 的结果弹窗）都走 i18n::trw()，
    // 而正常的「读配置 → setLang」在很后面 —— 那时语言还是默认的英文，
    // 于是中文用户会看到一个英文的「已经有程序在运行了」。
    //
    // 只读一次 language 到一个临时对象：loadConfig 只读文件、不改任何
    // 全局状态、不写盘、不生成 deviceId，所以不会破坏上面那条
    // 「被挡掉的进程不产生副作用」的约束。
    {
        BridgeConfig earlyCfg;
        loadConfig(defaultConfigPath(), earlyCfg);
        i18n::setLang(i18n::fromConfigValue(earlyCfg.language));
    }

    if (anotherInstanceIsRunning()) {
        CoUninitialize();
        return 0;
    }

    // 放行规则检查：必须放在单实例检测**之后** ——
    // 要被单实例挡掉的那一份进程只会一闪而过，不该弹这个窗口。
    //
    // 也放在读配置之前：它是纯粹的"能不能联网"问题，与用户设置无关。
    // 但托盘模式以外（--console 等）同样需要，所以只排除那两个内部开关
    //（--fix-firewall / --clean-firewall-rules，它们本来就在干这件事）。
    if (wantTray) {
        // 自启项失效就顺手修正（静默，只写日志）。
        // 放在防火墙检查之前：它不弹窗，不会跟后面那个提示抢注意力。
        fixAutoStartIfStale();
        logAutoStartStatus();
        checkFirewallRuleAtStartup();
    }

    // 运行时数据在 %LOCALAPPDATA%/AirPlayBridge/
    std::string cfgPath = defaultConfigPath();
    BridgeConfig cfg;
    loadConfig(cfgPath, cfg);

    // 配置保存失败要留痕 —— saveConfig 的调用点全都忽略返回值，
    // 写盘失败（路径不可用 / 磁盘满）本来是完全静默的。
    // 只报第一条：之后每次改设置都会再失败，刷屏没有意义。
    // 注意不写完整路径（隐私：日志可能被贴进 issue）。
    //
    // ★ 必须在**第一次 saveConfig 之前**注册。下面「生成 deviceId 并落盘」
    //   就是本进程的第一次保存，而它正是「被音箱当成新设备」那条主诉的落点
    //   —— hook 晚一步，留痕就会恰好在最该起作用的那次失效。
    //   （2026-10-18 第三轮外部审查发现：原位置距此 47 行。）
    setConfigSaveFailHook([]() {
        static bool logged = false;
        if (logged) return;
        logged = true;
        logLine("配置保存失败：设置改动不会保留（后续同样失败不再重复记录）");
    });

    // 设备标识：第一次运行生成一个，之后一直复用
    if (cfg.deviceId.empty()) {
        cfg.deviceId = makeDeviceId();
        saveConfig(cfgPath, cfg);
    }
    g_deviceId = cfg.deviceId;

    // 延迟：**尊重用户存下的值，不做自动迁移。**
    //
    // 这里原来有一条「低于 250ms 一律提到 250ms 并落盘」。当时的依据是对的：
    // 250ms 是 AP2 的现实下限，低于它有欠载爆音风险（实测 120ms 明显、
    // 150ms 间歇性少量）；而 120~150 又恰好是我们自己给过的默认值，
    // 用户什么都没选就带着一个会爆音的配置 —— 那是我们的错，不是他的偏好。
    //
    // 但那个前提后来变了，两个修复把下限往下推了：
    //   v1.5.2  连接期间的音频不再灌满本地缓冲（水位 87% → 12%，余量 48ms → 323ms）
    //   v1.5.3  发包节奏回到设计值（时钟精度，主循环 30ms → 20ms）
    // 实测 150ms 长时间播放已无爆音。
    //
    // 所以改成**只记录、不改值**：用户显式选的值就是他的偏好，必须被记住。
    // 风险提示交给滑块窗口的档位评价（「可能爆音」/「接近下限」/「推荐」），
    // 而不是启动时偷偷改掉他的设置。
    if (cfg.latencyMs < 250) {
        logLine("延迟 " + std::to_string(cfg.latencyMs) +
                "ms（低于推荐的 250ms；如遇卡顿可在菜单里调高）");
    }

    // 值不在档位上时不再吸附到最近档位 —— 滑块调出来的 7.5 秒会被吸成 8 秒。
    // 菜单里这种情况由「自定义（滑块）」那一项承接勾选（见菜单构建处）。
    if (volume < 0) volume = cfg.volume;

    // 枚举可选采集端点，供托盘菜单使用。
    // 只做一次 —— 菜单树必须在弹出前建好，展开时才去枚举 COM 会卡。
    // 插拔设备后要刷新的话，重启程序即可（这类操作极罕见）。
    // 应用界面语言（默认英文）。首装引导里改过语言后会再应用一次。
    i18n::setLang(i18n::fromConfigValue(cfg.language));
    {
        // 漏翻自检：加了新界面文案却忘了补英文时，这里会报出来。
        // （查不到译文时 tr() 原样返回中文，界面不会空 ——
        //   但英文用户会看到一句中文，所以还是要知道。）
        int tr_ = 0, tot_ = 0;
        i18n::coverage(&tr_, &tot_);
        logLine("界面语言: " + std::string(i18n::toConfigValue(i18n::current())) +
                "（词条 " + std::to_string(tr_) + "/" + std::to_string(tot_) + "）");
    }

    setCaptureDeviceId(cfg.captureDeviceId);

    // 「连接时静音电脑扬声器」的崩溃恢复 ——
    // 和 preCaptureDevice 同一个模式：正常退出会清掉这个标记，
    // 启动时看到它还在，说明上次是被强杀的，端点多半还静着。
    if (cfg.localMutedByUs) {
        logLine("检测到上次异常退出时电脑扬声器仍处于静音，正在恢复");
        // ★ 只有恢复成功才清标记（D4 同一条原则，2026-10-18 审查 P2-4）。
        //   失败就留着 —— 下次启动还能再试，否则用户永久静音且无人再管。
        if (setSystemMuted(false)) {
            cfg.localMutedByUs = false;
            saveConfig(cfgPath, cfg);
        } else {
            logLine("恢复电脑扬声器失败：保留标记，下次启动重试");
        }
    }
    g_captureDevices = listRenderDevicesWithId();

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
    sess.cfgPath = cfgPath;   // 崩溃保护要用
    sess.onFirstConnect = [&tray](const std::string& dev) {
        // 首次连上弹一次。托盘图标只有 16px，颜色变化不明显，
        // 新用户需要一个明确的"连上了"信号。
        tray.notify("AirPlay Bridge",
                    dev.empty() ? std::string(i18n::tr("已连接"))
                                : (std::string(i18n::tr("已连接: ")) + dev));
    };
    sess.onStatus = [&tray, &sess](const std::string& s) {
        std::string tip = "AirPlay Bridge - " + s;
        // 握手一直连不上时，tooltip 里补一句可读的排查方向 ——
        // 用户看的是托盘，不是日志；日志里那条 netsh 命令他未必找得到。
        //
        // 只在"连接中"状态下补，且同样要连续失败够多次才出现：
        // 偶发一次超时不该让 tooltip 变长。
        //
        // 这里只改 tooltip，不弹气泡 —— 重连循环里弹气泡是骚扰。
        if (sess.running.load() &&
            g_handshakeFailStreak.load() >= kFirewallHintAfterFails) {
            tip += i18n::tr("（若持续失败请检查防火墙入站规则）");
        }
        tray.setTooltip(tip);
        logLine(s);
    };
    // 捕获 cfgPath：下面「静音扬声器」那几条要落盘（崩溃恢复用）。
    sess.onConnected = [&tray, &cfg, &cfgPath](bool ok) {
        tray.setConnected(ok);
        // tooltip 的"基础文案"跟着图标一起走 —— 这两者必须始终一致，
        // 分成两条独立的路就一定会有一条被漏掉。
        // 具体状态（失败原因、断开原因）稍后由 onStatus 覆盖上去。
        tray.setTooltip(ok ? i18n::tr("AirPlay Bridge - 已连接")
                           : i18n::tr("AirPlay Bridge - 未连接"));

        // 连接时静音电脑自己的扬声器，断开时恢复。
        //
        // 三条边界：
        //  1. 只在**没走虚拟声卡接管**时做 —— 接管中默认端点已经是虚拟设备，
        //     用户本来就听不到，静音它没有意义（而且会污染我们的判断）。
        //  2. **用户本来就静音**的话，我们什么都不做 —— 断开时也不去打开，
        //     那是他自己设的状态，不该被一个音频程序改掉。
        //  3. 恢复只看 g_mutedByUs，不受开关影响 —— 用户中途关掉开关时，
        //     已经静音的端点也必须还回去。
        if (ok) {
            if (cfg.muteLocalOnConnect && cfg.preCaptureDevice.empty() &&
                !g_mutedByUs.load()) {
                g_wasMutedBefore.store(isSystemMuted());
                if (!g_wasMutedBefore.load() && setSystemMuted(true)) {
                    g_mutedByUs.store(true);
                    // 落盘：万一进程被强杀，下次启动能据此把端点还回去。
                    cfg.localMutedByUs = true;
                    saveConfig(cfgPath, cfg);
                    logLine("已静音电脑扬声器（断开时自动恢复）");
                }
            }
        } else if (g_mutedByUs.load()) {
            // 同 ①：成功才清，失败保留标记等下次（P2-4）。
            if (setSystemMuted(false)) {
                g_mutedByUs.store(false);
                if (cfg.localMutedByUs) {
                    cfg.localMutedByUs = false;
                    saveConfig(cfgPath, cfg);
                }
            } else if (cfg.localMutedByUs) {
                logLine("断开时恢复电脑扬声器失败：保留标记，下次启动重试");
            }
            logLine("已恢复电脑扬声器的声音");
        }
    };

    // 菜单打开期间继续推音频 IO。
    // TrackPopupMenu 自带模态消息循环，主循环在里面完全停摆 ——
    // 不在这儿接上这口气，点一下托盘声音就会断。
    // 窗口被要求关闭（任务管理器 / 系统关机）时走和菜单「退出」同一条路 ——
    // 设 g_stop 让主循环收尾（拆会话、恢复默认设备、删托盘图标）。
    //
    // 以前没有这条：DefWindowProcW 收到 WM_CLOSE 直接销毁窗口，
    // 而主循环只认 g_stop，于是进程变成没有窗口的僵尸 ——
    // 清理跑过了但任务管理器里还挂着，磁盘上的「接管中」标记也清不掉。
    tray.setQuitRequest([]() { g_stop.store(true); });

    tray.setMenuPump([&]() {
        // 这个回调本身就跑在主线程上，所以顺手刷新主线程心跳 ——
        // 菜单开着等用户点是正常的，不该被看门狗当成卡死；
        // 反过来，菜单真卡住时这里就不跳了，看门狗照抓不误。
        g_mainTick.store(steadyNowMs());
        // 必须用非阻塞版本 —— 见 Session::pumpNonBlocking 的注释
        if (sess.running.load()) sess.pumpNonBlocking();
    });

    std::vector<AirPlayDevice> discovered;
    // discovered 的最后一次真扫时间（steady_clock 毫秒；0 = 从没扫过）。
    // 让重复重连不再每 5 秒同步重扫 2.5 秒 —— 见 resolveDevice。
    long long lastScanMs = 0;

    // 用「记住的设备」直接开一条会话（不扫网段）。
    //
    // 抽出这条路径是为了让两处复用同一份逻辑（这里只写一遍）：
    //   1) skipResolve —— 待机唤醒，等那 2.5 秒扫描开头的声音就丢了；
    //   2) 常规重连 —— 先直连试一次，失败才回落到扫描（见 tryConnect）。
    //
    // sess.host 为空时用 cfg.lastHost 补上（连带 deviceId / deviceName），
    // 这样即便进程刚起来 sess 还是空的也能直连。
    //
    // 返回 true 表示会话已建起来（start() 成功返回，握手结果由 pump 决定）；
    // false 表示没有可用目标或 start() 直接失败 —— 此时不弹气泡，
    // 由调用方决定后续（重连循环里反复弹气泡是骚扰）。
    auto startWithRememberedDevice = [&]() -> bool {
        if (sess.host.empty()) {
            if (cfg.lastHost.empty()) return false;   // 没记住任何设备
            sess.host = cfg.lastHost;
            sess.deviceId = cfg.lastDeviceId;
            sess.deviceName = cfg.lastDeviceName;
        }
        std::string err;
        return sess.start(err, true);
    };

    // 尝试连接（托盘模式：优先历史设备；没有历史则自动选 HomePod）
    //
    // skipResolve = true 时不扫描网段，直接用 sess 里已经记住的
    // host / deviceId / deviceName 建会话。待机唤醒走这条路 ——
    // 扫描要 2.5 秒，等它扫完开头的声音已经错过了。
    //
    // forceScan = true 时无视设备缓存强制重扫（用户点"重新扫描"用）。
    //
    // 常规重连（skipResolve = false）也先试一次直连：
    // resolveDevice 会同步扫网段 2.5 秒，每次重连都扫一遍，托盘菜单就僵住。
    // 记住的设备通常还活着，直连基本都能成 —— 扫描留作回落。
    auto tryConnect = [&](bool skipResolve = false, bool forceScan = false) {
        if (sess.running.load()) return;

        // 直连路径：skipResolve 走它；forceScan（重新扫描）不走 ——
        // 用户点"重新扫描"就是要真扫，不能被记住的设备挡住。
        const bool tryDirect = skipResolve || (!forceScan && !cfg.lastHost.empty());
        if (tryDirect) {
            if (startWithRememberedDevice()) return;   // 直连成功
            // 直连失败：静默回落。这里绝不能弹「启动失败」——
            // 重连循环每几秒来一次，会反复弹气泡。
            if (skipResolve) return;   // 待机唤醒没有回落路径，交给常规重连
        }

        if (resolveDevice(sess, cfg, discovered, lastScanMs, forceScan)) {
            std::string err;
            if (!sess.start(err)) {
                tray.setTooltip(i18n::tr("AirPlay Bridge - 启动失败"));
                tray.notify("AirPlay Bridge",
                                std::string(i18n::tr("启动失败: ")) + err);
            } else {
                cfg.lastHost = sess.host;
                if (!sess.deviceId.empty()) cfg.lastDeviceId = sess.deviceId;
                if (!sess.deviceName.empty()) cfg.lastDeviceName = sess.deviceName;
                saveConfig(cfgPath, cfg);
            }
        } else {
            tray.setTooltip(i18n::tr("AirPlay Bridge - 未找到设备"));
            // 首次运行那一次额外给个气泡 —— 否则用户看到的就是
            // 一个灰图标 + 菜单里「尚未扫描到设备」，无从判断是坏了
            // 还是没配好。exchange 消费掉，重连循环里不会再弹。
            if (g_notifyIfNoDevice.exchange(false)) {
                tray.notify("AirPlay Bridge", i18n::tr(
                                "没找到 AirPlay 设备。请确认音箱已通电，"
                                "并且和这台电脑在同一个网络下。"));
            }
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

    // 看门狗线程：只托盘模式拉起来。
    // 它盯着 g_mainTick —— 主线程每轮循环都会刷新它，哪天刷不动了
    // （卡在扫描 / 开采集 / 握手里），日志里就会留下"卡了多久、卡在哪"。
    g_mainTick.store(steadyNowMs());
    std::thread watchdog(watchdogLoop);

    // ---- 首次运行引导 ----
    //
    // 只在「用户手动打开托盘模式」时做，而且只做一次。
    // 绝不能在开机自启启动时做（那是 --last）—— 用户每次开机都被问
    // 一遍会非常烦。
    //
    // 放在这里而不是更早：托盘图标刚创建好，弹窗时图标已经在了，
    // 用户不会看到一个"没有来处"的对话框。
    if (wantTray && !wantLast && !cfg.askedAutoStart) {
        g_notifyIfNoDevice.store(true);   // 这次没扫到设备就提示一下
        // 先问界面语言 —— 必须排在「问自启」之前，
        // 这样后面那个弹窗出来时已经是用户选的语言了。
        askLanguageOnFirstRun(cfg, cfgPath);
        i18n::setLang(i18n::fromConfigValue(cfg.language));
        cfg.askedAutoStart = true;
        saveConfig(cfgPath, cfg);         // 先落盘，之后再崩也不会重复问
        askAutoStartOnFirstRun();
    }

    // 立即尝试连接。
    //
    // 以前这里是 if (wantLast || !cfg.lastHost.empty()) —— 只有"连过"
    // 才立即连。于是**首次运行**要干等一个重连退避周期（5 秒）才轮到它，
    // 而那 5 秒里菜单显示"尚未扫描到设备"，用户以为程序坏了。
    // 既然迟早都会连（重连分支一定会触发），不如立刻开始。
    tryConnect();

    // 主循环：同时 pump 托盘消息和网络
    auto lastCheck = std::chrono::steady_clock::now();
    // 重连退避：连续失败时逐次翻倍，避免每 5 秒猛扫一次
    int retryBackoffSec = 5;
    // 连接持续这么久才认为「真的连上了」，把重连退避复位回 5 秒。
    // 取值依据：HomePod 踢掉会话前通常能撑 3~8 秒，10 秒足以区分
    // 「短暂连上就被踢」和「稳定在用」。
    constexpr int kBackoffResetSec = 10;
    // 用户主动断开标记。
    // true = 用户自己点的"断开"，不要自动重连；
    // false = 正常状态（网络断线仍然自动重连）。
    // 没有这个标志的话，用户点完断开 5 秒后它又自己连上了。
    bool userDisconnected = false;
    // 后台扫描（避免阻塞托盘消息循环）

    while (!g_stop.load()) {
        // 每轮开头：先记心跳，再把阶段重置成"空闲"。
        // 顺序不能反 —— 先记时间戳，看门狗才不会把"上一轮的阶段"算成这一轮。
        g_mainTick.store(steadyNowMs());
        setPhase(PHASE_IDLE);

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
                    line1 = i18n::tr("已连接");
                    // 设备名单独一行；过长则截断，避免把菜单撑宽
                    std::string who = sess.deviceName.empty() ? sess.host : sess.deviceName;
                    const size_t kMaxName = 22;
                    if (who.size() > kMaxName) who = who.substr(0, kMaxName) + "...";
                    line2 = who;
                    // IP 再单独一行（短，不会撑宽菜单）
                    line3 = sess.host;
                } else if (sess.running.load()) {
                    line1 = i18n::tr("连接中...");
                } else {
                    line1 = i18n::tr("未连接");
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
            menu.push_back({i18n::tr(sess.running.load() ? "断开" : "连接"), MENU_TOGGLE});

            // 防火墙修复入口：**只在需要时才出现**。
            //
            // 常驻显示，不做任何条件判断。
            //
            // 之前试着「只在缺规则时显示」，结果是把唯一的入口藏起来了：
            // 刚装好还没连过 -> 失败计数是 0 -> 不显示；已经加过规则 ->
            // 也不需要显示。两头都看不到它，用户根本不知道有这个东西。
            //
            // 点它时的行为才是分流的地方（见 MENU_FIX_FIREWALL 分支）：
            // 没规则就加，有规则就告诉用户已经配好了。
            // "选择设备"是个真子菜单（向右展开），内容来自缓存的设备列表。
            // 之所以要缓存：Windows 的菜单树必须在弹出前一次建好，
            // 没法在展开的那一刻去扫网段。
            // 「设备」一级子菜单：把 选择设备 / 隐藏设备 / 采集设备 收进来。
            //
            // 为什么可以收成三级：这三项都是**低频**的 —— 程序本来就会自动
            // 连上次的设备，用户配好一次基本不再动。家里也就一两台设备，
            // 不存在频繁切换。所以多两级展开可以接受，换来的是一级菜单更短。
            TrayMenuItem devGroup;
            devGroup.id = MENU_DEVICE_GROUP;
            devGroup.label = i18n::tr("设备");
            // 设备列表**直接**挂在「设备 ›」下 ——
            // 不再套一层「选择设备」：在叫「设备」的组里点「选择设备」是重复的，
            // 而且换设备要多展开一层。
            {
                if (discovered.empty()) {
                    // 缓存是空的（还没扫过，或者上次啥也没找到）
                    devGroup.children.push_back({i18n::tr("尚未扫描到设备"), 0, false, false, false});
                } else {
                    for (size_t i = 0; i < discovered.size(); ++i) {
                        TrayMenuItem d;
                        d.label = deviceMenuLabel(discovered[i]);
                        d.id = static_cast<int>(kDeviceMenuBase + i);
                        // 当前连着的打勾
                        d.checked = sess.connected.load() &&
                                    !discovered[i].host.empty() &&
                                    discovered[i].host == sess.host;
                        devGroup.children.push_back(std::move(d));
                    }
                }
                devGroup.children.push_back({"", 0, true, true, false});
                devGroup.children.push_back({i18n::tr("重新扫描"), MENU_RESCAN_SUB, true, false, false});
                // 设备列表和下面两项设置之间也隔一条线
                devGroup.children.push_back({"", 0, true, true, false});
            }

            // 「隐藏设备」子菜单 —— 列出**全部**扫到的设备，勾选 = 显示。
            //
            // 为什么必须列全部：被隐藏的设备不出现在「选择设备」里，
            // 如果这里也只列可见的，用户就再也没有入口把它放回来了 ——
            // 那会变成「一隐藏就永远消失」。
            {
                TrayMenuItem hideItem;
                hideItem.id = MENU_HIDE_DEVICE;
                hideItem.label = i18n::tr("隐藏设备");
                std::vector<AirPlayDevice> all;
                {
                    std::lock_guard<std::mutex> lk(g_lastScanAllMutex);
                    all = g_lastScanAll;
                }
                if (all.empty()) {
                    hideItem.children.push_back({"尚未扫描到设备", 0, false, false, false});
                } else {
                    for (size_t i = 0; i < all.size(); ++i) {
                        TrayMenuItem d;
                        d.label = all[i].name.empty()
                                      ? std::string(i18n::tr("(未命名设备)"))
                                      : all[i].name;
                        d.id = static_cast<int>(kHideDeviceMenuBase + i);
                        // 勾选 = 显示中。取消勾选即隐藏。
                        bool ignored = false;
                        for (auto& ig : cfg.ignoredDeviceIds)
                            if (!ig.empty() && ig == all[i].deviceId) {
                                ignored = true; break;
                            }
                        d.checked = !ignored;
                        // 没有 deviceId 的设备无法被记住，禁止操作。
                        d.enabled = !all[i].deviceId.empty();
                        hideItem.children.push_back(std::move(d));
                    }
                }
                devGroup.children.push_back(std::move(hideItem));
            }

            // 「采集设备」子菜单 —— 指定从哪个输出端点采声音。
            // 默认跟随系统默认输出设备（绝大多数人不用改）。
            {
                TrayMenuItem capItem;
                capItem.id = MENU_CAPTURE_DEVICE;
                capItem.label = i18n::tr("采集设备");
                capItem.children.push_back({i18n::tr("跟随系统默认"),
                                           MENU_CAPTURE_DEVICE + 1000,
                                           true, false,
                                           cfg.captureDeviceId.empty()});
                if (!g_captureDevices.empty())
                    capItem.children.push_back({"", 0, true, true, false});
                for (size_t i = 0; i < g_captureDevices.size(); ++i) {
                    TrayMenuItem d;
                    d.label = g_captureDevices[i].second.empty()
                                  ? std::string(i18n::tr("(未命名设备)"))
                                  : g_captureDevices[i].second;
                    d.id = static_cast<int>(kCaptureDeviceMenuBase + i);
                    d.checked = (cfg.captureDeviceId ==
                                 g_captureDevices[i].first);
                    capItem.children.push_back(std::move(d));
                }
                devGroup.children.push_back(std::move(capItem));
            }
            menu.push_back(std::move(devGroup));

            // 延迟 / 音量 / 音质 —— 三个都是音频参数，都做成真子菜单。
            //
            // 内容都是固定的预设档位，不需要扫描，所以可以直接建成
            // 带 children 的子菜单 —— 这样系统会自动加个 › 箭头，
            // 用户一眼就知道能展开。

            // 延迟
            {
                TrayMenuItem it;
                it.id = MENU_LATENCY;   // 父项本身不可点，但保留 id 便于识别
                it.label = std::string(i18n::tr("延迟")) + "  " + std::to_string(cfg.latencyMs) + "ms";
                for (int i = 0; i < kLatencyLevelCount; ++i) {
                    it.children.push_back({i18n::tr(kLatencyLevels[i].label),
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
                it.children.push_back({i18n::tr("自定义（滑块）..."), MENU_LATENCY_CUSTOM,
                                       true, false, latencyCustom});
                menu.push_back(std::move(it));
            }

            // 音量
            {
                TrayMenuItem it;
                it.id = MENU_VOLUME;
                it.label = std::string(i18n::tr("音量")) + "  " + std::to_string(cfg.volume) + "%";
                for (int i = 0; i < kVolumeLevelCount; ++i) {
                    it.children.push_back({i18n::tr(kVolumeLevels[i].label),
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
                it.children.push_back({i18n::tr("自定义（滑块）..."), MENU_VOLUME_CUSTOM,
                                       true, false, volumeCustom});
                menu.push_back(std::move(it));
            }

            // 音质
            {
                TrayMenuItem it;
                it.id = MENU_QUALITY;
                it.label = std::string(i18n::tr("音质  ")) + qualityLevelName(cfg.qualityLevel);
                for (int i = 0; i < kQualityLevelCount; ++i) {
                    it.children.push_back({i18n::tr(kQualityLevels[i].label),
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
                it.children.push_back({i18n::tr("自定义（滑块）..."), MENU_QUALITY_CUSTOM,
                                       true, false, qualityCustom});
                menu.push_back(std::move(it));
            }
            menu.push_back({"", 0, true, true, false});
            // 更多设置 —— 平时不需要动的那些收在这里。
            // 命名照 Apple 系统设置的习惯：短名词短语、不放动词、不塞解释。
            {
                TrayMenuItem adv;
                adv.id = MENU_ADVANCED;
                // 叫「更多设置」而不是「高级设置」：里面五项都是常规设置，
                // 没有一项是"改了会出问题"的那种。用「高级」会让用户连不上时
                // 犹豫「高级的我不该碰」—— 恰好挡住最需要它的场景。
                adv.label = i18n::tr("更多设置");

                // 连接诊断：检查并修复网络放行（原来叫「修复连接问题」，动词开头太长）
                adv.children.push_back({i18n::tr("连接诊断"), MENU_FIX_FIREWALL});

                // 无声音时待机：原来叫「静音自动待机」——
                // 「静音」和「待机」两个概念粘在一起，容易被读成「静音就待机」。
                TrayMenuItem it;
                it.id = MENU_STANDBY;   // 父项本身不可点，但保留 id 便于识别
                const bool sbOn = standbyActiveConfig(cfg);
                it.label = sbOn
                               ? (std::string(i18n::tr("无声音时待机  ")) +
                                  tenthsToSecsText(cfg.standbyTenths))
                               : std::string(i18n::tr("无声音时待机  关闭"));
                bool standbyCustom = true;
                for (int i = 0; i < kStandbyLevelCount; ++i) {
                    const bool on = sbOn &&
                                    cfg.standbyTenths == kStandbyLevels[i].tenths;
                    if (on) standbyCustom = false;
                    it.children.push_back({i18n::tr(kStandbyLevels[i].label),
                                           kStandbyMenuBase + i,
                                           true, false, on});
                }
                it.children.push_back({"", 0, true, true, false});
                // 值不等于任何档位时勾打在「自定义（滑块）」上 —— 光比档位
                // 会让滑块调出来的 7.5 秒一个勾都没有。
                it.children.push_back({i18n::tr("自定义（滑块）..."), MENU_STANDBY_CUSTOM,
                                       true, false, standbyCustom});
                it.children.push_back({"", 0, true, true, false});
                // 时长为 0 也走这一项 —— 0 在语义上就是关闭。
                it.children.push_back({i18n::tr("关闭（不自动待机）"), MENU_STANDBY + 1000,
                                       true, false, !sbOn});
                adv.children.push_back(std::move(it));

                // 静音本机输出：原来叫「连接时静音电脑扬声器」——
                // 一句话塞进菜单，Apple 不会这么写。
                adv.children.push_back({i18n::tr("静音本机输出"), MENU_MUTE_LOCAL,
                                        true, false, cfg.muteLocalOnConnect});

                // 开机自启：这里**不用** macOS 的「登录时打开」——
                // 参考系统设置的措辞是为了更好懂，而中文 Windows 用户对
                // 「开机自启」远比直译过来的说法熟。参考不等于照搬。
                adv.children.push_back({i18n::tr("开机自启"), MENU_AUTOSTART,
                                        true, false, isAutoStartEnabled()});

                // 语言排在「日志」之上：它比翻日志更常被点。
                // 语言：用各语言自己的写法作项名 —— 中文用户找「中文」、
                // 英文用户找「English」，都不需要先看懂另一种语言。
                {
                    TrayMenuItem lang;
                    lang.id = MENU_LANGUAGE;
                    lang.label = i18n::tr("语言");
                    const i18n::Lang cur = i18n::current();
                    lang.children.push_back({i18n::nativeName(i18n::Lang::Zh),
                                             MENU_LANGUAGE_ZH, true, false,
                                             cur == i18n::Lang::Zh});
                    lang.children.push_back({i18n::nativeName(i18n::Lang::En),
                                             MENU_LANGUAGE_EN, true, false,
                                             cur == i18n::Lang::En});
                    adv.children.push_back(std::move(lang));
                }

                adv.children.push_back({i18n::tr("日志"), MENU_OPEN_LOG});

                menu.push_back(std::move(adv));
            }
            // 关于 —— 检查更新并进来了：点开就是版本信息，有新版本再告诉用户。
            // 版本号不放在菜单标题里（那是「关于」弹窗该说的）。
            menu.push_back({i18n::tr("关于"), MENU_ABOUT});
            menu.push_back({"", 0, true, true, false});
            menu.push_back({i18n::tr("退出"), MENU_QUIT});

            // 打点：TrackPopupMenu 跑的是模态消息循环，主循环在里面停摆。
            // 这一步卡住的话用户看到的就是"点托盘没反应"。
            setPhase(PHASE_MENU_OPEN);
            tray.showMenu(menu, [&](int id) {
                // 设备列表里点了一项：直接连它。
                // 「隐藏设备」子菜单的点击 —— 切换该设备的显示/隐藏。
                if (id >= kHideDeviceMenuBase) {
                    std::vector<AirPlayDevice> all;
                    {
                        std::lock_guard<std::mutex> lk(g_lastScanAllMutex);
                        all = g_lastScanAll;
                    }
                    const size_t idx =
                        static_cast<size_t>(id - kHideDeviceMenuBase);
                    if (idx < all.size() && !all[idx].deviceId.empty()) {
                        const std::string& did = all[idx].deviceId;
                        auto& v = cfg.ignoredDeviceIds;
                        auto it = std::find(v.begin(), v.end(), did);
                        if (it == v.end()) {
                            v.push_back(did);
                            logLine("已隐藏设备: " + all[idx].name);
                        } else {
                            v.erase(it);
                            logLine("已恢复显示设备: " + all[idx].name);
                        }
                        saveConfig(cfgPath, cfg);
                    }
                    return;
                }

                // 「采集设备」子菜单的点击 —— 选中即保存并重连生效。
                if (id == MENU_CAPTURE_DEVICE + 1000) {
                    cfg.captureDeviceId.clear();
                    setCaptureDeviceId(std::string());
                    saveConfig(cfgPath, cfg);
                    logLine("采集设备改为跟随系统默认，重连生效");
                    if (sess.running.load()) {
                        sess.teardown();
                        tryConnect();
                    }
                    return;
                }
                if (id >= kCaptureDeviceMenuBase) {
                    const size_t ci =
                        static_cast<size_t>(id - kCaptureDeviceMenuBase);
                    if (ci < g_captureDevices.size()) {
                        cfg.captureDeviceId = g_captureDevices[ci].first;
                        setCaptureDeviceId(cfg.captureDeviceId);
                        saveConfig(cfgPath, cfg);
                        logLine("采集设备改为: " + g_captureDevices[ci].second +
                                "，重连生效");
                        if (sess.running.load()) {
                            sess.teardown();
                            tryConnect();
                        }
                    }
                    return;
                }

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
                        tray.setTooltip(i18n::tr("AirPlay Bridge - 启动失败"));
                        tray.notify("AirPlay Bridge",
                                std::string(i18n::tr("启动失败: ")) + err);
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
                                    std::string(i18n::tr("延迟已设为 ")) +
                                    std::to_string(cfg.latencyMs) +
                                    std::string("ms") + i18n::tr("，正在重连…"));
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
                                        std::string(i18n::tr("延迟已设为 ")) +
                                        std::to_string(picked) +
                                        std::string("ms") + i18n::tr("，正在重连…"));
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
                    tray.notify("AirPlay Bridge", i18n::tr("音质已调整，正在重连…"));
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
                        tray.notify("AirPlay Bridge", i18n::tr("音质已调整，正在重连…"));
                        if (sess.running.load()) {
                            sess.teardown();
                            tryConnect();
                        }
                    }
                    return;
                }

                // 静音待机固定档位
                if (id >= kStandbyMenuBase && id < kStandbyMenuBase + kStandbyLevelCount) {
                    cfg.standbyEnabled = true;
                    cfg.standbyTenths = kStandbyLevels[id - kStandbyMenuBase].tenths;
                    saveConfig(cfgPath, cfg);
                    logLine("静音待机改为 " + tenthsToSecsText(cfg.standbyTenths));
                    // 阈值是主循环每次检查现读的，改了立刻生效，不用重连
                    return;
                }
                if (id == MENU_STANDBY_CUSTOM) {   // "自定义（滑块）..."
                    const int picked = showStandbyDialog(cfg.standbyTenths);
                    // 注意是 >= 0 —— 拉到 0 表示关闭，也要存下来。
                    // 只处理 > 0 的话，用户往左拖到底会没反应。
                    if (picked >= 0) {
                        cfg.standbyTenths = picked;
                        cfg.standbyEnabled = true;
                        saveConfig(cfgPath, cfg);
                        logLine(picked > 0
                                    ? ("静音待机改为 " + tenthsToSecsText(picked))
                                    : std::string("静音待机已关闭（滑块拉到 0）"));
                    }
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

                if (id == MENU_LANGUAGE_EN || id == MENU_LANGUAGE_ZH) {
                    cfg.language = (id == MENU_LANGUAGE_ZH) ? "zh" : "en";
                    i18n::setLang(i18n::fromConfigValue(cfg.language));
                    saveConfig(cfgPath, cfg);
                    logLine("界面语言切换为 " + cfg.language);
                    // 菜单是每次弹出时重建的，下次打开就是新语言 —— 不用重连。
                    return;
                }

                if (id == MENU_MUTE_LOCAL) {
                    cfg.muteLocalOnConnect = !cfg.muteLocalOnConnect;
                    saveConfig(cfgPath, cfg);
                    // 关掉开关时如果端点还静着，立刻还回去 ——
                    // 不能等下一次断开，那时用户已经在困惑电脑为什么没声音了。
                    if (!cfg.muteLocalOnConnect &&
                        g_mutedByUs.exchange(false)) {
                        if (setSystemMuted(false))
                            logLine("已恢复电脑扬声器的声音（开关关闭）");
                        else
                            logLine("开关关闭后恢复电脑扬声器失败");
                    }
                    return;
                }

                switch (id) {
                    case MENU_TOGGLE:
                        if (sess.running.load()) {
                            // 用户主动断开：标记一下，别再自动连回来
                            userDisconnected = true;
                            sess.teardown();
                            tray.setConnected(false);
                            tray.setTooltip(i18n::tr("AirPlay Bridge - 已断开（手动）"));
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
                        tray.setTooltip(i18n::tr("AirPlay Bridge - 扫描中"));

                        // 先给个反馈再扫。扫描要一两秒，期间主循环是卡住的，
                        // 没有这句话用户会以为点击没生效。
                        tray.notify("AirPlay Bridge", i18n::tr("正在扫描局域网设备…"));
                        // 用户点重新扫描就是要刷新 —— 必须真扫，不能被缓存挡住。
                        setPhase(PHASE_SCAN);
                        discovered = filterDevices(discoverAirPlayDevices(2000), cfg);
                        // 刷新缓存年龄：这是最新的一份设备列表
                        lastScanMs = steadyNowMs();
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
                            tray.setTooltip(i18n::tr("AirPlay Bridge - 未找到设备"));
                        } else {
                            for (size_t i = 0; i < discovered.size(); ++i) {
                                sub.push_back({deviceMenuLabel(discovered[i]),
                                               static_cast<int>(1000 + i),
                                               true, false, false});
                            }
                            tray.setTooltip(
                                std::string(i18n::tr("AirPlay Bridge - 找到 ")) +
                                std::to_string(discovered.size()) + i18n::tr(" 台设备"));
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
                                tray.setTooltip(i18n::tr("AirPlay Bridge - 启动失败"));
                                tray.notify("AirPlay Bridge",
                                std::string(i18n::tr("启动失败: ")) + err);
                            } else {
                                cfg.lastHost = sess.host;
                                if (!sess.deviceId.empty()) cfg.lastDeviceId = sess.deviceId;
                                if (!sess.deviceName.empty()) cfg.lastDeviceName = sess.deviceName;
                                saveConfig(cfgPath, cfg);
                            }
                        });
                        break;
                    }

                    case MENU_FIX_FIREWALL:
                        // 一键加防火墙入站规则：提权拉起自己去跑
                        // --fix-firewall（用户会看到一次 UAC）。那一份加完
                        // 规则会弹 MessageBox 告知结果并退出 —— 这里不等它。
                        //
                        // 规则加好后不需要在这里做特殊处理：连接是断的，
                        // 下一轮重连本来就会再试一次，那时规则已经生效。
                        // 用户点了菜单后能看到的反馈来自：
                        //   1) 立即弹出的 UAC 窗口；
                        //   2) 那一份跑完后弹的结果提示框。
                        // 已经有规则就直接告知，不再走提权 —— 免得多弹一次 UAC。
                        if (firewallRuleExists()) {
                            logLine("「连接诊断」：本机已有放行规则，无需重复添加");
                            // 用 (wchar_t)10 拼换行，避免在源码里写转义字符。
                            const wchar_t kNLw = static_cast<wchar_t>(10);
                            std::wstring tip;
                            tip += i18n::trw("本机已有本程序的放行规则，无需重复添加。");
                            tip += kNLw; tip += kNLw;
                            tip += i18n::trw("若仍连不上，请确认接收端与本程序在同一网络下。");
                            promptUser(tip.c_str(),
                                        L"AirPlay Bridge", MB_OK);
                            break;
                        }
                        logLine("用户点击「连接诊断」，请求提权添加防火墙入站规则");
                        elevateAndRunFixFirewall();
                        break;
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
                    case MENU_ABOUT: {
                        // 「关于」：**立刻弹窗，不等网络** —— 点下去就有反应。
                        //
                        // 检查更新并进来了：弹完窗**有意不 break**，让控制流落到
                        // 下面 MENU_CHECK_UPDATE 那段去做一次后台检查。
                        // 有新版本时它会再弹一个窗；已是最新则完全静默。
                        std::string about = "AirPlay Bridge";
                        about += char(10); about += char(10);
                        about += std::string(i18n::tr("版本")) + " v" + kAppVersion;
                        about += char(10); about += char(10);
                        about += kProjectUrl;
                        {
                            const int need = MultiByteToWideChar(
                                CP_UTF8, 0, about.c_str(), -1, nullptr, 0);
                            std::wstring w(need > 1 ? static_cast<size_t>(need) : 0, 0);
                            if (need > 1)
                                MultiByteToWideChar(CP_UTF8, 0, about.c_str(), -1,
                                                    w.data(), need);
                            const std::wstring aboutTitle = i18n::trw("关于");
                            promptUser(w.c_str(), aboutTitle.c_str(),
                                       MB_OK | MB_SETFOREGROUND);
                        }
                        // 落下去执行检查更新（见上方注释）。
                    }
                    // ★ 属性必须在**真正的穿越点**上，也就是本条 case 结束、
                    //   紧接着下一个 case 标签之前。放在块首是无效的
                    //   （标准要求它是该 switch section 的最后一条语句）——
                    //   2026-10-18 外部审查指出位置放错了。
                    [[fallthrough]];
                    case MENU_CHECK_UPDATE: {
                        // 网络请求放独立线程 —— 托盘程序卡一下就会被当成死掉。
                        // 结果通过 g_updateResult 交给主循环去弹窗（见下面轮询）。
                        if (g_updateRunning.load()) break;   // 防连点
                        g_updateRunning.store(true);
                        logLine("开始检查更新，当前版本 " + std::string(kAppVersion));
                        // 不要捕获 [&] —— 这个线程是 detach 的，用户点「退出」后
                        // 它可能还在跑，那时 main 的局部作用域已经析构，
                        // 捕获到的引用全是悬垂。
                        //
                        // 这里用不着捕获任何东西：线程体只用全局量
                        // （g_updateMutex / g_updateResult / g_updateReady）
                        // 和它自己的局部变量。所以留空捕获列表最安全 ——
                        // 以后有人想往里面加东西，编译器会直接报错拦住。
                        std::thread([]() {
                            std::string tag, url;
                            const bool ok = fetchLatestRelease(tag, url);
                            UpdateResult r;
                            r.state = ok ? (isNewerVersion(tag, kAppVersion)
                                            ? UpdateState::HasNew : UpdateState::UpToDate)
                                         : UpdateState::Failed;
                            r.tag = tag; r.url = url;
                            {
                                std::lock_guard<std::mutex> lk(g_updateMutex);
                                g_updateResult = std::move(r);
                            }
                            g_updateReady.store(true);
                            g_updateRunning.store(false);
                        }).detach();
                        break;
                    }
                    case MENU_QUIT:
                        g_stop.store(true);
                        break;
                    default:
                        break;
                }
            });
        });

        // 检查更新的结果回来了就弹窗。
        // 不在网络线程里弹 —— MessageBox 是模态的，在非 UI 线程上弹
        // 没有正确的父窗口和消息泵，容易出怪问题。
        if (g_updateReady.load()) {
            g_updateReady.store(false);
            UpdateResult r;
            {
                std::lock_guard<std::mutex> lk(g_updateMutex);
                r = g_updateResult;
            }
            const wchar_t kNLw = static_cast<wchar_t>(10);
            std::wstring tip;
            const std::wstring cur(kAppVersion, kAppVersion + std::strlen(kAppVersion));
            if (r.state == UpdateState::Failed) {
                tip += i18n::trw("检查更新失败，请稍后重试。");
            } else if (r.state == UpdateState::UpToDate) {
                // 已是最新：**什么都不弹** ——
                // 用户点「关于」只是想看版本，没更新就不该多一个窗打扰。
                logLine("检查更新：已是最新版本 v" + std::string(kAppVersion));
                tip.clear();
            } else {
                tip += i18n::trw("发现新版本 ");
                tip += std::wstring(r.tag.begin(), r.tag.end());
                tip += i18n::trw("（当前 v"); tip += cur; tip += L"）。";
                tip += kNLw; tip += kNLw; tip += i18n::trw("是否前往下载？");
                logLine("发现新版本 " + r.tag + "（当前 " + std::string(kAppVersion) + "）");
                const int r2 = promptUser(tip.c_str(), L"AirPlay Bridge",
                                           MB_YESNO | MB_SETFOREGROUND);
                if (r2 == IDYES && !r.url.empty()) {
                    // ★ r.url 来自 GitHub API 返回的 JSON。
                    //   虽然那是我们自己请求的接口，但「把远端返回的字符串
                    //   直接交给 ShellExecute」是典型的注入面 —— API 被劫持或
                    //   中间人改包，就能让用户点一下「检查更新」打开任意东西。
                    //   只放行 https://github.com/ 开头的地址（纵深防御）。
                    if (r.url.rfind("https://github.com/", 0) != 0) {
                        logLine("更新检查: 返回的链接不是 github.com，已忽略");
                    } else
                    ShellExecuteA(nullptr, "open", r.url.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
                tip.clear();
            }
            if (!tip.empty()) {
                promptUser(tip.c_str(), L"AirPlay Bridge",
                            MB_OK | MB_SETFOREGROUND);
                logLine(r.state == UpdateState::Failed
                            ? std::string("检查更新失败（网络不可用或接口异常）")
                            : ("检查更新：已是最新版本 " + std::string(kAppVersion)));
            }
        }

        // ---- 音频链路健康度汇总 ----
        // 每 5 秒看一次两个计数器，**只在有变化时**记一条 ——
        // 正常播放时一条都不写，出问题时能直接指出是「丢帧」还是「削波」。
        {
            static uint64_t lastDropped = 0;
            static uint64_t lastClipped = 0;
            static long long lastHealthCheck = 0;
            const long long nowH = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowH - lastHealthCheck >= 5000) {
                lastHealthCheck = nowH;
                const uint64_t dNow = g_ringDroppedFrames.load();
                const uint64_t cNow = g_clippedSamples.load();
                if (dNow != lastDropped) {
                    logLine("音频丢帧：累计 " + std::to_string(dNow) +
                            " 帧推不进缓冲环（音频不连续，会造成爆音）");
                    lastDropped = dNow;
                }
                // 采集电平：诊断期间每窗口都记一条（看清链路才有得判断）。
                {
                    const int pk = g_peakWindowMax.exchange(0);
                    logLine("采集电平：本 5 秒最大峰值 " + std::to_string(pk) +
                            "（静音阈值 " + std::to_string(kSilencePeak) + "）");
                }

                if (cNow != lastClipped) {
                    logLine("音频削波：累计 " + std::to_string(cNow) +
                            " 次峰值触顶（系统混音可能过载，会产生失真）");
                    lastClipped = cNow;
                }
            }
        }

        if (sess.running.load()) sess.pump();

        auto now = std::chrono::steady_clock::now();

        // 连上了就把退避复位回最短档。
        //
        // 判据必须是 connected，不能是 running —— start() 一返回
        // running 就是 true，哪怕握手随后立刻失败。用 running 的话退避
        // 永远长不起来，变成一直敲接收端；而它在连续被失败配对骚扰后会
        // 进入拒绝状态，于是它一直拒绝、我们一直敲，永远连不上。
        //
        // 之前这段被绑在「后台扫描是否回来」上，是错的：直连成功那条路
        // 根本不启动扫描，退避就永远不会复位。
        //
        // ★ 但「连上」这个判据太弱：HomePod 只允许一个活动会话，被踢之后
        //   如果我们 5 秒就重连，它往往还没释放上一个会话，于是又被踢。
        //   而每次被踢前都短暂 connected 过 —— 退避在那一瞬间被复位成 5，
        //   于是永远是「5 秒一次、连上、被踢」的死循环，直到设备自己冷却
        //   （实测要 90 秒），期间用户完全连不上。
        //   因此改成：连接持续存活满 kBackoffResetSec 秒才算「真的连上了」。
        static std::chrono::steady_clock::time_point connSince{};
        static bool wasConnected = false;
        if (sess.connected.load()) {
            if (!wasConnected) { connSince = now; wasConnected = true; }
            if (std::chrono::duration_cast<std::chrono::seconds>(now - connSince).count()
                    >= kBackoffResetSec)
                retryBackoffSec = 5;
        } else {
            wasConnected = false;
        }

        // ---- 无声音时待机 ----
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
                // 静音够久了就待机。lastLoudMs 是"最近一次听到声音"的时间戳，
                // 连接成功时会被重置成当前时间（见 ev.launched），
                // 所以刚连上不会秒待机。
                // 十分之一秒 -> 毫秒是 ×100。
                // 用 standbyActiveConfig：时长为 0 时它返回 false，
                // 不会掉进「静音超过 0 秒」这个恒成立的条件里
                // （那样会连上就断、然后反复振荡）。
                const bool wantStandby =
                    standbyActiveConfig(cfg) && sess.running.load() &&
                    sess.connected.load() &&
                    nowMs - sess.lastLoudMs.load() >=
                        static_cast<long long>(cfg.standbyTenths) * 100;
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
                // 这里原本还起了一个后台扫描（std::async 扫网段），
                // 但它的结果从来没被读过 —— 真正用的是下面 tryConnect()
                // -> resolveDevice() 内部那份 discovered。等于每轮重连都白扫
                // 一次网段（约 2.5 秒、一次网段广播）。删掉。
                //
                // 如果以后真需要预热扫描，应该让 resolveDevice 用这份结果，
                // 而不是各扫各的。
                // 上一轮失败时没人收尾（掉线那条路不经过 teardown），
                // 采集线程还挂着。先收尾再连 —— 顺带把音频路由也恢复回去。
                sess.teardown();
                tryConnect();
                // 退避翻倍，上限 60 秒
                retryBackoffSec = (retryBackoffSec < 60) ? retryBackoffSec * 2 : 60;
            }
        }

        // 主循环节拍 20ms → 8ms。
        //
        // 音频 pacer 的设计节拍是 8ms（kPacerMs），而 pacer 靠主循环里的
        // loop->pump() → sender.tick() 驱动。20ms 一轮时，发包就是「每 20ms
        // 一批 2~3 个包」，批与批之间有 20ms 的空窗 —— 接收端要靠自己的缓冲
        // 撑过那个空窗。
        //
        // 缓冲越浅，空窗占比越大：150ms 时 20/150 = 13%，125ms 时 16%。
        // 实测的爆音边界正好落在 100~150 之间，所以把节拍对齐到 pacer 的设计值。
        //
        // 代价：主循环 50 次/秒 → 125 次/秒，空转开销上升（每轮只做 tray.pump
        // 和状态检查，都是很轻的活）。v1.5.3 已经 timeBeginPeriod(1)，
        // 所以 8ms 是真的 8ms，不会被时钟粒度拉长。
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // 停看门狗：先置 g_stop（上面循环条件已置），再 join。
    // 看门狗内部每 500ms 检查一次 g_stop，最坏等 500ms 就退出来。
    if (watchdog.joinable()) watchdog.join();

    sess.teardown();
    // 兜底：万一断开那条路没走到，退出前保证端点恢复原状，
    // 否则用户会以为电脑没声音了。
    // 退出前最后一次尝试；失败也没机会重试了，至少留一条日志。
    if (g_mutedByUs.exchange(false) && !setSystemMuted(false))
        logLine("退出时恢复电脑扬声器失败（程序即将退出，无法重试）");
    tray.destroy();
    CoUninitialize();
    return 0;
}
