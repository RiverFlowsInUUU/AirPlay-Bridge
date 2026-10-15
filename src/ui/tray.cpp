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
#include <shellapi.h>

#include <cmath>
#include <cstring>

namespace fxchain {
namespace {

constexpr UINT WM_TRAY_CALLBACK = WM_APP + 1;

// 菜单打开期间的音频心跳定时器（见 TrayIcon::setMenuPump）
constexpr UINT_PTR kMenuPumpTimer = 0x41;
constexpr UINT     kMenuPumpMs    = 15;
std::function<void()> g_menuPump;

// Explorer 重启后会广播这条消息（消息号是动态的，必须运行时取）。
// 收到它要重新 Shell_NotifyIcon(NIM_ADD)，否则托盘图标永久消失。
UINT g_taskbarCreatedMsg = 0;
NOTIFYICONDATAW g_lastNid{};      // 记住最后一次注册的参数，便于重建
bool g_haveNid = false;
constexpr UINT ID_TRAY_ICON     = 1;

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
        MultiByteToWideChar(CP_UTF8, 0, item.label.c_str(), -1, wbuf, 255);

        if (!item.children.empty()) {
            HMENU hsub = buildMenuTree(item.children, dotGreen, dotGray);
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
    MultiByteToWideChar(CP_UTF8, 0, tooltip.c_str(), -1, nid.szTip, 128);

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
    MultiByteToWideChar(CP_UTF8, 0, tooltip.c_str(), -1, nid.szTip, 128);
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
    nid.uFlags = NIF_INFO;
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nid.szInfo, 255);
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nid.szInfoTitle, 63);
    nid.dwInfoFlags = NIIF_NONE;
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
    if (g_menuPump) SetTimer(d_->hwnd, kMenuPumpTimer, kMenuPumpMs, nullptr);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(d_->hwnd);

    int cmd = static_cast<int>(TrackPopupMenu(
        hmenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, d_->hwnd, nullptr));

    if (g_menuPump) KillTimer(d_->hwnd, kMenuPumpTimer);

    PostMessageW(d_->hwnd, WM_NULL, 0, 0);
    DestroyMenu(hmenu);

    // 先复位标志再回调 —— 否则回调里弹子菜单（比如延迟档位）会被自己挡掉。
    // 重入保护要防的是菜单还开着时又来一次右键，不是连续弹菜单。
    d_->menuOpen = false;

    if (cmd != 0 && cb) cb(cmd);
}

void TrayIcon::setMenuPump(std::function<void()> pump) {
    g_menuPump = std::move(pump);
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
