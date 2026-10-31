// SPDX-License-Identifier: Apache-2.0
//
// tray.cpp -- Win32 托盘图标实现。
//
// 托盘的消息循环和网络引擎共用同一个线程：pump() 用 PeekMessage 把
// 窗口消息取出来，翻译成 TrayEvent 交给调用方。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "tray.h"
#include "darkmode.h"

#include <windows.h>
#include <shobjidl_core.h>   // SetCurrentProcessExplicitAppUserModelID
#include <shellapi.h>

#include <cmath>
#include <cstring>

namespace fxchain {
namespace {

constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;

// 菜单打开期间的音频心跳定时器（见 TrayIcon::setMenuPump）
constexpr UINT_PTR kMenuPumpTimer = 0x41;
constexpr UINT     kMenuPumpMs    = 10;
std::function<void()> g_menuPump;
// 心跳定时器的嵌套深度：showMenu 可重入（回调里再弹子菜单/二次菜单），
// 只有最外层负责 Set/Kill。以前无条件 Kill，内层菜单一关心跳就没了，
// 而外层回调还在跑 —— 音频无人喂，表现为「点某些菜单项，声音断了」。
// （2026-10-31 外部审查 P2-1。）
int g_menuPumpDepth = 0;
std::function<void()> g_quitRequest;   // WM_CLOSE -> 主循环

// Explorer 重启后会广播这条消息（消息号是动态的，必须运行时取）。
// 收到它要重新 Shell_NotifyIcon(NIM_ADD)，否则托盘图标永久消失。
UINT g_taskbarCreatedMsg = 0;
NOTIFYICONDATAW g_lastNid{};      // 记住最后一次注册的参数，便于重建
bool g_haveNid = false;
constexpr UINT ID_TRAY_ICON     = 1;

// 让 Windows 知道"这是哪个应用"。
//
// 不设的话，系统通知没有归属的应用标识，只能拿通用图标顶上（灰扑扑的）；
// 任务栏分组、通知中心里的应用名也会显示成 exe 文件名。
// 这个 ID 是进程内的自我声明，不需要注册表配合。
void declareAppUserModelId() {
    static bool done = false;
    if (done) return;
    done = true;
    SetCurrentProcessExplicitAppUserModelID(L"RiverFlowsInUUU.AirPlayBridge");
}

// 动态生成一个圆点图标（连接=绿，未连接=灰）
HICON makeDotIcon(bool connected) {
    const int SZ = 16;
    HDC hdc = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(hdc);

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = SZ;
    bi.bV5Height = -SZ;          // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                     DIB_RGB_COLORS, &bits, nullptr, 0);
    HBITMAP mask = CreateBitmap(SZ, SZ, 1, 1, nullptr);
    // ★ GDI 资源耗尽时这两个调用会失败 —— 继续往下就是写空指针。
    //   本函数是「缺图标时的回退」，自己再崩就没有任何图标可言了。
    //   （2026-10-31 外部审查 P2-5。）
    if (!color || !mask || !bits) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        DeleteDC(memDC);
        // hdc 来自 GetDC(nullptr)，必须用 ReleaseDC 释放；
        // DeleteDC 只用于 CreateDC 的句柄，对它调用会静默失败 → 屏幕 DC 泄漏
        // （复审 R3：与下方正常路径的 ReleaseDC 保持一致）。
        ReleaseDC(nullptr, hdc);
        return nullptr;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, color);

    BYTE r = connected ? 0x2E : 0x88;
    BYTE g = connected ? 0xCC : 0x88;
    BYTE b = connected ? 0x71 : 0x88;

