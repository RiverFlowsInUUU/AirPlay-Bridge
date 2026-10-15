#pragma once

// 跟随系统深浅色主题。
//
// Win32 没有官方的「菜单深色模式」API。这里用的是 uxtheme.dll 里那组
// 未公开序号 —— Windows 自己的终端、记事本，以及几乎所有第三方程序
// 都走这条路。
//
// 取不到序号时全部静默跳过，最坏结果是菜单保持浅色，不会崩。

#include <windows.h>

namespace darkmode {

// SetPreferredAppMode 的参数，按未公开约定
enum class PreferredAppMode : int {
    Default = 0,
    AllowDark = 1,
    ForceDark = 2,
    ForceLight = 3,
    Max = 4,
};

constexpr int kOrdAllowDarkModeForWindow = 133;
constexpr int kOrdSetPreferredAppMode    = 135;
constexpr int kOrdFlushMenuThemes        = 136;

inline HMODULE uxtheme() {
    static HMODULE h = LoadLibraryExW(L"uxtheme.dll", nullptr,
                                      LOAD_LIBRARY_SEARCH_SYSTEM32);
    return h;
}

// 进程级开关，必须在创建任何窗口之前调用。
inline void init() {
    HMODULE ux = uxtheme();
    if (!ux) return;
    using Fn = PreferredAppMode(WINAPI*)(PreferredAppMode);
    auto fn = reinterpret_cast<Fn>(
        GetProcAddress(ux, MAKEINTRESOURCEA(kOrdSetPreferredAppMode)));
    if (fn) fn(PreferredAppMode::AllowDark);
}

// 主题切换后要清一次菜单主题缓存，否则已经画过的菜单还是旧配色。
inline void flushMenuThemes() {
    HMODULE ux = uxtheme();
    if (!ux) return;
    using Fn = void(WINAPI*)();
    auto fn = reinterpret_cast<Fn>(
        GetProcAddress(ux, MAKEINTRESOURCEA(kOrdFlushMenuThemes)));
    if (fn) fn();
}

// 让这个窗口（以及它弹出的菜单）按系统主题走。
// dark 为 false 时恢复默认主题。
inline void applyToWindow(HWND hwnd, bool dark) {
    HMODULE ux = uxtheme();
    if (!ux) return;

    using AllowFn = BOOL(WINAPI*)(HWND, BOOL);
    auto allow = reinterpret_cast<AllowFn>(
        GetProcAddress(ux, MAKEINTRESOURCEA(kOrdAllowDarkModeForWindow)));
    if (allow) allow(hwnd, dark ? TRUE : FALSE);

    using ThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    auto theme = reinterpret_cast<ThemeFn>(GetProcAddress(ux, "SetWindowTheme"));
    if (theme) {
        if (dark) theme(hwnd, L"DarkMode_Explorer", nullptr);
        else      theme(hwnd, L"", L"");   // 空串 = 撤掉自定义主题
    }
}

// 当前是不是浅色主题。读注册表，读不到按浅色处理。
inline bool usesLightTheme() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD value = 1, size = sizeof(value), type = 0;
    const LONG rc = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                     reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    return rc != ERROR_SUCCESS || value != 0;
}

inline bool isDark() { return !usesLightTheme(); }

}  // namespace darkmode
