#include "win32/WindowTarget.h"

namespace WinEase::Win32 {

QPoint cursorPosition()
{
    POINT point{ 0, 0 };
    if (::GetCursorPos(&point) == FALSE) {
        return QPoint();
    }
    return QPoint(point.x, point.y);
}

bool isAcceptableTarget(WindowHandle hwnd)
{
    if (!isManageableWindow(hwnd)) {
        return false;
    }
    // 最小化窗口的窗口矩形是"还原后"的位置，对它做置顶/分屏用户看不到效果，
    // 且很容易被误判成"功能坏了"，因此直接排除
    return !isWindowMinimized(hwnd);
}

WindowHandle resolveWindowTarget(WindowTargetMode mode, WindowHandle lockedWindow)
{
    if (mode == WindowTargetMode::LockedWindow) {
        return isAcceptableTarget(lockedWindow) ? lockedWindow : nullptr;
    }

    const WindowHandle underCursor = topLevelWindowAt(cursorPosition());
    if (isAcceptableTarget(underCursor)) {
        return underCursor;
    }

    // 光标压在 WinEase 自己界面 / 桌面 / 任务栏上时的回退：
    // 用前台窗口，且仍需过硬性过滤（桌面会被 isShellWindow 挡掉，返回 nullptr）
    const WindowHandle foreground = foregroundWindow();
    return isAcceptableTarget(foreground) ? foreground : nullptr;
}

QString targetModeKey(WindowTargetMode mode)
{
    switch (mode) {
    case WindowTargetMode::LockedWindow:
        return QStringLiteral("locked_window");
    case WindowTargetMode::FollowCursor:
        break;
    }
    return QStringLiteral("follow_cursor");
}

WindowTargetMode targetModeFromKey(const QString &key, WindowTargetMode fallback)
{
    if (key == QStringLiteral("follow_cursor")) {
        return WindowTargetMode::FollowCursor;
    }
    if (key == QStringLiteral("locked_window")) {
        return WindowTargetMode::LockedWindow;
    }
    return fallback;
}

} // namespace WinEase::Win32