    auto* px = static_cast<uint32_t*>(bits);
    const float cx = 7.5f, cy = 7.5f, radius = 6.5f;
    for (int y = 0; y < SZ; ++y) {
        for (int x = 0; x < SZ; ++x) {
            float dx = x - cx, dy = y - cy;
            float dist = dx * dx + dy * dy;
            uint32_t v = 0;
            if (dist <= radius * radius) {
                float edge = radius * radius - dist;
                BYTE a = (edge > radius) ? 255 : static_cast<BYTE>(edge / radius * 255);
                if (a > 200) a = 255;
                v = (static_cast<uint32_t>(a) << 24) |
                    (static_cast<uint32_t>(r) << 16) |
                    (static_cast<uint32_t>(g) << 8) |
                    static_cast<uint32_t>(b);
            }
            px[y * SZ + x] = v;
        }
    }

    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, hdc);

    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

TrayEventHandler g_handler;   // 单实例，够用

} // namespace

// 生成菜单项用的圆点位图。
// 用传统单色位图（1bpp）而不是 32 位 DIB —— 后者 SetMenuItemBitmaps
// 在某些 Windows 版本上不生效；单色位图是系统自己画对号用的格式，最稳。
// kind: 1 = 已连接（实心圆），2 = 未连接（空心圆）
HBITMAP makeDotBitmap(int kind) {
    const int SZ = 13;   // 菜单图标区的常用尺寸

    // 单色位图：每个像素 1 bit，1 = 用前景色画（黑色），0 = 不画
    // 行按 16 位对齐（每行 2 字节）
    const int stride = ((SZ + 15) / 16) * 2;
    BYTE bits[4 * 16] = {};

    const float cx = (SZ - 1) / 2.0f, cy = (SZ - 1) / 2.0f;
    const float rOuter = (SZ - 1) / 2.0f - 0.5f;
    const float rInner = rOuter - 2.0f;   // 空心圆的内径

    for (int y = 0; y < SZ; ++y) {
        for (int x = 0; x < SZ; ++x) {
            const float dx = x - cx, dy = y - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            bool on = false;
            if (kind == 1) {
                on = (d <= rOuter);                    // 实心
            } else {
                on = (d <= rOuter && d >= rInner);     // 空心
            }
            if (on) {
                // 单色位图：bit = 1 表示画（用系统菜单的文字色）
                bits[y * stride + (x / 8)] |= static_cast<BYTE>(0x80 >> (x % 8));
            }
        }
    }

    return CreateBitmap(SZ, SZ, 1, 1, bits);
}

// 递归建菜单。返回建好的 HMENU。
//
// 子菜单用 MF_POPUP 挂上 —— 这才是真子菜单（向右展开、带右箭头），
// 而不是"弹完一个再弹下一个"那种两个独立的框。
//
// 注意：菜单树必须在 TrackPopupMenu 之前一次建好，所以子菜单的内容
// 也得提前准备好 —— 这就是设备列表需要缓存的原因。
// 把 UTF-8 拷进 Win32 的定长宽字符缓冲（含结尾 NUL），超长就截断。
//
// ★ 不能直接 MultiByteToWideChar(..., -1, buf, N)：源串超长时那一整次调用**失败**，
//   buf 原样不动 —— 由于 NOTIFYICONDATA 是零初始化的，结果是
//   **空 tooltip / 空通知**，而不是截断。（P3-1，外部审查发现。）
//
//   这里先完整转成 std::wstring 再按目标容量截，所以也**不会把一个
//   多字节字符切一半**。
void copyWideTrunc(wchar_t* dst, size_t dstChars, const std::string& s) {
    if (dstChars == 0) return;
    dst[0] = 0;
    if (s.empty()) return;
    const int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (need <= 1) return;
    std::wstring w(static_cast<size_t>(need), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), need);
    w.resize(static_cast<size_t>(need - 1));      // 去掉结尾 NUL
    const size_t n = (w.size() < dstChars - 1) ? w.size() : dstChars - 1;
    std::wmemcpy(dst, w.c_str(), n);
    dst[n] = 0;
}

