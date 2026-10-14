// SPDX-License-Identifier: Apache-2.0
//
// latency_dialog.cpp -- 通用数值调节滑块窗口。
//
// 用最朴素的 Win32 控件：trackbar + 静态文本 + 两个按钮，不引框架。
// 延迟和音量共用这套实现。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "latency_dialog.h"

#include <windows.h>
#include <commctrl.h>

#include <cstdio>
#include <cwchar>

#pragma comment(lib, "comctl32.lib")

namespace fxchain {
namespace {

constexpr int ID_SLIDER = 1001;
constexpr int ID_LABEL  = 1002;
constexpr int ID_HINT   = 1003;
constexpr int ID_OK     = 1004;
constexpr int ID_CANCEL = 1005;

struct DialogState {
    const SliderDialogSpec* spec = nullptr;
    int  value = 0;
    HWND slider = nullptr;
    HWND label  = nullptr;
    bool ok = false;
};

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    return out;
}

void updateLabel(DialogState* st) {
    const SliderDialogSpec* sp = st->spec;
    std::wstring text = std::to_wstring(st->value) + L" " + utf8ToWide(sp->unit);

    if (sp->grade) {
        const char* g = sp->grade(st->value);
        if (g && *g) text += L"   " + utf8ToWide(g);
    }
    SetWindowTextW(st->label, text.c_str());
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            st = static_cast<DialogState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));

            HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            const SliderDialogSpec* sp = st->spec;

            std::wstring hint = utf8ToWide(sp->hint);
            HWND hintWnd = CreateWindowW(L"STATIC", hint.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                16, 12, 400, 40, hwnd,
                reinterpret_cast<HMENU>(ID_HINT), nullptr, nullptr);
            SendMessageW(hintWnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            st->label = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                      16, 56, 400, 26, hwnd,
                                      reinterpret_cast<HMENU>(ID_LABEL), nullptr, nullptr);
            SendMessageW(st->label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            st->slider = CreateWindowW(TRACKBAR_CLASSW, L"",
                WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
                16, 88, 400, 40, hwnd,
                reinterpret_cast<HMENU>(ID_SLIDER), nullptr, nullptr);
            SendMessageW(st->slider, TBM_SETRANGE, TRUE,
                         MAKELPARAM(sp->minValue, sp->maxValue));
            if (sp->tickFreq > 0)
                SendMessageW(st->slider, TBM_SETTICFREQ, sp->tickFreq, 0);
            SendMessageW(st->slider, TBM_SETPOS, TRUE, st->value);

            HWND okBtn = CreateWindowW(L"BUTTON", L"应用",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                240, 140, 84, 30, hwnd,
                reinterpret_cast<HMENU>(ID_OK), nullptr, nullptr);
            SendMessageW(okBtn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            HWND cancelBtn = CreateWindowW(L"BUTTON", L"取消",
                WS_CHILD | WS_VISIBLE,
                332, 140, 84, 30, hwnd,
                reinterpret_cast<HMENU>(ID_CANCEL), nullptr, nullptr);
            SendMessageW(cancelBtn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

            updateLabel(st);
            return 0;
        }

        case WM_HSCROLL:
            if (st && reinterpret_cast<HWND>(lp) == st->slider) {
                st->value = static_cast<int>(SendMessageW(st->slider, TBM_GETPOS, 0, 0));
                updateLabel(st);
            }
            return 0;

        case WM_COMMAND:
            if (!st) return 0;
            switch (LOWORD(wp)) {
                case ID_OK:
                    st->value = static_cast<int>(SendMessageW(st->slider, TBM_GETPOS, 0, 0));
                    st->ok = true;
                    DestroyWindow(hwnd);
                    return 0;
                case ID_CANCEL:
                    st->ok = false;
                    DestroyWindow(hwnd);
                    return 0;
                default:
                    break;
            }
            return 0;

        case WM_CLOSE:
            if (st) st->ok = false;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int showSliderDialog(const SliderDialogSpec& spec, int currentValue) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    DialogState st;
    st.spec = &spec;
    st.value = (currentValue >= spec.minValue && currentValue <= spec.maxValue)
                   ? currentValue : spec.minValue;

    const wchar_t* CLASS = L"AirPlaySliderDialog";
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = CLASS;
        if (!RegisterClassExW(&wc)) {
            if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
        }
        registered = true;
    }

    const int W = 448, H = 220;
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

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_QUIT) break;
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return st.ok ? st.value : -1;
}

// ---- 延迟调节的预设 ----

namespace {
const char* latencyGrade(int ms) {
    if (ms < 120) return "极低（可能爆音）";
    if (ms < 200) return "低（推荐）";
    if (ms < 350) return "中";
    if (ms < 600) return "较高（稳）";
    return "高（最稳）";
}
const char* volumeGrade(int pct) {
    if (pct == 0)  return "静音";
    if (pct < 30)  return "很轻";
    if (pct < 60)  return "适中";
    if (pct < 85)  return "较响";
    return "很响";
}
} // namespace

int showLatencyDialog(int currentMs) {
    SliderDialogSpec spec;
    spec.title = "AirPlay Bridge — 延迟调节";
    spec.hint = "拖到不爆音的最低位置就是最优解。\n"
                "越低延迟越小，但接收端缓冲不足会爆音/卡顿。";
    spec.minValue = 60;
    spec.maxValue = 1000;
    spec.tickFreq = 60;
    spec.unit = "ms";
    spec.grade = latencyGrade;
    return showSliderDialog(spec, currentMs);
}



const char* qualityLevelName(int q) {
    // 档位划分依据实测曲线（19.9kHz 信噪比）：
    //   <20%   -> 20-30 dB，失方可闻
    //   20-44% -> 30-38 dB，明显改善
    //   45-74% -> 38-39.5 dB，接近透明
    //   >=75%  -> 39.5 dB（到顶）
    if (q <= 0)  return "关闭";
    if (q < 20)  return "标准";
    if (q < 45)  return "高";
    if (q < 75)  return "极高";
    return "至臻";
}

int showQualityDialog(int currentLevel) {
    SliderDialogSpec spec;
    spec.title = "AirPlay Bridge — 音质";
    spec.hint = "声卡通常跑 48kHz，推给 HomePod 要先转到 44.1kHz。\n这里决定转换的质量：越往右高频越干净，CPU 也越贵。";
    spec.minValue = 0;
    spec.maxValue = 100;
    spec.tickFreq = 10;
    spec.unit = "";   // 不显示百分比 —— 用户看档位名就够了
    spec.grade = qualityLevelName;
    return showSliderDialog(spec, currentLevel);
}

int showVolumeDialog(int currentPct) {
    SliderDialogSpec spec;
    spec.title = "AirPlay Bridge — 音量调节";
    spec.hint = "调节 HomePod 的播放音量（不影响电脑本身的音量）。";
    spec.minValue = 0;
    spec.maxValue = 100;
    spec.tickFreq = 10;
    spec.unit = "%";
    spec.grade = volumeGrade;
    return showSliderDialog(spec, currentPct);
}

} // namespace fxchain
