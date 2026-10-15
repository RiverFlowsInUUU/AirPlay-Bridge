// SPDX-License-Identifier: Apache-2.0
//
// tray.h -- 系统托盘图标 + 右键菜单 + 气泡通知。
//
// 纯 Win32（Shell_NotifyIcon），不依赖任何 GUI 框架。
// 用法：主循环里反复调用 pump()，它把用户交互翻译成 TrayEvent 回调。

#pragma once

#include <functional>
#include <string>
#include <vector>

namespace fxchain {

enum class TrayEvent {
    LeftClick,     // 左键单击（通常用于"显示状态"）
    RightClick,    // 右键单击（弹出菜单）
    DoubleClick,   // 左键双击
};

using TrayEventHandler = std::function<void(TrayEvent)>;

struct TrayMenuItem {
    std::string label;         // 显示文字
    int  id = 0;               // 回传的标识
    bool enabled = true;
    bool separator = false;    // true = 画分隔线，label/id 忽略
    bool checked = false;      // 显示选中标记
    // 左侧彩色圆点：0=无，1=绿（已连接），2=灰（未连接）
    // 原生菜单不能给文字上色，只能靠位图做视觉指示。
    int  dot = 0;
    // 非空即表示这是一项可展开的子菜单（会有个右箭头）。
    // 注意：内容必须在 showMenu 之前就确定 —— Windows 的菜单树
    // 是弹出前一次性建好的，没法在展开时再去算。
    std::vector<TrayMenuItem> children;
};

class TrayIcon {
public:
    TrayIcon();
    ~TrayIcon();

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    // 创建托盘图标。tooltip 是鼠标悬停提示。
    bool create(const std::string& tooltip);

    void setTooltip(const std::string& tooltip);
    void setConnected(bool connected);   // 绿点 / 灰点
    void notify(const std::string& title, const std::string& text);
    void destroy();
    bool valid() const;

    // 主循环调用：处理窗口消息，把交互事件抛给 handler
    void pump(const TrayEventHandler& handler);

    // 弹出菜单（通常在收到 RightClick 时调用）。选中项通过 cb 回传。
    void showMenu(const std::vector<TrayMenuItem>& menu, const std::function<void(int)>& cb);

    // 菜单打开期间的心跳。
    //
    // Windows 的 TrackPopupMenu 会跑它自己的模态消息循环，主循环在这段
    // 时间里完全停摆。而音频的 socket IO 是靠主循环里的 pump 推的 ——
    // 于是菜单一弹出来，声音就断了。
    //
    // 这里注册一个回调：菜单开着的时候用定时器继续推，菜单关掉就停。
    // 定时器消息能进得来，是因为 TrackPopupMenu 的循环会把消息派发给
    // 菜单的宿主窗口（也就是托盘那个隐藏窗口）。
    void setMenuPump(std::function<void()> pump);

private:
    struct Impl;
    Impl* d_ = nullptr;
};

} // namespace fxchain