static HMENU buildMenuTree(const std::vector<TrayMenuItem>& menu,
                           HBITMAP* dotGreen, HBITMAP* dotGray) {
    HMENU hmenu = CreatePopupMenu();
    if (!hmenu) return nullptr;

    UINT posIndex = 0;
    for (auto& item : menu) {
        if (item.separator) {
            AppendMenuW(hmenu, MF_SEPARATOR, 0, nullptr);
            ++posIndex;
            continue;
        }

        wchar_t wbuf[256] = {};
        copyWideTrunc(wbuf, 256, item.label);

        if (!item.children.empty()) {
            HMENU hsub = buildMenuTree(item.children, dotGreen, dotGray);
            // CreatePopupMenu 失败时返回 nullptr（递归入口已经检查过一次）。
            // 不判空就会把一个空句柄当 HMENU 挂上去 —— 该项无法展开，
            // 严重时整个菜单行为异常。跳过这一项更安全。
            if (!hsub) continue;   // 注意：不 ++posIndex，避免位图索引错位
            UINT flags = MF_POPUP | MF_STRING;
            if (!item.enabled) flags |= MF_GRAYED;
            AppendMenuW(hmenu, flags, reinterpret_cast<UINT_PTR>(hsub), wbuf);
            ++posIndex;
            continue;
        }

        UINT flags = MF_STRING;
        if (!item.enabled) flags |= MF_GRAYED;
        if (item.checked)  flags |= MF_CHECKED;
        AppendMenuW(hmenu, flags, static_cast<UINT_PTR>(item.id), wbuf);

        // 状态点：按位置挂位图（同一 id 的多项也能各自挂上）
        if (item.dot != 0 && dotGreen && dotGray) {
            HBITMAP* slot = (item.dot == 1) ? dotGreen : dotGray;
            if (*slot == nullptr) *slot = makeDotBitmap(item.dot);
            if (*slot)
                SetMenuItemBitmaps(hmenu, posIndex, MF_BYPOSITION, *slot, *slot);
        }
        ++posIndex;
    }
    return hmenu;
}

// 图标已编译进 exe 资源段（见 bridge.rc），这里按 ID 取出来。
// 取不到就回退到画圆点，绝不因为缺图标而挂掉。
// 资源 ID: 102 = 已连接（绿）, 103 = 未连接（灰）
HICON loadTrayIcon(int resId, bool connected) {
    // 按系统托盘的小图标尺寸加载，避免系统缩放导致的模糊
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    HICON ic = static_cast<HICON>(
        LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(resId),
                   IMAGE_ICON, cx, cy, 0));
    if (ic) return ic;
    return makeDotIcon(connected);   // 回退
}

struct TrayIcon::Impl {
    HWND hwnd = nullptr;
    HBITMAP dotGreen = nullptr;   // 菜单里的绿点（已连接）
    HBITMAP dotGray  = nullptr;   // 菜单里的灰点（未连接）
    HICON iconConnected = nullptr;
    HICON iconIdle = nullptr;
    HICON current = nullptr;
    bool created = false;
    bool connected = false;
    // 防重入：菜单弹出时 TrackPopupMenu 自带模态消息循环，
    // 期间若又有右键消息进来会再弹一层，可能把正在跑的会话拆掉。
    bool menuOpen = false;

    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        // Explorer 重启：重新把图标挂上去
        if (g_taskbarCreatedMsg != 0 && msg == g_taskbarCreatedMsg) {
            if (g_haveNid) {
                g_lastNid.hWnd = hwnd;
                Shell_NotifyIconW(NIM_ADD, &g_lastNid);
            }
            return 0;
        }
        // 系统深浅色切换：重新应用到本窗口，并清掉菜单主题缓存 ——
        // 不清的话下一次弹出的菜单还是旧配色。
        if (msg == WM_SETTINGCHANGE && lp != 0 &&
            lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
            darkmode::applyToWindow(hwnd, darkmode::isDark());
            darkmode::flushMenuThemes();
            return 0;
        }
        // 窗口被要求关闭（任务管理器 / 系统关机 / 别处 PostMessage(WM_CLOSE)）。
        //
        // 不能让它落到 DefWindowProcW —— 那样窗口直接被销毁，而主循环
        // 还在跑（它只认 g_stop），进程就变成没有窗口的僵尸：
        // 托盘图标已经删了、清理也做完了，但任务管理器里还能看到它。
        if (msg == WM_CLOSE) {
            if (g_quitRequest) g_quitRequest();
            return 0;   // 不销毁窗口，退出由主循环统一收尾
        }

