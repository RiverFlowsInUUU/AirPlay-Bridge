// SPDX-License-Identifier: Apache-2.0
//
// latency_dialog.cpp -- 通用数值调节滑块窗口。
//
// 全部自绘：窗口里没有 trackbar、没有 static、没有 button 子控件。
//
// 为什么不用系统控件：深色模式下它们的配色跟不上 —— trackbar 根本没有
// 深色主题，两个按钮也是浅灰的。只把窗口背景刷黑、控件还是浅色会更难看。
// 而这窗口是延迟 / 音量 / 音质三个调节项共用的门面，值得自己画。
//
// 配色跟随系统深浅色；滑块主色取自托盘图标，两处观感是一套。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "latency_dialog.h"
#include "i18n.h"
#include "darkmode.h"

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>

#include <cstdio>
#include <string>

#pragma comment(lib, "dwmapi.lib")

namespace fxchain {
namespace {

// 取自托盘图标（HomePod mini 织物），让滑块和图标是一套色
constexpr COLORREF kAccent = RGB(0xE0, 0x58, 0x38);

struct Theme {
    COLORREF bg, text, muted, track, btnText, btnBorder, btnFill;
};

Theme themeFor(bool dark) {
    Theme t{};
    if (dark) {
        t.bg        = RGB(0x20, 0x20, 0x20);
        t.text      = RGB(0xFF, 0xFF, 0xFF);
        t.muted     = RGB(0x9A, 0x9A, 0x9A);
        t.track     = RGB(0x3A, 0x3A, 0x3A);
        t.btnText   = RGB(0xFF, 0xFF, 0xFF);
        t.btnBorder = RGB(0x4A, 0x4A, 0x4A);
        t.btnFill   = RGB(0x2B, 0x2B, 0x2B);
    } else {
        t.bg        = RGB(0xF3, 0xF3, 0xF3);
        t.text      = RGB(0x1A, 0x1A, 0x1A);
        t.muted     = RGB(0x6B, 0x6B, 0x6B);
        t.track     = RGB(0xD4, 0xD4, 0xD4);
        t.btnText   = RGB(0x1A, 0x1A, 0x1A);
        t.btnBorder = RGB(0xC8, 0xC8, 0xC8);
        t.btnFill   = RGB(0xFA, 0xFA, 0xFA);
    }
    return t;
}

struct DialogState {
    const SliderDialogSpec* spec = nullptr;
    int  value = 0;
    bool ok = false;
    bool dark = false;
    int  dpi = 96;
    int  w = 0, h = 0;          // 逻辑尺寸（像素，已含 DPI 缩放）
    int  pad = 0;
    int  trackY = 0;            // 滑块轨道中心 y
    int  trackH = 0;
    int  thumbR = 0;
    RECT btnApply{}, btnCancel{};
    HFONT fValue = nullptr, fGrade = nullptr, fSmall = nullptr;
    bool hotApply = false, hotCancel = false;
    bool dragging = false;
};

int dpiOf(HWND hwnd) {
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn) {
        UINT d = fn(hwnd);
        if (d >= 72) return static_cast<int>(d);
    }
    HDC dc = GetDC(nullptr);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d >= 72 ? d : 96;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? static_cast<size_t>(n - 1) : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

HFONT makeFont(int dpi, int px, bool bold) {
    return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0,
                       bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

void fillRound(HDC dc, const RECT& r, int radius, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    HGDIOBJ ob = SelectObject(dc, br);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
}

void strokeRound(HDC dc, const RECT& r, int radius, COLORREF color) {
    HBRUSH br = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HGDIOBJ ob = SelectObject(dc, br);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

void drawTextIn(HDC dc, const std::wstring& s, const RECT& r, HFONT f,
                COLORREF color, UINT flags) {
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT rr = r;
    DrawTextW(dc, s.c_str(), -1, &rr, flags | DT_NOPREFIX);
    SelectObject(dc, of);
}

int s(int dpi, int px) { return MulDiv(px, dpi, 96); }

int trackLeft(const DialogState& st)  { return st.pad; }
int trackRight(const DialogState& st) { return st.w - st.pad; }

int xFromValue(const DialogState& st) {
    const int range = st.spec->maxValue - st.spec->minValue;
    const int t = range > 0 ? (st.value - st.spec->minValue) * 1000 / range : 0;
    const int L = trackLeft(st), R = trackRight(st);
    return L + (R - L) * t / 1000;
}

int valueFromX(const DialogState& st, int x) {
    const int L = trackLeft(st), R = trackRight(st);
    int t = (x - L) * 1000 / (R > L ? R - L : 1);
    if (t < 0) t = 0;
    if (t > 1000) t = 1000;
    const int range = st.spec->maxValue - st.spec->minValue;
    return st.spec->minValue + range * t / 1000;
}

// 把内部值按 spec 的 displayDivisor / displayDecimals 格式化成显示文本。
//
// 手写整数除法而不是 snprintf("%.*f")：%.*f 的实参会走 double，
// 在 /utf-8 下还得再考虑格式化行为差异；整数除 + 取余没有这些麻烦。
std::wstring formatValue(int value, int divisor, int decimals) {
    if (divisor <= 0) divisor = 1;
    if (decimals < 0) decimals = 0;

    // 内部值是整数，负数先取绝对值算，最后补符号 ——
    // 取余对负数会给出负的余数，直接拼会变成 "7.-5"。
    const bool neg = value < 0;
    const long long absV = neg ? -static_cast<long long>(value) : value;
    const long long d = divisor;

    std::string out;
    if (neg) out += '-';
    out += std::to_string(absV / d);          // 整数部分
    if (decimals > 0) {
        out += '.';
        long long frac = absV % d;            // 小数部分（以 divisor 为分母）
        // 逐位取：按目标小数位把分母缩到 10^decimals，
        // 多出来的低位四舍五入进本位。
        long long scale = 1;
        for (int i = 0; i < decimals; ++i) scale *= 10;
        // frac / d 是 [0,1)，乘 scale 变成目标小数位的定点数
        long long scaled = (frac * scale * 2 + d) / (d * 2);   // 四舍五入
        for (int i = decimals - 1; i >= 0; --i) {
            long long p = 1;
            for (int k = 0; k < i; ++k) p *= 10;
            out += static_cast<char>('0' + static_cast<int>((scaled / p) % 10));
        }
    }
    return utf8ToWide(out);
}

std::wstring valueText(const DialogState& st) {
    std::wstring v = formatValue(st.value, st.spec->displayDivisor,
                                 st.spec->displayDecimals);
    const std::string unit = st.spec->unit;
    if (!unit.empty()) v += L" " + utf8ToWide(unit);
    return v;
}

void paint(HWND hwnd, DialogState& st) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    RECT rc{};
    GetClientRect(hwnd, &rc);

    // 双缓冲，避免拖动时闪
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    const Theme th = themeFor(st.dark);
    HBRUSH bg = CreateSolidBrush(th.bg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    const int dpi = st.dpi, pad = st.pad;

    // 数值
    RECT rv{pad, s(dpi, 24), st.w - pad, s(dpi, 80)};
    drawTextIn(mem, valueText(st), rv, st.fValue, th.text,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 档位评价
    if (st.spec->grade) {
        const char* g = st.spec->grade(st.value);
        if (g && *g) {
            RECT rg{pad, s(dpi, 82), st.w - pad, s(dpi, 106)};
            drawTextIn(mem, utf8ToWide(g), rg, st.fGrade, kAccent,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // 滑块
    const int cy = st.trackY, L = trackLeft(st), R = trackRight(st);
    const int half = st.trackH / 2;
    RECT trackR{L, cy - half, R, cy + half};
    fillRound(mem, trackR, half, th.track);

    const int tx = xFromValue(st);
    if (tx > L) {
        RECT fillR{L, cy - half, tx, cy + half};
        fillRound(mem, fillR, half, kAccent);
    }

    // 滑块头：白底 + 主色描边，两种主题下都清楚
    const int tr = st.thumbR;
    HBRUSH wb = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    HGDIOBJ ob = SelectObject(mem, wb);
    HPEN pen = CreatePen(PS_SOLID, s(dpi, 2), kAccent);
    HGDIOBJ op = SelectObject(mem, pen);
    Ellipse(mem, tx - tr, cy - tr, tx + tr, cy + tr);
    SelectObject(mem, op);
    SelectObject(mem, ob);
    DeleteObject(pen);
    DeleteObject(wb);

    // 两端刻度值（同样按显示单位换算：待机那项就是 5.0 / 10.0）
    RECT rmin{pad, s(dpi, 166), st.w / 2, s(dpi, 186)};
    drawTextIn(mem, formatValue(st.spec->minValue, st.spec->displayDivisor,
                                st.spec->displayDecimals),
               rmin, st.fSmall, th.muted, DT_LEFT | DT_TOP | DT_SINGLELINE);
    RECT rmax{st.w / 2, s(dpi, 166), st.w - pad, s(dpi, 186)};
    drawTextIn(mem, formatValue(st.spec->maxValue, st.spec->displayDivisor,
                                st.spec->displayDecimals),
               rmax, st.fSmall, th.muted, DT_RIGHT | DT_TOP | DT_SINGLELINE);

    // 说明
    RECT rh{pad, s(dpi, 198), st.w - pad, s(dpi, 238)};
    drawTextIn(mem, utf8ToWide(st.spec->hint), rh, st.fSmall, th.muted,
               DT_LEFT | DT_TOP | DT_WORDBREAK);

    // 按钮
    RECT ba = st.btnApply, bc = st.btnCancel;
    fillRound(mem, bc, s(dpi, 6), st.hotCancel ? th.track : th.btnFill);
    strokeRound(mem, bc, s(dpi, 6), th.btnBorder);
    drawTextIn(mem, i18n::trw("取消").c_str(), bc, st.fGrade, th.btnText,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    COLORREF applyFill = kAccent;
    if (st.hotApply) applyFill = RGB(GetRValue(kAccent) + 18 > 255 ? 255 : GetRValue(kAccent) + 18,
                                     GetGValue(kAccent) + 12, GetBValue(kAccent) + 10);
    fillRound(mem, ba, s(dpi, 6), applyFill);
    drawTextIn(mem, i18n::trw("应用").c_str(), ba, st.fGrade, RGB(0xFF, 0xFF, 0xFF),
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

void makeFonts(DialogState& st) {
    if (st.fValue) DeleteObject(st.fValue);
    if (st.fGrade) DeleteObject(st.fGrade);
    if (st.fSmall) DeleteObject(st.fSmall);
    st.fValue = makeFont(st.dpi, 40, true);
    st.fGrade = makeFont(st.dpi, 14, false);
    st.fSmall = makeFont(st.dpi, 12, false);
}

void layout(DialogState& st) {
    st.pad    = s(st.dpi, 28);
    st.w      = s(st.dpi, 460);
    st.h      = s(st.dpi, 292);
    st.trackY = s(st.dpi, 150);
    st.trackH = s(st.dpi, 6);
    st.thumbR = s(st.dpi, 9);

    const int bw = s(st.dpi, 100), bh = s(st.dpi, 36);
    const int gap = s(st.dpi, 12);
    const int by = st.h - s(st.dpi, 12) - bh;
    // 从右往左排：应用贴右边距，取消在它左边留一个间距。
    const int applyRight = st.w - st.pad;
    st.btnApply  = RECT{applyRight - bw, by, applyRight, by + bh};
    st.btnCancel = RECT{applyRight - bw - gap - bw, by,
                        applyRight - bw - gap, by + bh};
}

bool inRect(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

void applyValue(DialogState& st, int v) {
    const int step = st.spec->step > 0 ? st.spec->step : 1;
    // 吸附到步长网格（相对最小值，避免 minValue 不是步长整数倍时整体偏移）
    if (step > 1) {
        const int rel = v - st.spec->minValue;
        v = st.spec->minValue + ((rel + step / 2) / step) * step;
    }
    if (v < st.spec->minValue) v = st.spec->minValue;
    if (v > st.spec->maxValue) v = st.spec->maxValue;
    st.value = v;
}

void setDarkTitleBar(HWND hwnd, bool dark) {
    BOOL v = dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE：正式常量是 20，早期预览版是 19
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &v, sizeof(v))))
        DwmSetWindowAttribute(hwnd, 19, &v, sizeof(v));
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            st = static_cast<DialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

            st->dpi  = dpiOf(hwnd);
            st->dark = darkmode::isDark();
            layout(*st);

            makeFonts(*st);

            RECT wrc{0, 0, st->w, st->h};
            AdjustWindowRectEx(&wrc, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                               FALSE, WS_EX_DLGMODALFRAME);
            // 按实际 DPI 精修尺寸，并重新居中
            const int ww = wrc.right - wrc.left, wh = wrc.bottom - wrc.top;
            const int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
            const int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;
            SetWindowPos(hwnd, nullptr, sx, sy, ww, wh, SWP_NOZORDER);

            setDarkTitleBar(hwnd, st->dark);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;   // 全部自绘，不让系统刷背景

        case WM_PAINT:
            if (st) paint(hwnd, *st);
            else {
                PAINTSTRUCT ps{};
                BeginPaint(hwnd, &ps);
                EndPaint(hwnd, &ps);
            }
            return 0;

        case WM_DPICHANGED: {
            // 拖到另一块缩放不同的显示器，或系统改了缩放：全部按新 DPI 重算
            if (!st) return 0;
            st->dpi = static_cast<int>(LOWORD(wp));
            makeFonts(*st);
            layout(*st);
            const RECT* sug = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_SETTINGCHANGE:
            if (st && lp != 0 &&
                lstrcmpW(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) {
                st->dark = darkmode::isDark();
                setDarkTitleBar(hwnd, st->dark);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (!st) return 0;
            {
                const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                if (inRect(st->btnApply, x, y)) {
                    st->ok = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (inRect(st->btnCancel, x, y)) {
                    st->ok = false;
                    DestroyWindow(hwnd);
                    return 0;
                }
                const int band = s(st->dpi, 22);
                if (y >= st->trackY - band && y <= st->trackY + band) {
                    st->dragging = true;
                    SetCapture(hwnd);
                    applyValue(*st, valueFromX(*st, x));
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;

        case WM_MOUSEMOVE:
            if (!st) return 0;
            {
                const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
                if (st->dragging) {
                    applyValue(*st, valueFromX(*st, x));
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                const bool ha = inRect(st->btnApply, x, y);
                const bool hc = inRect(st->btnCancel, x, y);
                if (ha != st->hotApply || hc != st->hotCancel) {
                    st->hotApply = ha;
                    st->hotCancel = hc;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            return 0;

        case WM_MOUSELEAVE:
            if (st && (st->hotApply || st->hotCancel)) {
                st->hotApply = st->hotCancel = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (st && st->dragging) {
                st->dragging = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_KEYDOWN:
            if (!st) return 0;
            {
                const int range = st->spec->maxValue - st->spec->minValue;
                const int step = st->spec->step > 0 ? st->spec->step
                                                    : (range > 100 ? range / 100 : 1);
                if (wp == VK_LEFT || wp == VK_DOWN) {
                    applyValue(*st, st->value - step);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wp == VK_RIGHT || wp == VK_UP) {
                    applyValue(*st, st->value + step);
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wp == VK_RETURN) { st->ok = true;  DestroyWindow(hwnd); return 0; }
                if (wp == VK_ESCAPE) { st->ok = false; DestroyWindow(hwnd); return 0; }
            }
            return 0;

        case WM_CLOSE:
            if (st) st->ok = false;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (st) {
                if (st->fValue) DeleteObject(st->fValue);
                if (st->fGrade) DeleteObject(st->fGrade);
                if (st->fSmall) DeleteObject(st->fSmall);
                st->fValue = st->fGrade = st->fSmall = nullptr;
            }
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int showSliderDialog(const SliderDialogSpec& spec, int currentValue) {
    DialogState st;
    st.spec = &spec;
    // 超出范围夹到边界，不要退回最小值 ——
    // 老配置里可能存着更大的值，退回最小会让用户莫名变成最低延迟。
    st.value = currentValue;
    if (st.value < spec.minValue) st.value = spec.minValue;
    if (st.value > spec.maxValue) st.value = spec.maxValue;

    const wchar_t* CLASS = L"AirPlaySliderDialog";
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hbrBackground = nullptr;   // WM_ERASEBKGND 自己处理
        wc.lpszClassName = CLASS;
        if (!RegisterClassExW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
        }
        registered = true;
    }

    // 先按 96dpi 估一版窗口尺寸，WM_CREATE 里再按实际 DPI 精修
    const int W = 460, H = 292;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    std::wstring title = utf8ToWide(spec.title);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        CLASS, title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        (sw - W) / 2, (sh - H) / 2, W, H,
        nullptr, nullptr, hInst, &st);
    if (!hwnd) return -1;

    darkmode::applyToWindow(hwnd, darkmode::isDark());
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return st.ok ? st.value : -1;
}

// ---- 各调节项的评价文案 ----

namespace {
const char* latencyGrade(int ms) {
    // 分界依维护者的实测：
    //   105ms 零星爆音 / 110ms 基本不爆 / 150ms 完全流畅 / 200ms 很难听到
    if (ms < 110) return i18n::tr("可能爆音");
    if (ms < 150) return i18n::tr("偶有爆音");
    if (ms < 200) return i18n::tr("稳定");
    return i18n::tr("推荐");
}
const char* volumeGrade(int pct) {
    if (pct == 0)  return i18n::tr("静音");
    if (pct < 30)  return i18n::tr("很轻");
    if (pct < 60)  return i18n::tr("适中");
    if (pct < 85)  return i18n::tr("较响");
    return i18n::tr("很响");
}
// 待机时长评价，参数是十分之一秒（50 = 5.0 秒）。
const char* standbyGrade(int tenths) {
    if (tenths <= 0)   return i18n::tr("关闭");
    if (tenths < 75)  return i18n::tr("偏激进");
    if (tenths < 125) return i18n::tr("适中");
    if (tenths < 175) return i18n::tr("较保守");
    return i18n::tr("最保守");
}
} // namespace

int showLatencyDialog(int currentMs) {
    SliderDialogSpec spec;
    spec.title = i18n::tr("AirPlay Bridge — 延迟调节");
    spec.hint = i18n::tr(
        "拖到不爆音的最低位置就是最优解。低于 150ms 可能出现爆音，越低延迟越小，"
        "但接收端缓冲不足会爆音或卡顿。");
    // 范围比档位宽：低端放到 60ms 让用户自己探边界（档位表最低仍是 150ms ——
    // 那是「推荐值」，滑块是「实验区间」，两者职责不同），
    // 高端给到 400ms 以容纳网络条件差、需要更大缓冲的场景。
    spec.minValue = 100;
    spec.maxValue = 250;
    spec.tickFreq = 0;
    spec.unit = "ms";
    spec.grade = latencyGrade;
    return showSliderDialog(spec, currentMs);
}

const char* qualityLevelName(int q) {
    // 档位按实测曲线（19.9kHz 信噪比）划分：
    //   <10%   -> 15-25 dB，失方可闻
    //   10-29% -> 25-33 dB
    //   30-54% -> 33-38 dB
    //   55-79% -> 接近透明
    //   >=80%  -> 39.5 dB（到顶）
    //
    // 最低档不叫「关闭」—— 它并不是"不转换"，只是用的算法更省 CPU。
    // 命名沿用音频软件惯例（SoX 用 quick/low/medium/high，
    // Audacity 用 Low/Medium/High/Best）：按快慢和质量命名，
    // 不用「关」这种会被误解成"没声音"的词。
    if (q < 10)  return i18n::tr("快速");
    if (q < 30)  return i18n::tr("标准");
    if (q < 55)  return i18n::tr("高");
    if (q < 80)  return i18n::tr("极高");
    return i18n::tr("至臻");
}

int showQualityDialog(int currentLevel) {
    SliderDialogSpec spec;
    spec.title = i18n::tr("AirPlay Bridge — 音质");
    spec.hint = i18n::tr("声卡通常跑 48kHz，推给设备前要先转到 44.1kHz。"
                         "越往右高频越干净，CPU 也越贵。");
    spec.minValue = 0;
    spec.maxValue = 100;
    spec.tickFreq = 0;
    spec.unit = "";   // 不显示百分比 —— 用户看档位名就够了
    spec.grade = qualityLevelName;
    return showSliderDialog(spec, currentLevel);
}

int showVolumeDialog(int currentPct) {
    SliderDialogSpec spec;
    spec.title = i18n::tr("AirPlay Bridge — 音量调节");
    spec.hint = i18n::tr("调节设备的播放音量，不影响电脑本身的音量。");
    spec.minValue = 0;
    spec.maxValue = 100;
    spec.tickFreq = 0;
    spec.unit = "%";
    spec.grade = volumeGrade;
    return showSliderDialog(spec, currentPct);
}

int showStandbyDialog(int currentTenths) {
    SliderDialogSpec spec;
    spec.title = i18n::tr("AirPlay Bridge — 无声音时待机");
    spec.hint = i18n::tr("连续静音这么久之后断开与设备的连接，让屏幕熄灭；"
                         "再有任何声音立刻重连。档位太短会在换歌、"
                         "安静段落里误断，太长则熄屏迟钝。"
                         "拖到最左边（0 秒）表示关掉这个功能。");
    spec.minValue = 0;     // 0 = 关闭（不参与静音计时）
    spec.maxValue = 200;   // 20.0 秒
    spec.tickFreq = 0;
    spec.unit = i18n::tr("秒");
    spec.displayDivisor = 10;   // 内部十分之一秒 -> 显示秒
    spec.displayDecimals = 1;   // "7.5 秒"
    spec.step = 1;              // 0.1 秒（内部最小单位）—— 无极调节
    spec.grade = standbyGrade;
    return showSliderDialog(spec, currentTenths);
}

} // namespace fxchain