        // 注销 / 关机 / 重启：系统会在几秒内结束本进程。
        // 必须在这里就请求收尾 —— 否则 stop() 里「把默认播放设备切回去」
        // 那段永远不执行，用户下次开机时电脑是没声音的（设备卡在虚拟声卡上）。
        //
        // 注意覆盖面：这条只处理「系统给了机会」的退出路径。
        // 任务管理器强杀 / 崩溃 / 断电不会给任何通知，进程没有一个字节能执行到 ——
        // 那种情况靠启动时的恢复逻辑兜底。
        // WM_QUERYENDSESSION 只是「询问」：任何一个程序返回 FALSE 都能取消注销，
        // 这时候本程序**不该已经退掉了** —— 否则会出现「关机被别的程序拦下，
        // 但 AirPlay Bridge 已经自己关了」。所以这里只表态同意，不动手。
        //
        // 真正的收尾放在 WM_ENDSESSION 且 wParam 非零（确认结束会话）。
        // wParam == 0 表示会话被取消，什么都不做。
        if (msg == WM_QUERYENDSESSION) {
            return TRUE;    // 同意结束会话，但先不退出
        }
        if (msg == WM_ENDSESSION) {
            if (wp && g_quitRequest) g_quitRequest();
            return 0;
        }

        // 菜单开着时继续驱动音频 IO，否则接收端会饿死
        if (msg == WM_TIMER && wp == kMenuPumpTimer) {
            if (g_menuPump) g_menuPump();
            return 0;
        }

        if (msg == WM_TRAY_CALLBACK) {
            UINT ev = LOWORD(lp);
            if (g_handler) {
                switch (ev) {
                    case WM_LBUTTONUP:   g_handler(TrayEvent::LeftClick);   break;
                    case WM_RBUTTONUP:   g_handler(TrayEvent::RightClick);  break;
                    case WM_LBUTTONDBLCLK: g_handler(TrayEvent::DoubleClick); break;
                    default: break;
                }
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

TrayIcon::TrayIcon() { d_ = new Impl(); }

TrayIcon::~TrayIcon() {
    if (d_) { destroy(); delete d_; d_ = nullptr; }
}

bool TrayIcon::valid() const { return d_ && d_->created; }

bool TrayIcon::create(const std::string& tooltip) {
    if (!d_) return false;

    const wchar_t* CLASS_NAME = L"AlmaAirPlayBridgeTray";
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Impl::wndProc;
        wc.hInstance = hInst;
        wc.lpszClassName = CLASS_NAME;
        if (!RegisterClassExW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        registered = true;
    }

    d_->hwnd = CreateWindowExW(0, CLASS_NAME, L"AirPlay Bridge", 0,
                               0, 0, 0, 0, nullptr, nullptr, hInst, nullptr);
    if (!d_->hwnd) return false;

    // 菜单跟随系统深浅色（Windows 11 深色模式下原生菜单本来是白的）
    darkmode::applyToWindow(d_->hwnd, darkmode::isDark());

    declareAppUserModelId();   // 必须在注册托盘图标之前
    d_->iconConnected = loadTrayIcon(102, true);
    d_->iconIdle      = loadTrayIcon(103, false);
    d_->current       = d_->iconIdle;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = d_->hwnd;
    nid.uID = ID_TRAY_ICON;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_CALLBACK;
    nid.hIcon = d_->current;
    copyWideTrunc(nid.szTip, 128, tooltip);

    // 记住参数，Explorer 重启后要照着重挂
    g_lastNid = nid;
    g_haveNid = true;
    if (g_taskbarCreatedMsg == 0)
        g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");

    if (!Shell_NotifyIconW(NIM_ADD, &nid)) {
        // 失败路径必须自己收拾：destroy() 会因为 created==false 直接返回，
        // 把这些资源全漏掉。
        g_haveNid = false;
        if (d_->iconConnected) DestroyIcon(d_->iconConnected);
        if (d_->iconIdle)      DestroyIcon(d_->iconIdle);
        d_->iconConnected = d_->iconIdle = d_->current = nullptr;
        if (d_->hwnd) DestroyWindow(d_->hwnd);
        d_->hwnd = nullptr;
        return false;
    }

    d_->created = true;
    return true;
}

void TrayIcon::setTooltip(const std::string& tooltip) {
    if (!valid()) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = d_->hwnd;
    nid.uID = ID_TRAY_ICON;
    nid.uFlags = NIF_TIP;
    copyWideTrunc(nid.szTip, 128, tooltip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::setConnected(bool connected) {
    if (!valid()) return;
    if (d_->connected == connected && d_->current) return;
    d_->connected = connected;
    d_->current = connected ? d_->iconConnected : d_->iconIdle;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = d_->hwnd;
    nid.uID = ID_TRAY_ICON;
    nid.uFlags = NIF_ICON;
    nid.hIcon = d_->current;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::notify(const std::string& title, const std::string& text) {
    if (!valid()) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = d_->hwnd;
    nid.uID = ID_TRAY_ICON;

    // 通知图标必须自己指定，否则用系统通用图标（灰的）。
    //
    // 原来 dwInfoFlags = NIIF_NONE 且不给 hBalloonIcon —— Windows 10/11
    // 把托盘气泡转成系统 toast，没图标就顶一个灰的通用图标，跟托盘的
    // 橘橙色完全不搭。
    //
    // 用 102（已连接那枚，橘橙）而不是 d_->current：通知在任何状态下都可能
    // 发，图标不该随连接状态变 —— 它代表"这个程序"，不是"当前已连接"。
    //
    // ★ 只给 hBalloonIcon，绝不能带 NIF_ICON。
    // NIF_ICON + NIM_MODIFY 改的是**托盘图标本身**，不是气泡上那枚小图标；
    // 带上去就等于"任何通知都把托盘改成已连接"。更糟的是 setConnected()
    // 用 d_->connected 做去重缓存，内存里仍是 idle，后续 setConnected(false)
    // 直接 early-return —— 图标再也修不回来，直到重启。
    // 未连接时点「重新扫描」，图标就变成连接色、tooltip 却写着"未找到设备"。
    nid.uFlags = NIF_INFO;
    nid.hBalloonIcon = d_->iconConnected;
    // ★ 必须带 NIIF_NOSOUND —— 我们自己弹的提示不能出声。
    //
    // 没有它时 Windows 会播系统提示音，而提示音的播放设备是**默认设备**；
    // 我们在「连上」这一刻刚好把默认设备切成了 CABLE Input（没有物理输出），
    // 于是这个提示音：
    //   本机听不到  →  被我们自己的采集录进去  →  当作新会话的第一段音频
    //   送给 HomePod  →  用户听到「刚连上有一声通知音，而且常常是爆的」
    //
    // 也就是说：我们自己的界面提示音污染了我们正在转发的音频流。
    // 气泡本身照常显示（视觉提醒不受影响），只是不出声。
    nid.dwInfoFlags = NIIF_USER | NIIF_NOSOUND;
    copyWideTrunc(nid.szInfo, 256, text);
    copyWideTrunc(nid.szInfoTitle, 64, title);
    // 不要再写 nid.dwInfoFlags —— 上面已设为 NIIF_USER（用我们自己的图标），
    // 这里再赋值会把 NIIF_USER 盖掉，图标又变回系统那个灰的。
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayIcon::showMenu(const std::vector<TrayMenuItem>& menu,
                        const std::function<void(int)>& cb) {
    if (!valid()) return;
    if (d_->menuOpen) return;   // 已经在弹菜单了，忽略这一次
    d_->menuOpen = true;

    HMENU hmenu = buildMenuTree(menu, &d_->dotGreen, &d_->dotGray);
    if (!hmenu) {
        d_->menuOpen = false;
        return;
    }

    // 菜单一弹出来，主循环就进不去了 —— 先用定时器把音频心跳接上
    if (g_menuPump && g_menuPumpDepth++ == 0)
        SetTimer(d_->hwnd, kMenuPumpTimer, kMenuPumpMs, nullptr);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(d_->hwnd);

    int cmd = static_cast<int>(TrackPopupMenu(
        hmenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, d_->hwnd, nullptr));

    PostMessageW(d_->hwnd, WM_NULL, 0, 0);
    DestroyMenu(hmenu);

    // 先复位标志再回调 —— 否则回调里弹子菜单（比如延迟档位）会被自己挡掉。
    // 重入保护要防的是菜单还开着时又来一次右键，不是连续弹菜单。
    d_->menuOpen = false;

    // ★ 心跳必须活到回调结束，不能在这里就 KillTimer。
    //
    // 回调里会弹模态窗（「关于」、连接诊断的确认框）或开滑块窗口
    // （延迟/音量/音质/待机）—— 那些窗口各有自己的消息循环，主循环进不去。
    // 而音频是靠 pumpNonBlocking() 喂的：心跳一停，RTP 就断，
    // 用户听到的就是「点一下菜单里的东西，声音没了」。
    //
    // 实测确认（真走 showMenu 的测试入口）：修复前回调期间心跳 0 次；
    // 修复后正常跳动。模态循环会把 WM_TIMER 派发到托盘窗口，
    // 和 TrackPopupMenu 期间是同一个机制。
    if (cmd != 0 && cb) cb(cmd);

    if (g_menuPump && --g_menuPumpDepth == 0)
        KillTimer(d_->hwnd, kMenuPumpTimer);
}

void TrayIcon::setMenuPump(std::function<void()> pump) {
    g_menuPump = std::move(pump);
}

void TrayIcon::setQuitRequest(std::function<void()> cb) {
    g_quitRequest = std::move(cb);
}

void TrayIcon::pump(const TrayEventHandler& handler) {
    if (!valid()) return;
    g_handler = handler;

    // 每次只取一条消息。
    // 原来的 while 循环会把队列一次抽干 —— 用户连点几下右键时，
    // 队列里会堆着好几条 WM_TRAY_CALLBACK，它们会在同一个 pump() 里
    // 被连续派发。而右键处理会调 TrackPopupMenu（自带模态消息循环），
    // 于是弹出一层菜单、里面又处理下一条、再弹一层……同时回调里的
    // "断开" 分支会把正在跑的会话拆掉，有 use-after-free 风险。
    // 改成每次一条，主循环每 20ms 调一次 pump，天然挡住了这种重入。
    MSG msg;
    if (PeekMessageW(&msg, d_->hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void TrayIcon::destroy() {
    if (!d_ || !d_->created) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = d_->hwnd;
    nid.uID = ID_TRAY_ICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_haveNid = false;

    if (d_->dotGreen) { DeleteObject(d_->dotGreen); d_->dotGreen = nullptr; }
    if (d_->dotGray)  { DeleteObject(d_->dotGray);  d_->dotGray  = nullptr; }
    if (d_->iconConnected) DestroyIcon(d_->iconConnected);
    if (d_->iconIdle)      DestroyIcon(d_->iconIdle);
    d_->iconConnected = d_->iconIdle = d_->current = nullptr;

    if (d_->hwnd) DestroyWindow(d_->hwnd);
    d_->hwnd = nullptr;
    d_->created = false;
    g_handler = nullptr;
}

} // namespace fxchain
